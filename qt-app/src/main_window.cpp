#include "videovault/app/main_window.hpp"

#include "videovault/core/vault.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <string>
#include <utility>

namespace videovault::app {
namespace {

constexpr int kSetupPage = 0;
constexpr int kLoginPage = 1;
constexpr int kUnlockedPage = 2;
constexpr int kAutoLockMilliseconds = 5 * 60 * 1000;

std::filesystem::path pathFromText(const QString& text) {
    return std::filesystem::path(text.trimmed().toStdWString());
}

QString textFromPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

void clearSecret(std::string& secret) noexcept {
    if (!secret.empty()) {
        SecureZeroMemory(secret.data(), secret.size());
        secret.clear();
        secret.shrink_to_fit();
    }
}

QLabel* heading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("pageTitle"));
    return label;
}

QLabel* description(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("description"));
    label->setWordWrap(true);
    return label;
}

QLabel* errorLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setObjectName(QStringLiteral("errorLabel"));
    label->setWordWrap(true);
    label->hide();
    return label;
}

QFrame* cardFor(QWidget* page) {
    auto* card = new QFrame(page);
    card->setObjectName(QStringLiteral("card"));
    card->setMaximumWidth(620);
    return card;
}

void placeCard(QWidget* page, QFrame* card) {
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(48, 48, 48, 48);
    outer->addStretch();
    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch();
}

core::Argon2Parameters selectedParameters(const int profile) {
    core::Argon2Parameters parameters;
    if (profile == 1) {
        parameters.memory_kib = 512U * 1024U;
        parameters.iterations = 4U;
        parameters.parallelism = 2U;
    }
    return parameters;
}

} // namespace

