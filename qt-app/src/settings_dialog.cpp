#include "videovault/app/settings_dialog.hpp"

#include "videovault/core/vault.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <utility>

namespace videovault::app {

SettingsDialog::SettingsDialog(
    const std::shared_ptr<videovault::core::Vault>& vault,
    QWidget* parent)
    : QDialog(parent),
      vault_(vault) {
    setWindowTitle(QStringLiteral("Vault settings"));
    setModal(true);
    resize(460, 520);

    auto* layout = new QVBoxLayout(this);

    // --- Security: change the vault password. ---
    auto* securityBox = new QFrame(this);
    securityBox->setObjectName(QStringLiteral("settingsGroup"));
    auto* securityLayout = new QFormLayout(securityBox);
    currentPassword_ = new QLineEdit(securityBox);
    currentPassword_->setEchoMode(QLineEdit::Password);
    newPassword_ = new QLineEdit(securityBox);
    newPassword_->setEchoMode(QLineEdit::Password);
    confirmPassword_ = new QLineEdit(securityBox);
    confirmPassword_->setEchoMode(QLineEdit::Password);
    securityProfile_ = new QComboBox(securityBox);
    securityProfile_->addItem(QStringLiteral("Balanced — 256 MiB, 3 iterations"));
    securityProfile_->addItem(QStringLiteral("High security — 512 MiB, 4 iterations"));
    changePasswordButton_ = new QPushButton(QStringLiteral("Change password"), securityBox);
    securityLayout->addRow(QStringLiteral("Current password"), currentPassword_);
    securityLayout->addRow(QStringLiteral("New password"), newPassword_);
    securityLayout->addRow(QStringLiteral("Confirm new password"), confirmPassword_);
    securityLayout->addRow(QStringLiteral("Argon2id security profile"), securityProfile_);
    securityLayout->addRow(QString(), changePasswordButton_);

    // --- Tags: add, rename, delete. ---
    auto* tagsBox = new QFrame(this);
    tagsBox->setObjectName(QStringLiteral("settingsGroup"));
    auto* tagsLayout = new QVBoxLayout(tagsBox);
    tagList_ = new QListWidget(tagsBox);
    tagList_->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* tagButtons = new QHBoxLayout();
    addTagButton_ = new QPushButton(QStringLiteral("Add…"), tagsBox);
    renameTagButton_ = new QPushButton(QStringLiteral("Rename…"), tagsBox);
    removeTagButton_ = new QPushButton(QStringLiteral("Remove"), tagsBox);
    tagButtons->addWidget(addTagButton_);
    tagButtons->addWidget(renameTagButton_);
    tagButtons->addWidget(removeTagButton_);
    tagButtons->addStretch(1);
    tagsLayout->addWidget(tagList_);
    tagsLayout->addLayout(tagButtons);

    // --- Playback: streaming cache budget. ---
    auto* playbackBox = new QFrame(this);
    playbackBox->setObjectName(QStringLiteral("settingsGroup"));
    auto* playbackLayout = new QFormLayout(playbackBox);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setStyleSheet(QStringLiteral("color: #c74e4e;"));

    layout->addWidget(securityBox);
    layout->addWidget(tagsBox);
    layout->addWidget(playbackBox);
    layout->addWidget(status_);
    layout->addStretch(1);

    connect(changePasswordButton_, &QPushButton::clicked,
        this, [this] { beginPasswordChange(); });
    connect(addTagButton_, &QPushButton::clicked, this, [this] { addTag(); });
    connect(renameTagButton_, &QPushButton::clicked,
        this, [this] { renameSelectedTag(); });
    connect(removeTagButton_, &QPushButton::clicked,
        this, [this] { removeSelectedTag(); });
    connect(cacheCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) { applyCacheSelection(); });

    reloadTags();
}

void SettingsDialog::setStatus(const QString& message, const bool error) {
    status_->setText(message);
    status_->setStyleSheet(error
        ? QStringLiteral("color: #c74e4e;")
        : QStringLiteral("color: #6fae6f;"));
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
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  (%2)").arg(QString::fromStdString(tag.name))
                .arg(tag.video_count),
            tagList_);
        item->setData(Qt::UserRole, static_cast<qulonglong>(tag.id));
        item->setToolTip(QStringLiteral("Videos with this tag: %1")
            .arg(tag.video_count));
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
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Add tag"),
        QStringLiteral("Tag name (shared across videos):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    setStatus({});
    const auto result = vault_->create_tag(name.toStdString());
    if (!result) {
        setStatus(QString::fromUtf8(
            core::user_message(result.error().code).data()), true);
    }
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
        QLineEdit::Normal, item->text().section(QLatin1Char(' '), 0, 0), &ok);
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

void SettingsDialog::applyCacheSelection() {
    const int megabytes = cacheCombo_->currentData().toInt();
    if (megabytes > 0) {
        QSettings().setValue(QStringLiteral("player/streamCacheMib"), megabytes);
    }
}

} // namespace videovault::app
