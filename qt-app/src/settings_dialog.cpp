#include "videovault/app/settings_dialog.hpp"

#include "videovault/core/vault.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include "videovault/app/glass.hpp"

#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QVBoxLayout>

#include <utility>

namespace videovault::app {

SettingsDialog::SettingsDialog(
    const std::shared_ptr<videovault::core::Vault>& vault,
    QWidget* parent)
    : QDialog(parent),
      vault_(vault) {
    setWindowTitle(QStringLiteral("Settings"));
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(48, 48, 48, 48);
    outer->addStretch(1);

    auto* sheet = new QFrame(this);
    sheet->setObjectName(QStringLiteral("sheet"));
    sheet->setAttribute(Qt::WA_StyledBackground, true);
    sheet->setFixedWidth(440);
    sheet->setStyleSheet(QStringLiteral(
        "QFrame#sheet { background: #121214; border-radius: 18px; }"
        "QFrame#settingsGroup { background: #1c1c1e; border: none; border-radius: 14px; }"
        "QFrame#sheet QLineEdit, QFrame#sheet QComboBox {"
        "  background: transparent; border: none; border-radius: 0; min-height: 40px; color: white; }"
        "QFrame#sheet QComboBox QAbstractItemView {"
        "  background: #1c1c1e; color: white; selection-background-color: #3390ec; }"
        "QPushButton#sheetAction {"
        "  background: transparent; border: none; color: #3390ec; font-weight: 600; min-height: 40px; }"
        "QPushButton#sheetAction:disabled { color: #636366; }"));
    auto* layout = new QVBoxLayout(sheet);
    layout->setContentsMargins(18, 16, 18, 18);
    layout->setSpacing(14);

    auto* titleRow = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("Settings"), sheet);
    title->setObjectName(QStringLiteral("pageTitle"));
    auto* done = new QPushButton(QStringLiteral("Done"), sheet);
    done->setObjectName(QStringLiteral("sheetAction"));
    done->setCursor(Qt::PointingHandCursor);
    done->setFocusPolicy(Qt::NoFocus);
    done->setAutoDefault(false);
    done->setDefault(false);
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    titleRow->addWidget(done);
    layout->addLayout(titleRow);

