#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <memory>

namespace videovault::app {

// One ephemeral certificate for a share session. The phone pins its SHA-256,
// which is carried in the QR code, so a different machine on the network
// cannot present its own certificate.
class ShareTlsIdentity {
public:
    static std::shared_ptr<ShareTlsIdentity> create(
        const QStringList& addresses, QString* fingerprint, QString* error);
    ~ShareTlsIdentity();

    ShareTlsIdentity(const ShareTlsIdentity&) = delete;
    ShareTlsIdentity& operator=(const ShareTlsIdentity&) = delete;

private:
    struct Impl;
    friend class TlsChannel;
    explicit ShareTlsIdentity(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// A TCP connection after the TLS handshake. Reads and writes are plaintext.
class TlsChannel {
public:
    TlsChannel();
    ~TlsChannel();

    TlsChannel(const TlsChannel&) = delete;
    TlsChannel& operator=(const TlsChannel&) = delete;

    bool accept(qintptr descriptor, const std::shared_ptr<ShareTlsIdentity>& identity);
    qint64 write(const char* data, qint64 size);
    qint64 write(const QByteArray& bytes);
    bool waitForBytesWritten(int timeoutMs);
    bool waitForReadyRead(int timeoutMs);
    QByteArray readAll();
    void flush();
    void disconnectFromHost();
    bool waitForDisconnected(int timeoutMs);
    [[nodiscard]] QAbstractSocket::SocketState state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace videovault::app
