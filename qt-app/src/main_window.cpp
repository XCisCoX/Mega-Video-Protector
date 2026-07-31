#include "videovault/app/main_window.hpp"

#include "videovault/core/vault.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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
    auto* lockButton = new QPushButton(QStringLiteral("Lock vault"), card);
    lockButton->setProperty("primary", true);
    layout->addWidget(lockButton);
    connect(lockButton, &QPushButton::clicked, this, [this] { lockVault(); });
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

    vault_ = std::move(outcome->vault);
    QSettings settings;
    settings.setValue(QStringLiteral("vault/location"), textFromPath(vault_->root_path()));
    showUnlocked();
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
}

void MainWindow::lockVault() {
    autoLockTimer_->stop();
    std::filesystem::path root;
    if (vault_) {
        root = vault_->root_path();
        vault_->lock();
        vault_.reset();
    }
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