struct VaultOperationResult {
    bool succeeded{false};
    std::unique_ptr<core::Vault> vault;
    core::VaultError error;
};

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), autoLockTimer_(new QTimer(this)) {
    setWindowTitle(QStringLiteral("Mega Video Protect"));
    resize(1020, 700);
    setMinimumSize(780, 520);

    autoLockTimer_->setSingleShot(true);
    autoLockTimer_->setInterval(kAutoLockMilliseconds);
    connect(autoLockTimer_, &QTimer::timeout, this, [this] { lockVault(); });
    qApp->installEventFilter(this);

    buildInterface();

    QSettings settings;
    const auto remembered = pathFromText(settings.value(QStringLiteral("vault/location")).toString());
    if (!remembered.empty() && core::Vault::exists(remembered)) {
        showLogin(remembered);
    } else {
        showSetup();
    }
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    if (importWatcher_ != nullptr) {
        importWatcher_->waitForFinished();
    }
    if (adminWatcher_ != nullptr) {
        adminWatcher_->waitForFinished();
    }
    if (vault_) {
        vault_->lock();
    }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (vault_ && vault_->is_unlocked()) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
            resetAutoLock();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::buildInterface() {
    pages_ = new QStackedWidget(this);
    setupPage_ = buildSetupPage();
    loginPage_ = buildLoginPage();
    unlockedPage_ = buildUnlockedPage();
    pages_->addWidget(setupPage_);
    pages_->addWidget(loginPage_);
    pages_->addWidget(unlockedPage_);
    setCentralWidget(pages_);
}

QWidget* MainWindow::buildSetupPage() {
    auto* page = new QWidget(this);
    auto* card = cardFor(page);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(42, 38, 42, 40);
    layout->setSpacing(14);
    layout->addWidget(heading(QStringLiteral("Create your encrypted vault"), card));
    layout->addWidget(description(
        QStringLiteral("Choose a private storage location and a strong password. "
                       "The password is never stored."), card));

    auto* locationRow = new QHBoxLayout();
    setupLocation_ = new QLineEdit(card);
    setupLocation_->setPlaceholderText(QStringLiteral("Vault storage folder"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), card);
    locationRow->addWidget(setupLocation_, 1);
    locationRow->addWidget(browse);
    layout->addLayout(locationRow);

    setupPassword_ = new QLineEdit(card);
    setupPassword_->setEchoMode(QLineEdit::Password);
    setupPassword_->setPlaceholderText(QStringLiteral("Password"));
    setupConfirmation_ = new QLineEdit(card);
    setupConfirmation_->setEchoMode(QLineEdit::Password);
    setupConfirmation_->setPlaceholderText(QStringLiteral("Confirm password"));
    layout->addWidget(setupPassword_);
    layout->addWidget(setupConfirmation_);

    securityProfile_ = new QComboBox(card);
    securityProfile_->addItem(QStringLiteral("Balanced — 256 MiB, 3 iterations"));
    securityProfile_->addItem(QStringLiteral("High security — 512 MiB, 4 iterations"));
    layout->addWidget(description(QStringLiteral("Argon2id security profile"), card));
    layout->addWidget(securityProfile_);

    setupError_ = errorLabel(card);
    layout->addWidget(setupError_);

    createButton_ = new QPushButton(QStringLiteral("Create vault"), card);
    createButton_->setProperty("primary", true);
    auto* existingButton = new QPushButton(QStringLiteral("Open an existing vault"), card);
    layout->addWidget(createButton_);
    layout->addWidget(existingButton);

    connect(browse, &QPushButton::clicked, this, [this] { selectSetupLocation(); });
    connect(createButton_, &QPushButton::clicked, this, [this] { beginCreate(); });
    connect(existingButton, &QPushButton::clicked, this, [this] { showLogin({}); });
    connect(setupConfirmation_, &QLineEdit::returnPressed, this, [this] { beginCreate(); });

    placeCard(page, card);
    return page;
}

QWidget* MainWindow::buildLoginPage() {
    auto* page = new QWidget(this);
    auto* card = cardFor(page);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(42, 38, 42, 40);
    layout->setSpacing(14);
    layout->addWidget(heading(QStringLiteral("Unlock Mega Video Protect"), card));
    layout->addWidget(description(
        QStringLiteral("Enter the vault password to unlock the encrypted database."), card));

    auto* locationRow = new QHBoxLayout();
    loginLocation_ = new QLineEdit(card);
    loginLocation_->setPlaceholderText(QStringLiteral("Vault storage folder"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), card);
    locationRow->addWidget(loginLocation_, 1);
    locationRow->addWidget(browse);
    layout->addLayout(locationRow);

    loginPassword_ = new QLineEdit(card);
    loginPassword_->setEchoMode(QLineEdit::Password);
    loginPassword_->setPlaceholderText(QStringLiteral("Password"));
    layout->addWidget(loginPassword_);

    loginError_ = errorLabel(card);
    layout->addWidget(loginError_);

    unlockButton_ = new QPushButton(QStringLiteral("Unlock vault"), card);
    unlockButton_->setProperty("primary", true);
    auto* newVaultButton = new QPushButton(QStringLiteral("Create a new vault"), card);
    layout->addWidget(unlockButton_);
    layout->addWidget(newVaultButton);

    connect(browse, &QPushButton::clicked, this, [this] { selectLoginLocation(); });
    connect(unlockButton_, &QPushButton::clicked, this, [this] { beginOpen(); });
    connect(loginPassword_, &QLineEdit::returnPressed, this, [this] { beginOpen(); });
    connect(newVaultButton, &QPushButton::clicked, this, [this] { showSetup(); });

    placeCard(page, card);
    return page;
}

QWidget* MainWindow::buildUnlockedPage() {
    auto* page = new QWidget(this);
    auto* card = cardFor(page);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(42, 38, 42, 40);
    layout->setSpacing(16);
    layout->addWidget(heading(QStringLiteral("Vault unlocked"), card));
    layout->addWidget(description(
        QStringLiteral("The SQLCipher database and authenticated password record are open. "
                       "Video import and gallery features are added in the following phases."), card));
    unlockedLocation_ = description({}, card);
    unlockedLocation_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(unlockedLocation_);
    layout->addWidget(description(
        QStringLiteral("Automatic lock: 5 minutes after the last keyboard or mouse activity."), card));

    gallery_ = new QListWidget(card);
    gallery_->setObjectName(QStringLiteral("gallery"));
    layout->addWidget(gallery_);

    auto* importRow = new QHBoxLayout();
    importButton_ = new QPushButton(QStringLiteral("Import video…"), card);
    importButton_->setProperty("primary", true);
    removeButton_ = new QPushButton(QStringLiteral("Remove selected"), card);
    galleryStatus_ = errorLabel(card);
    importRow->addWidget(importButton_);
    importRow->addWidget(removeButton_);
    importRow->addWidget(galleryStatus_, 1);
    layout->addLayout(importRow);

    changePasswordButton_ = new QPushButton(QStringLiteral("Change password…"), card);
    auto* lockButton = new QPushButton(QStringLiteral("Lock vault"), card);
    lockButton->setProperty("primary", true);
    layout->addWidget(changePasswordButton_);
    layout->addWidget(lockButton);
    connect(lockButton, &QPushButton::clicked, this, [this] { lockVault(); });
    connect(importButton_, &QPushButton::clicked, this, [this] { beginImport(); });
    connect(removeButton_, &QPushButton::clicked, this, [this] { beginRemoveSelected(); });
    connect(changePasswordButton_, &QPushButton::clicked, this, [this] { beginChangePassword(); });
    placeCard(page, card);
    return page;
}

void MainWindow::selectSetupLocation() {
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose vault storage location"), setupLocation_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty()) {
        setupLocation_->setText(QDir::toNativeSeparators(selected));
    }
}

