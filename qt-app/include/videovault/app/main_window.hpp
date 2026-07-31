#pragma once

#include <QMainWindow>

#include <filesystem>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QEvent;
class QObject;

template <typename T>
class QFutureWatcher;

namespace videovault::core {
class Vault;
}

namespace videovault::app {

struct VaultOperationResult;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildInterface();
    QWidget* buildSetupPage();
    QWidget* buildLoginPage();
    QWidget* buildUnlockedPage();

    void selectSetupLocation();
    void selectLoginLocation();
    void beginCreate();
    void beginOpen();
    void finishOperation();
    void setBusy(bool busy);
    void showSetup();
    void showLogin(const std::filesystem::path& path);
    void showUnlocked();
    void lockVault();
    void resetAutoLock();
    void setError(QLabel* label, const QString& message);

    QStackedWidget* pages_{nullptr};
    QWidget* setupPage_{nullptr};
    QWidget* loginPage_{nullptr};
    QWidget* unlockedPage_{nullptr};

    QLineEdit* setupLocation_{nullptr};
    QLineEdit* setupPassword_{nullptr};
    QLineEdit* setupConfirmation_{nullptr};
    QComboBox* securityProfile_{nullptr};
    QLabel* setupError_{nullptr};
    QPushButton* createButton_{nullptr};

    QLineEdit* loginLocation_{nullptr};
    QLineEdit* loginPassword_{nullptr};
    QLabel* loginError_{nullptr};
    QPushButton* unlockButton_{nullptr};

    QLabel* unlockedLocation_{nullptr};
    QTimer* autoLockTimer_{nullptr};
    QFutureWatcher<std::shared_ptr<VaultOperationResult>>* watcher_{nullptr};
    std::unique_ptr<core::Vault> vault_;
};

} // namespace videovault::app