    auto section = [sheet](const QString& text) {
        auto* label = new QLabel(text, sheet);
        label->setObjectName(QStringLiteral("section"));
        return label;
    };
    auto hairline = [](QWidget* parent) {
        auto* line = new QWidget(parent);
        line->setFixedHeight(1);
        line->setAttribute(Qt::WA_StyledBackground, true);
        line->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 24);"));
        return line;
    };

    layout->addWidget(section(QStringLiteral("Password")));
    auto* securityBox = new QFrame(sheet);
    securityBox->setObjectName(QStringLiteral("settingsGroup"));
    securityBox->setAttribute(Qt::WA_StyledBackground, true);
    auto* securityLayout = new QVBoxLayout(securityBox);
    securityLayout->setContentsMargins(0, 4, 0, 4);
    securityLayout->setSpacing(0);
    currentPassword_ = new QLineEdit(securityBox);
    currentPassword_->setEchoMode(QLineEdit::Password);
    currentPassword_->setPlaceholderText(QStringLiteral("Current password"));
    newPassword_ = new QLineEdit(securityBox);
    newPassword_->setEchoMode(QLineEdit::Password);
    newPassword_->setPlaceholderText(QStringLiteral("New password"));
    confirmPassword_ = new QLineEdit(securityBox);
    confirmPassword_->setEchoMode(QLineEdit::Password);
    confirmPassword_->setPlaceholderText(QStringLiteral("Confirm new password"));
    securityProfile_ = new QComboBox(securityBox);
    securityProfile_->addItem(QStringLiteral("Balanced — 256 MiB, 3 iterations"));
    securityProfile_->addItem(QStringLiteral("High security — 512 MiB, 4 iterations"));
    changePasswordButton_ = new QPushButton(QStringLiteral("Change password"), securityBox);
    changePasswordButton_->setObjectName(QStringLiteral("sheetAction"));
    changePasswordButton_->setCursor(Qt::PointingHandCursor);
    changePasswordButton_->setAutoDefault(false);
    changePasswordButton_->setDefault(false);
    changePasswordButton_->setFocusPolicy(Qt::NoFocus);
    securityLayout->addWidget(currentPassword_);
    securityLayout->addWidget(hairline(securityBox));
    securityLayout->addWidget(newPassword_);
    securityLayout->addWidget(hairline(securityBox));
    securityLayout->addWidget(confirmPassword_);
    securityLayout->addWidget(hairline(securityBox));
    securityLayout->addWidget(securityProfile_);
    securityLayout->addWidget(hairline(securityBox));
    securityLayout->addWidget(changePasswordButton_);
    layout->addWidget(securityBox);

    layout->addWidget(section(QStringLiteral("Tags")));
    auto* tagsBox = new QFrame(sheet);
    tagsBox->setObjectName(QStringLiteral("settingsGroup"));
    tagsBox->setAttribute(Qt::WA_StyledBackground, true);
    auto* tagsLayout = new QVBoxLayout(tagsBox);
    tagsLayout->setContentsMargins(0, 4, 0, 4);
    tagsLayout->setSpacing(0);
    tagList_ = new QListWidget(tagsBox);
    tagList_->setObjectName(QStringLiteral("tagList"));
    tagList_->setSelectionMode(QAbstractItemView::NoSelection);
    tagList_->setFrameShape(QFrame::NoFrame);
    tagList_->setFixedHeight(200);
    auto* addRow = new QHBoxLayout();
    addRow->setContentsMargins(0, 0, 8, 0);
    newTagEdit_ = new QLineEdit(tagsBox);
    newTagEdit_->setPlaceholderText(QStringLiteral("New tag"));
    addTagButton_ = new QPushButton(QStringLiteral("Add"), tagsBox);
    addTagButton_->setObjectName(QStringLiteral("sheetAction"));
    addTagButton_->setCursor(Qt::PointingHandCursor);
    addTagButton_->setAutoDefault(false);
    addTagButton_->setDefault(false);
    addTagButton_->setFocusPolicy(Qt::NoFocus);
    newTagEdit_->installEventFilter(this);
    addRow->addWidget(newTagEdit_, 1);
    addRow->addWidget(addTagButton_);
    tagsLayout->addWidget(tagList_);
    tagsLayout->addWidget(hairline(tagsBox));
    tagsLayout->addLayout(addRow);
    layout->addWidget(tagsBox);

    layout->addWidget(section(QStringLiteral("Thumbnails")));
    auto* playbackBox = new QFrame(sheet);
    playbackBox->setObjectName(QStringLiteral("settingsGroup"));
    playbackBox->setAttribute(Qt::WA_StyledBackground, true);
    auto* playbackLayout = new QVBoxLayout(playbackBox);
    playbackLayout->setContentsMargins(0, 4, 0, 4);
    regenerateButton_ = new QPushButton(QStringLiteral("Regenerate missing"), playbackBox);
    regenerateButton_->setObjectName(QStringLiteral("sheetAction"));
    regenerateButton_->setCursor(Qt::PointingHandCursor);
    regenerateButton_->setAutoDefault(false);
    regenerateButton_->setDefault(false);
    regenerateButton_->setFocusPolicy(Qt::NoFocus);
    regenerateButton_->setToolTip(QStringLiteral(
        "Build thumbnails only for videos that do not have one yet"));
    playbackLayout->addWidget(regenerateButton_);
    layout->addWidget(playbackBox);

    status_ = new QLabel(sheet);
    status_->setWordWrap(true);
    status_->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
    layout->addWidget(status_);

    outer->addWidget(sheet, 0, Qt::AlignHCenter);
    outer->addStretch(1);

    connect(done, &QPushButton::clicked, this, &QDialog::reject);

    connect(changePasswordButton_, &QPushButton::clicked,
        this, [this] { beginPasswordChange(); });
    connect(addTagButton_, &QPushButton::clicked, this, [this] { addTag(); });
    connect(regenerateButton_, &QPushButton::clicked,
        this, [this] { beginRegenerateMissing(); });
    if (cacheCombo_ != nullptr) {
        connect(cacheCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { applyCacheSelection(); });
    }

    reloadTags();
}

void SettingsDialog::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!backdrop_.isNull()) {
        painter.drawImage(rect(), backdrop_);
    } else {
        painter.fillRect(rect(), QColor(0, 0, 0));
    }
    painter.fillRect(rect(), QColor(0, 0, 0, 120));
}

void SettingsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (QWidget* host = parentWidget()) {
        const QPoint origin = host->mapToGlobal(QPoint(0, 0));
        setGeometry(QRect(origin, host->size()));
        backdrop_ = frost(host->grab().toImage());
        update();
    }
}

