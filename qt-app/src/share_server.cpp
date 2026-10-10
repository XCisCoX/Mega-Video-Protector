#include "videovault/app/share_server.hpp"
#include "videovault/app/share_tls.hpp"
#include "videovault/app/glass.hpp"

#include "qrcodegen.hpp"
#include "videovault/core/vault.hpp"

#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QShowEvent>
#include <QRandomGenerator>
#include <QSemaphore>
#include <QTcpServer>
#include <QThread>
#include <QRunnable>
#include <QThreadPool>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace videovault::app {
namespace {

constexpr int kMaxReadBytes = 1 << 20;

QString jsonEscape(const QString& text) {
    QString out;
    out.reserve(text.size() + 8);
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        switch (code) {
            case '%': out += QStringLiteral("%%"); break;
            case '"': out += QStringLiteral("\\\""); break;
            case '\\': out += QStringLiteral("\\\\"); break;
            case '\n': out += QStringLiteral("\\n"); break;
            case '\r': out += QStringLiteral("\\r"); break;
            case '\t': out += QStringLiteral("\\t"); break;
            default:
                if (code < 0x20) {
                    out += QStringLiteral("\\u%1").arg(code, 4, 16, QLatin1Char('0'));
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

QString jsonEscape(const std::string& text) {
    return jsonEscape(QString::fromUtf8(text.data(), static_cast<int>(text.size())));
}

int hostRank(const QString& ip) {
    if (ip.startsWith(QStringLiteral("192.168."))) return 0;
    if (ip.startsWith(QStringLiteral("10."))) return 1;
    if (ip.startsWith(QStringLiteral("172."))) return 2;
    return 3;
}

QStringList lanHosts() {
    QStringList hosts;
    const auto addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress& address : addresses) {
        if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()) {
            continue;
        }
        hosts.append(address.toString());
    }
    std::sort(hosts.begin(), hosts.end(), [](const QString& a, const QString& b) {
        const int rank = hostRank(a) - hostRank(b);
        return rank != 0 ? rank < 0 : a < b;
    });
    hosts.removeDuplicates();
    return hosts;
}

class ReplyWriter {
public:
    static void send(TlsChannel* socket, int code, const char* reason,
        const QByteArray& type, const QByteArray& body) {
        QByteArray head;
        head += "HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n";
        head += "Content-Type: " + type + "\r\n";
        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        head += "Connection: close\r\n\r\n";
        socket->write(head);
        if (!body.isEmpty()) socket->write(body);
        socket->waitForBytesWritten(20000);
    }
};

struct Request {
    QByteArray method;
    QString path;
    QUrlQuery query;
    QByteArray authorization;
    QByteArray range;
    QByteArray body;
};

void wipe(QByteArray* bytes) {
    if (bytes == nullptr || bytes->isEmpty()) return;
    bytes->fill('\0');
    bytes->clear();
}

void wipe(std::string* text) {
    if (text == nullptr || text->empty()) return;
    std::fill(text->begin(), text->end(), '\0');
    text->clear();
}

bool writeRaw(TlsChannel* socket, const char* data, int size) {
    int off = 0;
    while (off < size) {
        const qint64 wrote = socket->write(data + off, size - off);
        if (wrote <= 0) return false;
        off += static_cast<int>(wrote);
        if (!socket->waitForBytesWritten(20000)) return false;
    }
    return true;
}

bool readRequest(TlsChannel* socket, Request* request) {
    QByteArray raw;
    while (!raw.contains("\r\n\r\n")) {
        if (!socket->waitForReadyRead(8000)) return false;
        raw += socket->readAll();
        if (raw.size() > 65536) return false;
    }
    const int end = raw.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = raw.left(end).split('\n');
    if (lines.isEmpty()) return false;
    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() < 2) return false;
    request->method = parts.first().toUpper();
    if (request->method != "GET" && request->method != "POST") return false;
    const QByteArray target = parts.at(1);
    const int queryAt = target.indexOf('?');
    request->path = QString::fromUtf8(queryAt < 0 ? target : target.left(queryAt));
    if (queryAt >= 0) {
        request->query = QUrlQuery(QString::fromUtf8(target.mid(queryAt + 1)));
    }
    int contentLength = -1;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const QByteArray lower = line.toLower();
        if (lower.startsWith("authorization:")) {
            request->authorization = line.mid(int(sizeof("authorization:") - 1)).trimmed();
        } else if (lower.startsWith("range:")) {
            request->range = line.mid(int(sizeof("range:") - 1)).trimmed();
        } else if (lower.startsWith("content-length:")) {
            bool ok = false;
            contentLength = line.mid(int(sizeof("content-length:") - 1)).trimmed().toInt(&ok);
            if (!ok) return false;
        }
    }
    QByteArray body = raw.mid(end + 4);
    if (contentLength > 2048) return false;
    if (contentLength > 0) {
        while (body.size() < contentLength) {
            if (!socket->waitForReadyRead(8000)) return false;
            body += socket->readAll();
            if (body.size() > 4096) return false;
        }
        body.truncate(contentLength);
    } else {
        body.clear();
    }
    request->body = body;
    return true;
}

