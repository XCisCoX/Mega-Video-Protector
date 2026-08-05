#pragma once

#include <QMainWindow>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;
class QEvent;
class QObject;
class QTreeWidget;
class QTreeWidgetItem;

template <typename T>
class QFutureWatcher;

namespace videovault::core {
class Vault;
struct ThumbnailInfo;
struct TagInfo;
template <typename T>
class Result;
} // namespace videovault::core

namespace videovault::app {

struct VaultOperationResult;
class PlayerWindow;

// Snapshot of tag data loaded off the UI thread for the tag editor dialog.
struct TagEditorData {
    std::int64_t video_id{0};
    std::vector<core::TagInfo> all_tags;
    std::vector<core::TagInfo> video_tags;
};

// Shared state for a batch operation (import/restore) running on the thread
// pool: per-item progress, the success count, and the first error if any.
// Lives in the .cpp; MainWindow methods take it by shared_ptr.
struct ImportProgress;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

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
    void beginImportFolder();
    void beginImportMany(std::vector<std::filesystem::path> sources);
    void finishImport(std::shared_ptr<ImportProgress> progress);
    void refreshGallery();
    void beginRemoveSelected(std::int64_t video_id = -1);
    void finishRemoveSelected();
    void beginChangePassword();
    void finishChangePassword();
    void openSettings();
    void beginRestoreSelected();
    void finishRestoreSelected(std::shared_ptr<ImportProgress> progress);
    void beginGenerateThumbnail(std::int64_t video_id = -1);
    void finishGenerateThumbnail();
    void beginPlayback(std::int64_t video_id);
    std::int64_t selectedVideoId() const;
    std::vector<std::int64_t> selectedVideoIds() const;
    void setViewMode(int index);
    void refreshTagFilter();
    void beginEditTags(std::int64_t video_id);
    void finishEditTags();
    void showGalleryContextMenu(const QPoint& position);
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
    QPushButton* importFolderButton_{nullptr};
    QPushButton* settingsButton_{nullptr};
    QComboBox* viewModeCombo_{nullptr};
    QComboBox* tagFilterCombo_{nullptr};
    QLineEdit* searchEdit_{nullptr};
    QStackedWidget* galleryStack_{nullptr};
    QTreeWidget* detailsTree_{nullptr};
    QListWidget* iconList_{nullptr};
    QProgressBar* progressBar_{nullptr};
    PlayerWindow* playerWindow_{nullptr};
    QLabel* statusCountLabel_{nullptr};
    std::int64_t tagFilterId_{-1};
    QString tagFilterName_;
    QString searchText_;
    QTimer* autoLockTimer_{nullptr};
    QFutureWatcher<std::shared_ptr<VaultOperationResult>>* watcher_{nullptr};
    QFutureWatcher<void>* importWatcher_{nullptr};
    QFutureWatcher<std::shared_ptr<core::Result<bool>>>* adminWatcher_{nullptr};
    QFutureWatcher<void>* restoreWatcher_{nullptr};
    QFutureWatcher<std::shared_ptr<core::Result<core::ThumbnailInfo>>>* thumbnailWatcher_{nullptr};
    QFutureWatcher<std::shared_ptr<TagEditorData>>* tagEditorWatcher_{nullptr};
    // Shared so worker threads can hold the vault alive during import/list.
    std::shared_ptr<core::Vault> vault_;
};

} // namespace videovault::app
