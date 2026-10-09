#pragma once

#include <QMainWindow>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

class QAbstractButton;
class QButtonGroup;
class QComboBox;
class QDragEnterEvent;
class QShowEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QThread;
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
// Holds every selected video when the user edits tags on a multi-selection;
// video_tags is the tag set shared by ALL of them (the dialog's checked set).
struct TagEditorData {
    std::vector<std::int64_t> video_ids;
    std::vector<core::TagInfo> all_tags;
    std::vector<core::TagInfo> video_tags;
};

class BatchWorker;
class FrostedBar;
class ShareServer;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;

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
    void beginRestoreSelected();
    void finishBatch(bool ok, const QString& message, int count);
    void refreshGallery();
    void beginRemoveSelected();
    void beginChangePassword();
    void finishChangePassword();
    void openSettings();
    void beginGenerateThumbnail(const std::vector<std::int64_t>& ids = {});
    void finishGenerateThumbnail();
    void beginPlayback(std::int64_t video_id);
    void stepPlayback(int delta);
    void embedPlayer();
    void popPlayer();
    void restoreGalleryFocus();
    std::int64_t selectedVideoId() const;
    std::vector<std::int64_t> selectedVideoIds() const;
    void setViewMode(int index);
    void applyGallerySort(int key);
    void sortGalleryItems();
    void updateSortButton();
    void layoutLibraryChrome();
    void refreshTagFilter();
    void beginEditTags(const std::vector<std::int64_t>& video_ids);
    void finishEditTags();
    void showGalleryContextMenu(const QPoint& position);
    void setBusy(bool busy);
    void showSetup();
    void showLogin(const std::filesystem::path& path);
    void showUnlocked();
    void openShare();
    void lockVault();
    void resetAutoLock();
    void setError(QLabel* label, const QString& message);

    QWidget* captionBar_{nullptr};
    QAbstractButton* captionMax_{nullptr};
    bool frameReady_{false};
    QStackedWidget* contentStack_{nullptr};
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
    QPushButton* shareButton_{nullptr};
    QComboBox* viewModeCombo_{nullptr};
    QButtonGroup* viewModeGroup_{nullptr};
    QPushButton* sortButton_{nullptr};
    int sortKey_{0};
    bool sortAscending_{true};
    QComboBox* tagFilterCombo_{nullptr};
    QLineEdit* searchEdit_{nullptr};
    QStackedWidget* galleryStack_{nullptr};
    QTreeWidget* detailsTree_{nullptr};
    QListWidget* iconList_{nullptr};
    QProgressBar* progressBar_{nullptr};
    PlayerWindow* playerWindow_{nullptr};
    QLabel* statusCountLabel_{nullptr};
    FrostedBar* libraryTop_{nullptr};
    FrostedBar* libraryBottom_{nullptr};
    std::int64_t tagFilterId_{-1};
    QString tagFilterName_;
    QString searchText_;
    // Label for the in-progress batch status text ("Importing"/"Restoring").
    QString batchLabel_;
    // Monotonic epochs: refreshGallery/refreshTagFilter bump their own counter;
    // async snapshots capture it at start and are DISCARDED when they land on
    // a stale epoch. This stops a pre-delete snapshot from resurrecting a
    // removed video after the remove's own refresh finished. Two counters so
    // the tag filter and the gallery don't invalidate each other when they
    // run as part of the same refresh cycle.
    std::uint64_t galleryGeneration_{0};
    std::uint64_t tagGeneration_{0};
    QTimer* autoLockTimer_{nullptr};
    // Batch worker thread: imports/restores run off the UI thread and report
    // progress through queued signals (never blocks the UI, no polling).
    QThread* batchThread_{nullptr};
    BatchWorker* batchWorker_{nullptr};
    bool batchBusy_{false};
    QFutureWatcher<std::shared_ptr<VaultOperationResult>>* watcher_{nullptr};
    QFutureWatcher<std::shared_ptr<core::Result<bool>>>* adminWatcher_{nullptr};
    QFutureWatcher<std::shared_ptr<core::Result<int>>>* thumbnailWatcher_{nullptr};
    QFutureWatcher<std::shared_ptr<TagEditorData>>* tagEditorWatcher_{nullptr};
    // Shared so worker threads can hold the vault alive during import/list.
    std::shared_ptr<core::Vault> vault_;
    // Declared after the vault so sharing stops before the vault is destroyed.
    std::unique_ptr<ShareServer> shareServer_;
};

} // namespace videovault::app