bool authorized(const Request& request, const QByteArray& tokenHex) {
    if (tokenHex.isEmpty()) return false;
    const QByteArray prefix = "Bearer ";
    if (!request.authorization.startsWith(prefix)) return false;
    const QByteArray got = request.authorization.mid(prefix.size()).trimmed().toLower();
    if (got.size() != tokenHex.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < got.size(); ++i) {
        diff |= static_cast<unsigned char>(got.at(i) ^ tokenHex.at(i));
    }
    return diff == 0;
}

qlonglong queryId(const Request& request) {
    bool ok = false;
    const qlonglong id = request.query.queryItemValue(QStringLiteral("id")).toLongLong(&ok);
    return ok ? id : -1;
}

QByteArray videosJson(core::Vault& vault) {
    auto list = vault.list_videos();
    if (!list) return {};
    QString out = QStringLiteral("[");
    bool first = true;
    for (const auto& video : list.value()) {
        if (!first) out += QLatin1Char(',');
        first = false;
        QString tags = QStringLiteral("[");
        bool tagFirst = true;
        for (const auto& tag : video.tags) {
            if (!tagFirst) tags += QLatin1Char(',');
            tagFirst = false;
            tags += QLatin1Char('"') + jsonEscape(tag) + QLatin1Char('"');
        }
        tags += QLatin1Char(']');
        out += QStringLiteral("{\"id\":%1,\"name\":\"%2\",\"size\":%3,\"importedAt\":%4,\"folderId\":%5,\"tags\":%6}")
            .arg(video.id)
            .arg(jsonEscape(video.display_name))
            .arg(static_cast<qulonglong>(video.original_size))
            .arg(static_cast<qulonglong>(video.imported_at))
            .arg(static_cast<qlonglong>(video.folder_id))
            .arg(tags);
    }
    out += QLatin1Char(']');
    return out.toUtf8();
}

QByteArray foldersJson(core::Vault& vault) {
    auto list = vault.list_folders();
    if (!list) return {};
    QString out = QStringLiteral("[");
    bool first = true;
    for (const auto& folder : list.value()) {
        if (!first) out += QLatin1Char(',');
        first = false;
        out += QStringLiteral("{\"id\":%1,\"parentId\":%2,\"name\":\"%3\"}")
            .arg(folder.id)
            .arg(folder.parent_id)
            .arg(jsonEscape(folder.name));
    }
    out += QLatin1Char(']');
    return out.toUtf8();
}

QByteArray tagsJson(core::Vault& vault) {
    auto list = vault.list_tags();
    if (!list) return {};
    QString out = QStringLiteral("[");
    bool first = true;
    for (const auto& tag : list.value()) {
        if (!first) out += QLatin1Char(',');
        first = false;
        out += QStringLiteral("{\"id\":%1,\"name\":\"%2\",\"count\":%3}")
            .arg(tag.id)
            .arg(jsonEscape(tag.name))
            .arg(tag.video_count);
    }
    out += QLatin1Char(']');
    return out.toUtf8();
}

void handleClient(TlsChannel* socket, const std::shared_ptr<ShareServer::State>& state);

} // namespace

struct ShareServer::State {
    std::filesystem::path root;
    std::shared_ptr<ShareTlsIdentity> tls;
    std::mutex gate;
    std::shared_ptr<core::Vault> vault;
    QByteArray tokenHex;
    std::atomic<bool> accepting{true};
};