void SettingsDialog::mousePressEvent(QMouseEvent* event) {
    if (childAt(event->pos()) == nullptr) {
        reject();
        return;
    }
    QDialog::mousePressEvent(event);
}

void SettingsDialog::setStatus(const QString& message, const bool error) {
    status_->setText(message);
    status_->setStyleSheet(error
        ? QStringLiteral("color: #ff6b6b;")
        : QStringLiteral("color: #3390ec;"));
}

bool SettingsDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == newTagEdit_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<const QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            addTag();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
        const auto id = watched->property("tagId");
        if (id.isValid()) {
            for (int i = 0; i < tagList_->count(); ++i) {
                auto* item = tagList_->item(i);
                if (item->data(Qt::UserRole) == id) {
                    tagList_->setCurrentItem(item);
                    renameSelectedTag();
                    return true;
                }
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SettingsDialog::reloadTags() {
    tagList_->clear();
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto tags = vault_->list_tags();
    if (!tags) {
        return;
    }
    for (const auto& tag : tags.value()) {
        const QString name = QString::fromStdString(tag.name);
        auto* item = new QListWidgetItem(tagList_);
        item->setText(QString());
        item->setData(Qt::UserRole, static_cast<qulonglong>(tag.id));
        item->setData(Qt::UserRole + 1, name);
        item->setSizeHint(QSize(0, 44));
        item->setToolTip(QStringLiteral("Double-click to rename. Videos: %1")
            .arg(tag.video_count));
        auto* row = new QWidget(tagList_);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(16, 0, 8, 0);
        QFont nameFont(QStringLiteral("Segoe UI"));
        nameFont.setPointSize(11);
        QFont metaFont(QStringLiteral("Segoe UI"));
        metaFont.setPointSize(10);
        auto* nameLabel = new QLabel(name, row);
        nameLabel->setFont(nameFont);
        nameLabel->setStyleSheet(QStringLiteral("color: #ffffff; background: transparent;"));
        nameLabel->setProperty("tagId", static_cast<qulonglong>(tag.id));
        nameLabel->installEventFilter(this);
        auto* countLabel = new QLabel(QString::number(tag.video_count), row);
        countLabel->setFont(metaFont);
        countLabel->setStyleSheet(QStringLiteral("color: #8e8e93; background: transparent;"));
        auto* remove = new QPushButton(QStringLiteral("Remove"), row);
        remove->setFont(metaFont);
        remove->setCursor(Qt::PointingHandCursor);
        remove->setStyleSheet(QStringLiteral(
            "QPushButton { color: #ff453a; background: transparent; border: none;"
            " padding: 0 8px; }"));
        remove->setFocusPolicy(Qt::NoFocus);
        remove->setToolTip(QStringLiteral("Remove tag"));
        rowLayout->addWidget(nameLabel, 1);
        rowLayout->addWidget(countLabel);
        rowLayout->addWidget(remove);
        tagList_->setItemWidget(item, row);
        connect(remove, &QPushButton::clicked, this, [this, item] {
            tagList_->setCurrentItem(item);
            removeSelectedTag();
        });
    }
    tagList_->setEnabled(true);
}

void SettingsDialog::beginPasswordChange() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    setStatus({});
    const QString current_text = currentPassword_->text();
    const QString next_text = newPassword_->text();
    if (current_text.isEmpty() || next_text.isEmpty()) {
        setStatus(QStringLiteral("Enter the current and a new password."), true);
        return;
    }
    if (next_text != confirmPassword_->text()) {
        setStatus(QStringLiteral("The new password confirmation does not match."), true);
        return;
    }
    auto current = std::make_shared<std::string>(current_text.toUtf8().constData());
    auto next = std::make_shared<std::string>(next_text.toUtf8().constData());
    const auto parameters = securityProfile_->currentIndex() == 0
        ? core::Argon2Parameters{256U * 1024U, 3U, 1U}
        : core::Argon2Parameters{512U * 1024U, 4U, 1U};
    const auto vault = vault_;
    changePasswordButton_->setEnabled(false);
    passwordWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(passwordWatcher_, &QFutureWatcherBase::finished,
        this, [this] { finishPasswordChange(); });
    passwordWatcher_->setFuture(QtConcurrent::run([vault, current, next, parameters] {
        auto outcome = std::make_shared<core::Result<bool>>(
            vault->change_password(*current, *next, parameters));
        current->assign(current->size(), '\0');
        next->assign(next->size(), '\0');
        return outcome;
    }));
}

void SettingsDialog::finishPasswordChange() {
    auto* completed = passwordWatcher_;
    passwordWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    changePasswordButton_->setEnabled(true);
    if (!outcome) {
        setStatus(QString::fromUtf8(
            core::user_message(outcome.error().code).data()), true);
        return;
    }
    currentPassword_->clear();
    newPassword_->clear();
    confirmPassword_->clear();
    setStatus(QStringLiteral("Password changed."), false);
}

void SettingsDialog::addTag() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const QString name = newTagEdit_->text().trimmed();
    if (name.isEmpty()) {
        return;
    }
    setStatus({});
    const auto result = vault_->create_tag(name.toStdString());
    if (!result) {
        setStatus(QString::fromUtf8(
            core::user_message(result.error().code).data()), true);
        return;
    }
    newTagEdit_->clear();
    reloadTags();
    emit settingsChanged();
}