void MainWindow::selectLoginLocation() {
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Choose existing vault"), loginLocation_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!selected.isEmpty()) {
        loginLocation_->setText(QDir::toNativeSeparators(selected));
    }
}

void MainWindow::beginCreate() {
    setError(setupError_, {});
    const auto root = pathFromText(setupLocation_->text());
    const QString passwordText = setupPassword_->text();
    if (root.empty()) {
        setError(setupError_, QStringLiteral("Choose a vault storage location."));
        return;
    }
    if (passwordText.isEmpty()) {
        setError(setupError_, QStringLiteral("Enter a password."));
        return;
    }
    if (passwordText != setupConfirmation_->text()) {
        setError(setupError_, QStringLiteral("The password confirmation does not match."));
        return;
    }

    auto password = std::make_shared<std::string>(passwordText.toUtf8().constData());
    const auto parameters = selectedParameters(securityProfile_->currentIndex());
    setBusy(true);
    watcher_ = new QFutureWatcher<std::shared_ptr<VaultOperationResult>>(this);
    connect(watcher_, &QFutureWatcherBase::finished, this, [this] { finishOperation(); });
    watcher_->setFuture(QtConcurrent::run([root, password, parameters] {
        auto outcome = std::make_shared<VaultOperationResult>();
        auto result = core::Vault::create(root, *password, parameters);
        clearSecret(*password);
        if (result) {
            outcome->succeeded = true;
            outcome->vault = std::make_unique<core::Vault>(std::move(result.value()));
        } else {
            outcome->error = result.error();
        }
        return outcome;
    }));
}

void MainWindow::beginOpen() {
    setError(loginError_, {});
    const auto root = pathFromText(loginLocation_->text());
    const QString passwordText = loginPassword_->text();
    if (root.empty()) {
        setError(loginError_, QStringLiteral("Choose a vault storage location."));
        return;
    }
    if (passwordText.isEmpty()) {
        setError(loginError_, QStringLiteral("Enter the vault password."));
        return;
    }

    auto password = std::make_shared<std::string>(passwordText.toUtf8().constData());
    setBusy(true);
    watcher_ = new QFutureWatcher<std::shared_ptr<VaultOperationResult>>(this);
    connect(watcher_, &QFutureWatcherBase::finished, this, [this] { finishOperation(); });
    watcher_->setFuture(QtConcurrent::run([root, password] {
        auto outcome = std::make_shared<VaultOperationResult>();
        auto result = core::Vault::open(root, *password);
        clearSecret(*password);
        if (result) {
            outcome->succeeded = true;
            outcome->vault = std::make_unique<core::Vault>(std::move(result.value()));
        } else {
            outcome->error = result.error();
        }
        return outcome;
    }));
}

