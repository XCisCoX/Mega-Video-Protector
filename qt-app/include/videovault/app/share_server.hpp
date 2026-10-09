#pragma once

#include <QString>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>

class QWidget;

namespace videovault::core {
class Vault;
}

namespace videovault::app {

// Serves a vault to a phone on the same network. The QR code carries the
// address and the SHA-256 of this session's TLS certificate. The phone sends
// the vault password only after that certificate matches. Locking the PC
// window does not stop sharing. Stop sharing does.
class ShareServer {
public:
    ShareServer();
    ~ShareServer();

    ShareServer(const ShareServer&) = delete;
    ShareServer& operator=(const ShareServer&) = delete;

    // Listens on every IPv4 interface. `error` is a sentence for a dialog.
    // The vault itself is opened later, when the phone presents the password.
    bool start(const std::filesystem::path& root, QString* error);
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] QStringList hosts() const;
    [[nodiscard]] QString linkFor(const QString& host) const;

    // Called on the UI thread when sharing starts or stops.
    void setOnRunning(std::function<void(bool)> callback);

    // Non-modal. Closing it leaves the listener running.
    void showDialog(QWidget* parent);

    struct State;

private:
    class ListenThread;

    std::shared_ptr<State> state_;
    ListenThread* thread_{nullptr};
    quint16 port_{0};
    QString fingerprint_;
    std::function<void(bool)> onRunning_;
    QWidget* dialog_{nullptr};
};

} // namespace videovault::app