class ShareServer::ListenThread : public QThread {
public:
    ListenThread(std::shared_ptr<State> state, QThreadPool* pool)
        : state_(std::move(state)), pool_(pool) {}

    QString error;
    quint16 port{0};
    QSemaphore ready{0};

protected:
    void run() override;

private:
    class Server : public QTcpServer {
    public:
        Server(std::shared_ptr<State> state, QThreadPool* pool)
            : state_(std::move(state)), pool_(pool) {}

    protected:
        void incomingConnection(qintptr handle) override {
            if (!state_->accepting.load()) return;
            auto* task = new ClientTask(handle, state_);
            pool_->start(task);
        }

        struct ClientTask : QRunnable {
            ClientTask(qintptr handle, std::shared_ptr<State> state)
                : handle_(handle), state_(std::move(state)) {}
            void run() override {
                TlsChannel channel;
                if (!channel.accept(handle_, state_->tls)) return;
                try {
                    handleClient(&channel, state_);
                } catch (...) {
                }
                channel.flush();
                channel.waitForBytesWritten(20000);
                if (channel.state() != QAbstractSocket::UnconnectedState) {
                    channel.disconnectFromHost();
                    if (channel.state() != QAbstractSocket::UnconnectedState) {
                        channel.waitForDisconnected(15000);
                    }
                }
            }
            qintptr handle_;
            std::shared_ptr<State> state_;
        };

        std::shared_ptr<State> state_;
        QThreadPool* pool_;
    };

    std::shared_ptr<State> state_;
    QThreadPool* pool_;
};

void ShareServer::ListenThread::run() {
    Server server(state_, pool_);
    if (!server.listen(QHostAddress::AnyIPv4, 0)) {
        error = server.errorString();
        ready.release();
        return;
    }
    port = server.serverPort();
    ready.release();
    exec();
    server.close();
}