void MainWindow::finishOperation() {
    auto* completedWatcher = watcher_;
    watcher_ = nullptr;
    const auto outcome = completedWatcher->result();
    completedWatcher->deleteLater();
    setupPassword_->clear();
    setupConfirmation_->clear();
    loginPassword_->clear();
    setBusy(false);

    if (!outcome || !outcome->succeeded) {
        const auto error = outcome
            ? outcome->error
            : core::VaultError{core::VaultErrorCode::DatabaseFailure, "worker returned no result"};
        const QString message = QString::fromUtf8(core::user_message(error.code).data());
        setError(pages_->currentIndex() == kSetupPage ? setupError_ : loginError_, message);
        return;
    }

    vault_ = std::shared_ptr<core::Vault>(std::move(outcome->vault));
    QSettings settings;
    settings.setValue(QStringLiteral("vault/location"), textFromPath(vault_->root_path()));
    showUnlocked();
}

void MainWindow::beginImport() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    setError(galleryStatus_, {});
    const QString selected = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import video"), {},
        QStringLiteral("Video files (*.mp4 *.mkv *.avi *.mov *.wmv *.webm *.m4v);;All files (*)"));
    if (selected.isEmpty()) {
        return;
    }

    const auto vault = vault_;
    const auto source = std::make_shared<std::filesystem::path>(pathFromText(selected));
    importButton_->setEnabled(false);
    galleryStatus_->setText(QStringLiteral("Importing…"));
    galleryStatus_->setVisible(true);
    importWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<std::int64_t>>>(this);
    connect(importWatcher_, &QFutureWatcherBase::finished, this, [this] { finishImport(); });
    importWatcher_->setFuture(QtConcurrent::run([vault, source] {
        return std::make_shared<core::Result<std::int64_t>>(vault->import_file(*source));
    }));
}

void MainWindow::finishImport() {
    auto* completed = importWatcher_;
    importWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    importButton_->setEnabled(true);

    if (!outcome) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    refreshGallery();
}

void MainWindow::refreshGallery() {
    gallery_->clear();
    setError(galleryStatus_, {});
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto vault = vault_;
    auto* watcher = new QFutureWatcher<std::vector<core::VideoInfo>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, vault, watcher] {
        if (vault_ != vault || !vault_->is_unlocked()) {
            watcher->deleteLater();
            return;
        }
        const auto videos = watcher->result();
        for (const auto& video : videos) {
            const QString label = QStringLiteral("%1  (%2 bytes)")
                .arg(QString::fromStdString(video.display_name))
                .arg(static_cast<qulonglong>(video.original_size));
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, static_cast<qlonglong>(video.id));
            gallery_->addItem(item);
        }
        if (videos.empty()) {
            setError(galleryStatus_, QStringLiteral("No videos imported yet."));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([vault] {
        auto videos = vault->list_videos();
        if (!videos) {
            return std::vector<core::VideoInfo>{};
        }
        return videos.value();
    }));
}

void MainWindow::beginRemoveSelected() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    auto* item = gallery_->currentItem();
    if (item == nullptr) {
        setError(galleryStatus_, QStringLiteral("Select a video to remove."));
        galleryStatus_->setVisible(true);
        return;
    }
    const auto video_id = item->data(Qt::UserRole).toLongLong();
    const auto vault = vault_;
    importButton_->setEnabled(false);
    removeButton_->setEnabled(false);
    changePasswordButton_->setEnabled(false);
    adminWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(adminWatcher_, &QFutureWatcherBase::finished, this, [this] { finishRemoveSelected(); });
    adminWatcher_->setFuture(QtConcurrent::run([vault, video_id] {
        return std::make_shared<core::Result<bool>>(vault->remove_video(video_id));
    }));
}

void MainWindow::finishRemoveSelected() {
    auto* completed = adminWatcher_;
    adminWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    importButton_->setEnabled(true);
    removeButton_->setEnabled(true);
    changePasswordButton_->setEnabled(true);
    if (!outcome || !outcome.value()) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    refreshGallery();
}

