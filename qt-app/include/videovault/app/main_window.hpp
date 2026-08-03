#pragma once

#include <QMainWindow>

#include <cstdint>
#include <filesystem>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTimer;
class QEvent;
class QObject;

template <typename T>
class QFutureWatcher;

namespace videovault::core {
class Vault;
template <typename T>
class Result;
} // namespace videovault::core

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
    void beginImport();
    void finishImport();
    void refreshGallery();
    void beginRemoveSelected();
    void finishRemoveSelected();
    void beginChangePassword();
    void finishChangePassword();
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
    QLabel* galleryStatus_{nullptr};
    QPushButton* importButton_{nullptr};
    QPushButton* removeButton_{nullptr};
    QPushButton* changePasswordButton_{nullptr};
    QListWidget* gallery_{nullptr};
    QTimer* autoLockTimer_{nullptr};
    QFutureWatcher<std::shared_ptr<VaultOperationResult>>* watcher_{nullptr};
    QFutureWatcher<std::shared_ptr<core::Result<std::int64_t>>>* importWatcher_{nullptr};
    QFutureWatcher<std::shared_ptr<core::Result<bool>>>* adminWatcher_{nullptr};
    // Shared so worker threads can hold the vault alive during import/list.
    std::shared_ptr<core::Vault> vault_;
};

} // namespace videovault::app