namespace {

struct SessionView {
    std::shared_ptr<core::Vault> vault;
    QByteArray tokenHex;
};

SessionView sessionOf(const std::shared_ptr<ShareServer::State>& state) {
    std::lock_guard<std::mutex> lock(state->gate);
    return SessionView{state->vault, state->tokenHex};
}

enum class RangeKind { None, Ok, Bad };

struct ByteRange {
    std::uint64_t start{0};
    std::uint64_t end{0};
};

RangeKind parseRange(const QByteArray& header, std::uint64_t total, ByteRange* out) {
    if (header.isEmpty()) return RangeKind::None;
    if (total == 0) return RangeKind::Bad;
    QByteArray text = header.trimmed();
    if (!text.startsWith("bytes=")) return RangeKind::Bad;
    text = text.mid(int(sizeof("bytes=") - 1));
    const int comma = text.indexOf(',');
    if (comma >= 0) text = text.left(comma).trimmed();
    const int dash = text.indexOf('-');
    if (dash <= 0) return RangeKind::Bad;
    bool startOk = false;
    const auto start = text.left(dash).trimmed().toULongLong(&startOk);
    if (!startOk || start >= total) return RangeKind::Bad;
    std::uint64_t end = total - 1;
    const QByteArray tail = text.mid(dash + 1).trimmed();
    if (!tail.isEmpty()) {
        bool endOk = false;
        end = tail.toULongLong(&endOk);
        if (!endOk || end < start) return RangeKind::Bad;
        if (end >= total) end = total - 1;
    }
    out->start = start;
    out->end = end;
    return RangeKind::Ok;
}

void sendFile(TlsChannel* socket, core::Vault& vault, qlonglong id, const QByteArray& rangeHeader) {
    auto list = vault.list_videos();
    if (!list) {
        ReplyWriter::send(socket, 500, "Error", "text/plain", "list failed");
        return;
    }
    bool found = false;
    std::uint64_t total = 0;
    for (const auto& video : list.value()) {
        if (video.id == id) {
            found = true;
            total = video.original_size;
            break;
        }
    }
    if (!found) {
        ReplyWriter::send(socket, 404, "Not Found", "text/plain", "not found");
        return;
    }
    std::uint64_t start = 0;
    std::uint64_t end = total == 0 ? 0 : total - 1;
    bool partial = false;
    if (total > 0 && !rangeHeader.isEmpty()) {
        ByteRange range;
        const RangeKind kind = parseRange(rangeHeader, total, &range);
        if (kind == RangeKind::Bad) {
            ReplyWriter::send(socket, 416, "Range Not Satisfiable", "text/plain", "bad range");
            return;
        }
        if (kind == RangeKind::Ok) {
            start = range.start;
            end = range.end;
            partial = true;
        }
    }
    const std::uint64_t count = total == 0 ? 0 : (end - start + 1);
    QByteArray head = partial
        ? QByteArray("HTTP/1.1 206 Partial Content\r\n")
        : QByteArray("HTTP/1.1 200 OK\r\n");
    head += "Content-Type: application/octet-stream\r\n";
    head += "Accept-Ranges: bytes\r\n";
    head += "Content-Length: " + QByteArray::number(static_cast<qulonglong>(count)) + "\r\n";
    if (partial) {
        head += "Content-Range: bytes " + QByteArray::number(static_cast<qulonglong>(start))
            + "-" + QByteArray::number(static_cast<qulonglong>(end))
            + "/" + QByteArray::number(static_cast<qulonglong>(total)) + "\r\n";
    }
    head += "Connection: close\r\n\r\n";
    if (!writeRaw(socket, head.constData(), head.size())) return;
    std::uint64_t sent = 0;
    while (sent < count) {
        const auto ask = std::min<std::uint64_t>(256ULL * 1024ULL, count - sent);
        auto bytes = vault.read_video_range(id, start + sent, static_cast<std::size_t>(ask));
        if (!bytes || bytes.value().empty()) return;
        const auto& data = bytes.value();
        const auto n = static_cast<int>(std::min<std::size_t>(
            data.size(), static_cast<std::size_t>(count - sent)));
        if (n <= 0) return;
        if (!writeRaw(socket, reinterpret_cast<const char*>(data.data()), n)) return;
        sent += static_cast<std::uint64_t>(n);
    }
}

void handleLogin(TlsChannel* socket, const std::shared_ptr<ShareServer::State>& state, Request& request) {
    const int bodyBytes = request.body.size();
    std::string password(
        bodyBytes > 0 ? request.body.constData() : "",
        static_cast<std::size_t>(bodyBytes > 0 ? bodyBytes : 0));
    wipe(&request.body);
    const bool empty = password.empty();
    const bool sharing = state->accepting.load();
    if (empty || !sharing) {
        wipe(&password);
        ReplyWriter::send(socket, empty ? 401 : 403,
            empty ? "Unauthorized" : "Forbidden",
            "text/plain",
            empty ? "wrong password" : "sharing stopped");
        return;
    }
    auto opened = core::Vault::open(state->root, password);
    wipe(&password);
    if (!opened) {
        const auto code = opened.error().code;
        if (code == core::VaultErrorCode::WrongPassword
            || code == core::VaultErrorCode::AuthenticationFailed) {
            ReplyWriter::send(socket, 401, "Unauthorized", "text/plain", "wrong password");
        } else {
            ReplyWriter::send(socket, 500, "Error", "text/plain", "could not open the vault");
        }
        return;
    }
    auto fresh = std::make_shared<core::Vault>(std::move(opened).value());
    if (!state->accepting.load()) {
        fresh->lock();
        ReplyWriter::send(socket, 403, "Forbidden", "text/plain", "sharing stopped");
        return;
    }
    quint32 words[8] = {};
    QRandomGenerator::system()->fillRange(words);
    const QByteArray raw(reinterpret_cast<const char*>(words), static_cast<int>(sizeof(words)));
    const QByteArray hex = raw.toHex();
    std::shared_ptr<core::Vault> previous;
    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(state->gate);
        if (!state->accepting.load()) {
            stopped = true;
        } else {
            previous = std::move(state->vault);
            state->vault = fresh;
            state->tokenHex = hex;
        }
    }
    if (stopped) {
        fresh->lock();
        ReplyWriter::send(socket, 403, "Forbidden", "text/plain", "sharing stopped");
        return;
    }
    if (previous) previous->lock();
    ReplyWriter::send(socket, 200, "OK", "application/json",
        QByteArray("{\"ok\":true,\"token\":\"") + hex + "\"}");
}