void MainWindow::beginChangePassword() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    setError(galleryStatus_, {});

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Change vault password"));
    dialog.setModal(true);
    auto* form = new QFormLayout(&dialog);
    auto* current = new QLineEdit(&dialog);
    current->setEchoMode(QLineEdit::Password);
    auto* next = new QLineEdit(&dialog);
    next->setEchoMode(QLineEdit::Password);
    auto* confirmation = new QLineEdit(&dialog);
    confirmation->setEchoMode(QLineEdit::Password);
    auto* profile = new QComboBox(&dialog);
    profile->addItem(QStringLiteral("Balanced — 256 MiB, 3 iterations"));
    profile->addItem(QStringLiteral("High security — 512 MiB, 4 iterations"));
    form->addRow(QStringLiteral("Current password"), current);
    form->addRow(QStringLiteral("New password"), next);
    form->addRow(QStringLiteral("Confirm new password"), confirmation);
    form->addRow(QStringLiteral("Argon2id security profile"), profile);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (next->text() != confirmation->text()) {
        setError(galleryStatus_, QStringLiteral("The new password confirmation does not match."));
        galleryStatus_->setVisible(true);
        return;
    }

    const auto vault = vault_;
    const auto current_text =
        std::make_shared<std::string>(current->text().toUtf8().constData());
    const auto next_text =
        std::make_shared<std::string>(next->text().toUtf8().constData());
    const auto parameters = selectedParameters(profile->currentIndex());
    current->clear();
    next->clear();
    confirmation->clear();

    importButton_->setEnabled(false);
    removeButton_->setEnabled(false);
    changePasswordButton_->setEnabled(false);
    galleryStatus_->setText(QStringLiteral("Changing password…"));
    galleryStatus_->setVisible(true);
    adminWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(adminWatcher_, &QFutureWatcherBase::finished, this, [this] { finishChangePassword(); });
    adminWatcher_->setFuture(QtConcurrent::run([vault, current_text, next_text, parameters] {
        auto result = vault->change_password(*current_text, *next_text, parameters);
        clearSecret(*current_text);
        clearSecret(*next_text);
        return std::make_shared<core::Result<bool>>(std::move(result));
    }));
}

void MainWindow::finishChangePassword() {
    auto* completed = adminWatcher_;
    adminWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    importButton_->setEnabled(true);
    removeButton_->setEnabled(true);
    changePasswordButton_->setEnabled(true);
    if (!outcome || !outcome.value()) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    setError(galleryStatus_, QStringLiteral("Password changed."));
    galleryStatus_->setVisible(true);
}

void MainWindow::setBusy(const bool busy) {
    createButton_->setEnabled(!busy);
    unlockButton_->setEnabled(!busy);
    setupPage_->setEnabled(!busy);
    loginPage_->setEnabled(!busy);
    if (busy) {
        setCursor(Qt::WaitCursor);
    } else {
        unsetCursor();
    }
}

void MainWindow::showSetup() {
    setError(setupError_, {});
    pages_->setCurrentIndex(kSetupPage);
    setupPassword_->clear();
    setupConfirmation_->clear();
    setupLocation_->setFocus();
}

void MainWindow::showLogin(const std::filesystem::path& path) {
    setError(loginError_, {});
    if (!path.empty()) {
        loginLocation_->setText(textFromPath(path));
    }
    pages_->setCurrentIndex(kLoginPage);
    loginPassword_->clear();
    loginPassword_->setFocus();
}

void MainWindow::showUnlocked() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    unlockedLocation_->setText(
        QStringLiteral("Vault location: %1").arg(textFromPath(vault_->root_path())));
    pages_->setCurrentIndex(kUnlockedPage);
    resetAutoLock();
    refreshGallery();
}

void MainWindow::lockVault() {
    autoLockTimer_->stop();
    std::filesystem::path root;
    if (vault_) {
        root = vault_->root_path();
        vault_->lock();
        vault_.reset();
    }
    importButton_->setEnabled(true);
    removeButton_->setEnabled(true);
    changePasswordButton_->setEnabled(true);
    gallery_->clear();
    showLogin(root);
}

void MainWindow::resetAutoLock() {
    if (vault_ && vault_->is_unlocked()) {
        autoLockTimer_->start();
    }
}

void MainWindow::setError(QLabel* label, const QString& message) {
    label->setText(message);
    label->setVisible(!message.isEmpty());
}

} // namespace videovault::app
