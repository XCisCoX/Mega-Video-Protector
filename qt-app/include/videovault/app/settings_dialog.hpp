#pragma once

#include <QDialog>
#include <QEvent>
#include <QImage>

#include <memory>
#include <utility>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QShowEvent;

template <typename T>
class QFutureWatcher;

namespace videovault::core {
class Vault;
template <typename T>
class Result;
} // namespace videovault::core

namespace videovault::app {

// Vault settings: change password, manage tags (add/rename/delete), and the
// player's streaming cache budget (persisted in QSettings; the player picks
// it up when it opens a video). Tag changes emit settingsChanged() so the
// main window can refresh the gallery and tag filter.
class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(
        const std::shared_ptr<videovault::core::Vault>& vault,
        QWidget* parent = nullptr);

signals:
    void settingsChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void reloadTags();
    void beginPasswordChange();
    void finishPasswordChange();
    void addTag();
    void renameSelectedTag();
    void removeSelectedTag();
    void applyCacheSelection();
    void beginRegenerateMissing();
    void finishRegenerateMissing();
    void setStatus(const QString& message, bool error = false);
    bool eventFilter(QObject* watched, QEvent* event) override;

    std::shared_ptr<videovault::core::Vault> vault_;

    // Security section.
    QLineEdit* currentPassword_{nullptr};
    QLineEdit* newPassword_{nullptr};
    QLineEdit* confirmPassword_{nullptr};
    QComboBox* securityProfile_{nullptr};
    QPushButton* changePasswordButton_{nullptr};
    QFutureWatcher<std::shared_ptr<videovault::core::Result<bool>>>* passwordWatcher_{nullptr};

    // Tags section.
    QListWidget* tagList_{nullptr};
    QLineEdit* newTagEdit_{nullptr};
    QPushButton* addTagButton_{nullptr};
    QPushButton* renameTagButton_{nullptr};
    QPushButton* removeTagButton_{nullptr};

    // Playback section.
    QComboBox* cacheCombo_{nullptr};

    // Rebuilds stored thumbnails that import never managed to create.
    QPushButton* regenerateButton_{nullptr};
    QFutureWatcher<std::shared_ptr<std::pair<int, int>>>* regenerateWatcher_{nullptr};

    QLabel* status_{nullptr};
    QImage backdrop_;
};

} // namespace videovault::app