void handleClient(TlsChannel* socket, const std::shared_ptr<ShareServer::State>& state) {
    Request request;
    if (!readRequest(socket, &request)) {
        ReplyWriter::send(socket, 400, "Bad Request", "text/plain", "bad request");
        return;
    }
    struct BodyGuard {
        QByteArray* body;
        ~BodyGuard() { wipe(body); }
    } guard{&request.body};

    if (!state->accepting.load()) {
        ReplyWriter::send(socket, 403, "Forbidden", "text/plain", "sharing stopped");
        return;
    }
    if (request.method == "POST" && request.path == QStringLiteral("/v1/login")) {
        handleLogin(socket, state, request);
        return;
    }
    if (request.method == "GET" && request.path == QStringLiteral("/v1/ping")) {
        ReplyWriter::send(socket, 200, "OK", "application/json", "{\"ok\":true}");
        return;
    }
    if (request.method != "GET") {
        ReplyWriter::send(socket, 405, "Method Not Allowed", "text/plain", "method");
        return;
    }
    const SessionView session = sessionOf(state);
    if (!authorized(request, session.tokenHex)) {
        ReplyWriter::send(socket, 401, "Unauthorized", "text/plain", "unauthorized");
        return;
    }
    auto vault = session.vault;
    if (!vault || !vault->is_unlocked()) {
        ReplyWriter::send(socket, 403, "Forbidden", "text/plain", "sign in with the vault password");
        return;
    }

    if (request.path == QStringLiteral("/v1/videos")) {
        const QByteArray body = videosJson(*vault);
        if (body.isEmpty()) {
            ReplyWriter::send(socket, 500, "Error", "text/plain", "list failed");
            return;
        }
        ReplyWriter::send(socket, 200, "OK", "application/json", body);
        return;
    }
    if (request.path == QStringLiteral("/v1/folders")) {
        const QByteArray body = foldersJson(*vault);
        if (body.isEmpty()) {
            ReplyWriter::send(socket, 500, "Error", "text/plain", "folders failed");
            return;
        }
        ReplyWriter::send(socket, 200, "OK", "application/json", body);
        return;
    }
    if (request.path == QStringLiteral("/v1/tags")) {
        const QByteArray body = tagsJson(*vault);
        if (body.isEmpty()) {
            ReplyWriter::send(socket, 500, "Error", "text/plain", "tags failed");
            return;
        }
        ReplyWriter::send(socket, 200, "OK", "application/json", body);
        return;
    }

    const qlonglong id = queryId(request);
    if (id < 0) {
        ReplyWriter::send(socket, 400, "Bad Request", "text/plain", "missing id");
        return;
    }

    if (request.path == QStringLiteral("/v1/size")) {
        auto list = vault->list_videos();
        bool found = false;
        std::uint64_t bytes = 0;
        if (list) {
            for (const auto& video : list.value()) {
                if (video.id == id) {
                    found = true;
                    bytes = video.original_size;
                    break;
                }
            }
        }
        if (!found) {
            ReplyWriter::send(socket, 404, "Not Found", "text/plain", "not found");
            return;
        }
        ReplyWriter::send(socket, 200, "OK", "text/plain", QByteArray::number(static_cast<qulonglong>(bytes)));
        return;
    }
    if (request.path == QStringLiteral("/v1/media")) {
        auto info = vault->media_info(id);
        if (!info) {
            ReplyWriter::send(socket, 404, "Not Found", "application/json", "{}");
            return;
        }
        const auto& media = info.value();
        const QByteArray body = QStringLiteral(
            "{\"durationMs\":%1,\"width\":%2,\"height\":%3,\"rotation\":%4,\"codec\":\"%5\",\"title\":\"%6\",\"artist\":\"%7\"}")
            .arg(static_cast<qulonglong>(media.duration_ms))
            .arg(media.width)
            .arg(media.height)
            .arg(media.rotation_degrees)
            .arg(jsonEscape(media.codec_name))
            .arg(jsonEscape(media.title))
            .arg(jsonEscape(media.artist))
            .toUtf8();
        ReplyWriter::send(socket, 200, "OK", "application/json", body);
        return;
    }
    if (request.path == QStringLiteral("/v1/thumbnail")) {
        bool ok = false;
        int maxDimension = request.query.queryItemValue(QStringLiteral("max")).toInt(&ok);
        if (!ok) maxDimension = 320;
        maxDimension = qBound(64, maxDimension, 720);
        auto thumb = vault->display_thumbnail(id, static_cast<std::uint32_t>(maxDimension));
        if (!thumb || thumb.value().bytes.empty()) {
            ReplyWriter::send(socket, 404, "Not Found", "text/plain", "no thumbnail");
            return;
        }
        const auto& bytes = thumb.value().bytes;
        ReplyWriter::send(socket, 200, "OK", "image/jpeg",
            QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())));
        return;
    }
    if (request.path == QStringLiteral("/v1/file")) {
        sendFile(socket, *vault, id, request.range);
        return;
    }
    if (request.path == QStringLiteral("/v1/read")) {
        bool offsetOk = false;
        bool sizeOk = false;
        const qulonglong offset = request.query.queryItemValue(QStringLiteral("offset")).toULongLong(&offsetOk);
        int size = request.query.queryItemValue(QStringLiteral("size")).toInt(&sizeOk);
        if (!offsetOk || !sizeOk || size <= 0) {
            ReplyWriter::send(socket, 400, "Bad Request", "text/plain", "bad range");
            return;
        }
        if (size > kMaxReadBytes) size = kMaxReadBytes;
        auto bytes = vault->read_video_range(id, offset, static_cast<std::size_t>(size));
        if (!bytes) {
            ReplyWriter::send(socket, 404, "Not Found", "text/plain", "read failed");
            return;
        }
        const auto& data = bytes.value();
        const QByteArray payload = data.empty()
            ? QByteArray()
            : QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
        ReplyWriter::send(socket, 200, "OK", "application/octet-stream", payload);
        return;
    }
    ReplyWriter::send(socket, 404, "Not Found", "text/plain", "not found");
}