void SettingsDialog::renameSelectedTag() {
    auto* item = tagList_->currentItem();
    if (item == nullptr || !vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto tag_id = static_cast<std::int64_t>(item->data(Qt::UserRole).toULongLong());
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename tag"),
        QStringLiteral("New name:"),
        QLineEdit::Normal, item->data(Qt::UserRole + 1).toString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    const auto result = vault_->rename_tag(tag_id, name.toStdString());
    if (!result) {
        setStatus(QString::fromUtf8(
            core::user_message(result.error().code).data()), true);
    }
    reloadTags();
    emit settingsChanged();
}

void SettingsDialog::removeSelectedTag() {
    auto* item = tagList_->currentItem();
    if (item == nullptr || !vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto tag_id = static_cast<std::int64_t>(item->data(Qt::UserRole).toULongLong());
    const auto result = vault_->delete_tag(tag_id);
    if (!result) {
        setStatus(QString::fromUtf8(
            core::user_message(result.error().code).data()), true);
    }
    reloadTags();
    emit settingsChanged();
}

void SettingsDialog::beginRegenerateMissing() {
    if (!vault_ || !vault_->is_unlocked() || regenerateWatcher_ != nullptr) {
        return;
    }
    setStatus(QStringLiteral("Regenerating missing thumbnails…"), false);
    regenerateButton_->setEnabled(false);
    const auto vault = vault_;
    regenerateWatcher_ =
        new QFutureWatcher<std::shared_ptr<std::pair<int, int>>>(this);
    connect(regenerateWatcher_, &QFutureWatcherBase::finished,
        this, [this] { finishRegenerateMissing(); });
    regenerateWatcher_->setFuture(QtConcurrent::run([vault] {
        // first = how many had no thumbnail, second = how many were created.
        // first < 0 means the gallery could not be listed.
        auto result = std::make_shared<std::pair<int, int>>(0, 0);
        const auto videos = vault->list_videos();
        if (!videos) {
            result->first = -1;
            return result;
        }
        for (const auto& video : videos.value()) {
            const auto stored = vault->thumbnail(video.id);
            if (stored && !stored.value().bytes.empty()) {
                continue;
            }
            ++result->first;
            const auto generated = vault->generate_thumbnail(video.id, 320U);
            if (generated && !generated.value().bytes.empty()) {
                ++result->second;
            }
        }
        return result;
    }));
}

void SettingsDialog::finishRegenerateMissing() {
    auto* completed = regenerateWatcher_;
    regenerateWatcher_ = nullptr;
    const auto outcome = completed->result();
    completed->deleteLater();
    regenerateButton_->setEnabled(true);
    const int missing = outcome->first;
    const int regenerated = outcome->second;
    if (missing < 0) {
        setStatus(QStringLiteral("Could not list the vault."), true);
        return;
    }
    if (missing == 0) {
        setStatus(QStringLiteral("Every video already has a thumbnail."), false);
        return;
    }
    if (regenerated == missing) {
        setStatus(QStringLiteral("Regenerated %1 missing thumbnail(s).").arg(regenerated), false);
    } else {
        setStatus(QStringLiteral("Regenerated %1 of %2 missing thumbnail(s).")
            .arg(regenerated)
            .arg(missing),
            regenerated == 0);
    }
    emit settingsChanged();
}

void SettingsDialog::applyCacheSelection() {
    const int megabytes = cacheCombo_->currentData().toInt();
    if (megabytes > 0) {
        QSettings().setValue(QStringLiteral("player/streamCacheMib"), megabytes);
    }
}

} // namespace videovault::app