QImage qrImage(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int modules = code.getSize();
    constexpr int scale = 8;
    constexpr int border = 4;
    const int pixels = (modules + border * 2) * scale;
    QImage image(pixels, pixels, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (code.getModule(x, y)) {
                painter.drawRect((x + border) * scale, (y + border) * scale, scale, scale);
            }
        }
    }
    return image;
}

} // namespace

// Paints the code inside a fixed white card so the pixmap cannot grow over
// the address row underneath it.
class QrCard final : public QWidget {
public:
    explicit QrCard(QWidget* parent = nullptr)
        : QWidget(parent) {
        setFixedSize(320, 320);
    }

    void setCode(const QImage& image) {
        image_ = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRoundedRect(rect(), 18, 18);
        if (image_.isNull()) {
            return;
        }
        const QRect inner = rect().adjusted(22, 22, -22, -22);
        const QImage scaled = image_.scaled(
            inner.size(), Qt::KeepAspectRatio, Qt::FastTransformation);
        painter.setClipRect(inner);
        painter.drawImage(
            QPoint(
                inner.left() + (inner.width() - scaled.width()) / 2,
                inner.top() + (inner.height() - scaled.height()) / 2),
            scaled);
    }

private:
    QImage image_;
};

// Same sheet as Settings: the library stays blurred behind it. Closing the
// sheet leaves sharing running. Stop sharing is the only thing that ends it.
class ShareSheet final : public QDialog {
public:
    explicit ShareSheet(QWidget* parent = nullptr)
        : QDialog(parent) {
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setModal(false);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (!backdrop_.isNull()) {
            painter.drawImage(rect(), backdrop_);
        } else {
            painter.fillRect(rect(), QColor(0, 0, 0));
        }
        painter.fillRect(rect(), QColor(0, 0, 0, 120));
    }

    void showEvent(QShowEvent* event) override {
        QDialog::showEvent(event);
        if (QWidget* host = parentWidget()) {
            const QPoint origin = host->mapToGlobal(QPoint(0, 0));
            setGeometry(QRect(origin, host->size()));
            backdrop_ = frost(host->grab().toImage());
            update();
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (childAt(event->pos()) == nullptr) {
            close();
            return;
        }
        QDialog::mousePressEvent(event);
    }

private:
    QImage backdrop_;
};

ShareServer::ShareServer() = default;

ShareServer::~ShareServer() {
    stop();
}

bool ShareServer::start(const std::filesystem::path& root, QString* error) {
    stop();
    if (root.empty() || !core::Vault::exists(root)) {
        if (error != nullptr) *error = QStringLiteral("Unlock the vault before sharing it.");
        return false;
    }
    QString fingerprint;
    auto tls = ShareTlsIdentity::create(lanHosts(), &fingerprint, error);
    if (!tls) {
        if (error != nullptr && error->isEmpty()) {
            *error = QStringLiteral("Could not start the encrypted connection.");
        }
        return false;
    }

    state_ = std::make_shared<State>();
    state_->root = root;
    state_->tls = std::move(tls);
    state_->accepting.store(true);

    auto* pool = new QThreadPool();
    pool->setMaxThreadCount(4);
    thread_ = new ListenThread(state_, pool);
    // The pool outlives the listen thread's tasks. Parent it to nothing and
    // keep it alive in the thread object via a property... store on the thread.
    pool->setParent(thread_);
    thread_->start();
    thread_->ready.acquire();
    port_ = thread_->port;
    if (port_ == 0) {
        const QString reason = thread_->error.isEmpty()
            ? QStringLiteral("Could not listen on the network.")
            : thread_->error;
        stop();
        if (error != nullptr) *error = reason;
        return false;
    }
    if (onRunning_) onRunning_(true);
    fingerprint_ = fingerprint;
    return true;
}

void ShareServer::stop() {
    const bool was = running();
    if (state_) state_->accepting.store(false);
    if (thread_ != nullptr) {
        thread_->quit();
        thread_->wait(3000);
        // Tasks still inside a decrypt finish or fail once the vault locks.
        if (auto* pool = thread_->findChild<QThreadPool*>()) {
            pool->waitForDone(2000);
        }
        delete thread_;
        thread_ = nullptr;
    }
    if (state_) {
        std::shared_ptr<core::Vault> vault;
        {
            std::lock_guard<std::mutex> lock(state_->gate);
            vault = std::move(state_->vault);
            wipe(&state_->tokenHex);
        }
        if (vault) vault->lock();
    }
    state_.reset();
    port_ = 0;
    fingerprint_.clear();
    if (dialog_ != nullptr) {
        dialog_->close();
        dialog_ = nullptr;
    }
    if (was && onRunning_) onRunning_(false);
}

bool ShareServer::running() const {
    return thread_ != nullptr && port_ != 0;
}

quint16 ShareServer::port() const { return port_; }

QStringList ShareServer::hosts() const { return lanHosts(); }

QString ShareServer::linkFor(const QString& host) const {
    return QStringLiteral("mvpvault://%1:%2?fp=%3").arg(host).arg(port_).arg(fingerprint_);
}

void ShareServer::setOnRunning(std::function<void(bool)> callback) {
    onRunning_ = std::move(callback);
}

void ShareServer::showDialog(QWidget* parent) {
    if (dialog_ != nullptr) {
        dialog_->raise();
        dialog_->activateWindow();
        return;
    }
    auto* dialog = new ShareSheet(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Share"));
    dialog_ = dialog;
    QObject::connect(dialog, &QDialog::destroyed, parent, [this] {
        dialog_ = nullptr;
    });

    auto* outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(48, 48, 48, 48);
    outer->addStretch(1);

    auto* sheet = new QFrame(dialog);
    sheet->setObjectName(QStringLiteral("shareSheet"));
    sheet->setAttribute(Qt::WA_StyledBackground, true);
    sheet->setFixedWidth(440);
    sheet->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);
    sheet->setStyleSheet(QStringLiteral(
        "QFrame#shareSheet { background: #121214; border-radius: 18px; }"
        "QFrame#shareAddress { background: #1c1c1e; border: none; border-radius: 12px; }"
        "QFrame#shareSheet QComboBox {"
        "  background: #1c1c1e; color: white; border: none; border-radius: 12px;"
        "  min-height: 36px; padding: 0 12px; }"
        "QFrame#shareSheet QComboBox QAbstractItemView {"
        "  background: #1c1c1e; color: white; selection-background-color: #3390ec; }"
        "QPushButton#shareDone { background: transparent; border: none; color: #3390ec; min-height: 32px; }"
        "QPushButton#shareCopy { background: transparent; border: none; color: #3390ec; min-height: 32px; padding: 0 4px; }"
        "QPushButton#shareStop { background: #2c1518; border: none; color: #ff6b6b; border-radius: 14px; min-height: 40px; }"));
    auto* layout = new QVBoxLayout(sheet);
    layout->setContentsMargins(22, 16, 22, 18);
    layout->setSpacing(12);

    auto* titleRow = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("Share"), sheet);
    QFont titleFont(QStringLiteral("Segoe UI"));
    titleFont.setPointSize(16);
    title->setFont(titleFont);
    title->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
    auto* done = new QPushButton(QStringLiteral("Done"), sheet);
    done->setObjectName(QStringLiteral("shareDone"));
    done->setCursor(Qt::PointingHandCursor);
    done->setFocusPolicy(Qt::NoFocus);
    QFont doneFont(QStringLiteral("Segoe UI"));
    doneFont.setPointSize(11);
    done->setFont(doneFont);
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    titleRow->addWidget(done);
    layout->addLayout(titleRow);

    auto* intro = new QLabel(QStringLiteral(
        "Scan with Mega Vault Protect on your phone, then enter the vault password. "
        "Use the same Wi-Fi. Locking this PC does not stop sharing."), sheet);
    intro->setWordWrap(true);
    QFont bodyFont(QStringLiteral("Segoe UI"));
    bodyFont.setPointSize(10);
    intro->setFont(bodyFont);
    intro->setStyleSheet(QStringLiteral("color: #8e8e93; background: transparent;"));
    layout->addWidget(intro);

    auto* code = new QrCard(sheet);
    layout->addWidget(code, 0, Qt::AlignHCenter);

    auto* addressBar = new QFrame(sheet);
    addressBar->setObjectName(QStringLiteral("shareAddress"));
    addressBar->setAttribute(Qt::WA_StyledBackground, true);
    addressBar->setFixedHeight(44);
    auto* addressRow = new QHBoxLayout(addressBar);
    addressRow->setContentsMargins(14, 0, 10, 0);
    addressRow->setSpacing(8);
    auto* address = new QLabel(addressBar);
    address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    address->setFont(bodyFont);
    address->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
    auto* copy = new QPushButton(QStringLiteral("Copy"), addressBar);
    copy->setObjectName(QStringLiteral("shareCopy"));
    copy->setCursor(Qt::PointingHandCursor);
    copy->setFocusPolicy(Qt::NoFocus);
    copy->setAutoDefault(false);
    copy->setDefault(false);
    copy->setFont(bodyFont);
    addressRow->addWidget(address, 1);
    addressRow->addWidget(copy);
    layout->addWidget(addressBar);

    const QStringList available = hosts();
    auto* hostBox = new QComboBox(sheet);
    hostBox->setFont(bodyFont);
    if (available.isEmpty()) {
        hostBox->hide();
        code->hide();
        addressBar->hide();
        intro->setText(QStringLiteral(
            "This PC has no network address. Connect it to Wi-Fi, then open Share again."));
    } else {
        hostBox->addItems(available);
        hostBox->setVisible(available.size() > 1);
    }
    layout->addWidget(hostBox);

    auto currentLink = std::make_shared<QString>();
    const auto refresh = [this, code, address, hostBox, currentLink] {
        const QString host = hostBox->currentText();
        if (!running() || host.isEmpty()) {
            code->setCode(QImage());
            address->clear();
            currentLink->clear();
            return;
        }
        *currentLink = linkFor(host);
        code->setCode(qrImage(*currentLink));
        address->setText(QStringLiteral("%1:%2").arg(host).arg(port_));
    };
    QObject::connect(hostBox, &QComboBox::currentTextChanged, dialog, [refresh](const QString&) { refresh(); });
    refresh();

    QObject::connect(copy, &QPushButton::clicked, dialog, [currentLink, copy] {
        if (currentLink->isEmpty()) {
            return;
        }
        QGuiApplication::clipboard()->setText(*currentLink);
        copy->setText(QStringLiteral("Copied"));
    });
    QObject::connect(done, &QPushButton::clicked, dialog, &QDialog::close);

    auto* stop = new QPushButton(QStringLiteral("Stop sharing"), sheet);
    stop->setObjectName(QStringLiteral("shareStop"));
    stop->setCursor(Qt::PointingHandCursor);
    stop->setFocusPolicy(Qt::NoFocus);
    stop->setFont(bodyFont);
    QObject::connect(stop, &QPushButton::clicked, dialog, [this] { this->stop(); });
    layout->addWidget(stop);

    outer->addWidget(sheet, 0, Qt::AlignHCenter);
    outer->addStretch(1);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace videovault::app
