#include "videovault/app/main_window.hpp"

#include "videovault/app/player_window.hpp"
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
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>
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
    if (thumbnailWatcher_ != nullptr) {
        thumbnailWatcher_->waitForFinished();
    }
    if (tagEditorWatcher_ != nullptr) {
        tagEditorWatcher_->waitForFinished();
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
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Slim explorer-style command bar.
    auto* toolbar = new QWidget(page);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 8, 12, 8);
    toolbarLayout->setSpacing(8);

    viewModeCombo_ = new QComboBox(toolbar);
    viewModeCombo_->addItem(QStringLiteral("Details"));
    viewModeCombo_->addItem(QStringLiteral("Large icons"));
    viewModeCombo_->addItem(QStringLiteral("List"));
    tagFilterCombo_ = new QComboBox(toolbar);
    tagFilterCombo_->setMinimumWidth(150);
    searchEdit_ = new QLineEdit(toolbar);
    searchEdit_->setPlaceholderText(QStringLiteral("Search tags or names…"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(220);
    importButton_ = new QPushButton(QStringLiteral("Import video…"), toolbar);
    importButton_->setProperty("primary", true);
    changePasswordButton_ = new QPushButton(QStringLiteral("Change password…"), toolbar);
    auto* lockButton = new QPushButton(QStringLiteral("Lock"), toolbar);
    lockButton->setToolTip(QStringLiteral("Lock the vault"));
    lockButton->setFlat(true);

    toolbarLayout->addWidget(viewModeCombo_);
    toolbarLayout->addWidget(tagFilterCombo_);
    toolbarLayout->addWidget(searchEdit_);
    toolbarLayout->addStretch(1);
    toolbarLayout->addWidget(importButton_);
    toolbarLayout->addWidget(changePasswordButton_);
    toolbarLayout->addWidget(lockButton);
    layout->addWidget(toolbar);

    // Explorer-style gallery views fill the window.
    detailsTree_ = new QTreeWidget(page);
    detailsTree_->setObjectName(QStringLiteral("gallery"));
    detailsTree_->setColumnCount(7);
    detailsTree_->setHeaderLabels({
        QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Duration"),
        QStringLiteral("Resolution"), QStringLiteral("Codec"), QStringLiteral("Tags"),
        QStringLiteral("Imported")});
    detailsTree_->setRootIsDecorated(false);
    detailsTree_->setAlternatingRowColors(true);
    detailsTree_->setUniformRowHeights(true);
    detailsTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    detailsTree_->header()->setStretchLastSection(true);

    iconList_ = new QListWidget(page);
    iconList_->setObjectName(QStringLiteral("gallery"));
    iconList_->setViewMode(QListView::IconMode);
    iconList_->setIconSize(QSize(128, 128));
    iconList_->setGridSize(QSize(172, 192));
    iconList_->setSpacing(8);
    iconList_->setWordWrap(true);
    iconList_->setResizeMode(QListView::Adjust);
    iconList_->setMovement(QListView::Static);
    iconList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    iconList_->setSelectionMode(QAbstractItemView::SingleSelection);

    galleryStack_ = new QStackedWidget(page);
    galleryStack_->addWidget(detailsTree_);
    galleryStack_->addWidget(iconList_);
    layout->addWidget(galleryStack_, 1);

    // Status bar like Explorer's.
    auto* statusBar = new QWidget(page);
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 4, 12, 4);
    statusLayout->setSpacing(12);
    unlockedLocation_ = new QLabel(statusBar);
    unlockedLocation_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    unlockedLocation_->setStyleSheet(QStringLiteral("color: #666666;"));
    statusCountLabel_ = new QLabel(statusBar);
    statusCountLabel_->setStyleSheet(QStringLiteral("color: #666666;"));
    galleryStatus_ = errorLabel(statusBar);
    statusLayout->addWidget(unlockedLocation_);
    statusLayout->addStretch(1);
    statusLayout->addWidget(statusCountLabel_);
    statusLayout->addWidget(galleryStatus_);
    layout->addWidget(statusBar);

    connect(lockButton, &QPushButton::clicked, this, [this] { lockVault(); });
    connect(importButton_, &QPushButton::clicked, this, [this] { beginImport(); });
    connect(changePasswordButton_, &QPushButton::clicked, this, [this] { beginChangePassword(); });
    connect(viewModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](const int index) { setViewMode(index); });
    connect(tagFilterCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](int) {
            tagFilterId_ = tagFilterCombo_->currentData().toLongLong();
            tagFilterName_ = tagFilterCombo_->currentData(Qt::UserRole + 1).toString();
            refreshGallery();
        });
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        searchText_ = text.trimmed();
        refreshGallery();
    });
    connect(detailsTree_, &QTreeWidget::itemDoubleClicked,
        this, [this](QTreeWidgetItem* item, int) {
            beginPlayback(item->data(0, Qt::UserRole).toLongLong());
        });
    connect(iconList_, &QListWidget::itemDoubleClicked,
        this, [this](QListWidgetItem* item) {
            beginPlayback(item->data(Qt::UserRole).toLongLong());
        });
    detailsTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    iconList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(detailsTree_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& position) {
            showGalleryContextMenu(detailsTree_->viewport()->mapToGlobal(position));
        });
    connect(iconList_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& position) {
            showGalleryContextMenu(iconList_->viewport()->mapToGlobal(position));
        });

    QSettings settings;
    const int saved_view = settings.value(QStringLiteral("gallery/viewMode"), 1).toInt();
    viewModeCombo_->setCurrentIndex(saved_view);
    setViewMode(saved_view);
    return page;
}

void MainWindow::setViewMode(const int index) {
    switch (index) {
    case 0: // Details
        galleryStack_->setCurrentWidget(detailsTree_);
        break;
    case 1: // Large icons
        iconList_->setViewMode(QListView::IconMode);
        iconList_->setIconSize(QSize(128, 128));
        iconList_->setGridSize(QSize(172, 192));
        galleryStack_->setCurrentWidget(iconList_);
        break;
    default: // List
        iconList_->setViewMode(QListView::ListMode);
        iconList_->setIconSize(QSize(32, 32));
        galleryStack_->setCurrentWidget(iconList_);
        break;
    }
    // Relayout from the top so icons never start half-clipped after a mode
    // switch (Qt IconMode keeps the old scroll offset and item layout).
    iconList_->scrollToTop();
    iconList_->doItemsLayout();
    QSettings settings;
    settings.setValue(QStringLiteral("gallery/viewMode"), index);
}

void MainWindow::refreshTagFilter() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto vault = vault_;
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<core::Result<std::vector<core::TagInfo>>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, vault, watcher] {
        const auto outcome = *watcher->result();
        watcher->deleteLater();
        if (vault_ != vault) {
            return;
        }
        const std::int64_t previous = tagFilterId_;
        tagFilterCombo_->blockSignals(true);
        tagFilterCombo_->clear();
        tagFilterCombo_->addItem(QStringLiteral("All videos"),
            QVariant::fromValue<qlonglong>(-1));
        tagFilterCombo_->setItemData(0, QString(), Qt::UserRole + 1);
        if (outcome) {
            for (const auto& tag : outcome.value()) {
                tagFilterCombo_->addItem(
                    QStringLiteral("%1 (%2)")
                        .arg(QString::fromStdString(tag.name))
                        .arg(tag.video_count),
                    QVariant::fromValue<qlonglong>(tag.id));
                tagFilterCombo_->setItemData(
                    tagFilterCombo_->count() - 1,
                    QString::fromStdString(tag.name), Qt::UserRole + 1);
            }
        }
        int index = 0;
        for (int i = 0; i < tagFilterCombo_->count(); ++i) {
            if (tagFilterCombo_->itemData(i).toLongLong() == previous) {
                index = i;
                break;
            }
        }
        tagFilterCombo_->setCurrentIndex(index);
        tagFilterCombo_->blockSignals(false);
        tagFilterId_ = tagFilterCombo_->itemData(index).toLongLong();
        tagFilterName_ = tagFilterCombo_->itemData(index, Qt::UserRole + 1).toString();
    });
    watcher->setFuture(QtConcurrent::run([vault] {
        return std::make_shared<core::Result<std::vector<core::TagInfo>>>(
            vault->list_tags());
    }));
}

void MainWindow::beginEditTags(const std::int64_t video_id) {
    if (!vault_ || !vault_->is_unlocked() || video_id < 0) {
        return;
    }
    const auto vault = vault_;
    if (tagEditorWatcher_ != nullptr) {
        tagEditorWatcher_->waitForFinished();
    }
    tagEditorWatcher_ = new QFutureWatcher<std::shared_ptr<TagEditorData>>(this);
    connect(tagEditorWatcher_, &QFutureWatcherBase::finished, this,
        [this] { finishEditTags(); });
    tagEditorWatcher_->setFuture(QtConcurrent::run([vault, video_id] {
        auto data = std::make_shared<TagEditorData>();
        data->video_id = video_id;
        auto all = vault->list_tags();
        if (all) {
            data->all_tags = all.value();
        }
        auto own = vault->tags_for_video(video_id);
        if (own) {
            data->video_tags = own.value();
        }
        return data;
    }));
}

void MainWindow::finishEditTags() {
    auto* completed = tagEditorWatcher_;
    tagEditorWatcher_ = nullptr;
    const auto data = completed->result();
    completed->deleteLater();
    if (!data || !vault_) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit tags"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* list = new QListWidget(&dialog);
    QSet<QString> applied;
    for (const auto& tag : data->video_tags) {
        applied.insert(QString::fromStdString(tag.name));
    }
    for (const auto& tag : data->all_tags) {
        auto* item = new QListWidgetItem(QString::fromStdString(tag.name), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(
            applied.contains(QString::fromStdString(tag.name))
                ? Qt::Checked : Qt::Unchecked);
    }

    auto* inputRow = new QHBoxLayout();
    auto* input = new QLineEdit(&dialog);
    input->setPlaceholderText(QStringLiteral("New tag…"));
    auto* addButton = new QPushButton(QStringLiteral("Add"), &dialog);
    inputRow->addWidget(input, 1);
    inputRow->addWidget(addButton);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(list, 1);
    layout->addLayout(inputRow);
    layout->addWidget(buttons);

    connect(addButton, &QPushButton::clicked, &dialog, [list, input] {
        const QString name = input->text().trimmed();
        if (name.isEmpty()) {
            return;
        }
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->text().compare(name, Qt::CaseInsensitive) == 0) {
                list->item(i)->setCheckState(Qt::Checked);
                list->setCurrentRow(i);
                input->clear();
                return;
            }
        }
        auto* item = new QListWidgetItem(name, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        list->setCurrentItem(item);
        input->clear();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QSet<QString> desired;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked) {
            desired.insert(list->item(i)->text());
        }
    }
    if (desired == applied) {
        return; // nothing changed
    }

    const auto vault = vault_;
    const auto video_id = data->video_id;
    importButton_->setEnabled(false);
    changePasswordButton_->setEnabled(false);
    adminWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(adminWatcher_, &QFutureWatcherBase::finished, this, [this] {
        auto* completed = adminWatcher_;
        adminWatcher_ = nullptr;
        const auto outcome = *completed->result();
        completed->deleteLater();
        importButton_->setEnabled(true);
        changePasswordButton_->setEnabled(true);
        if (!outcome) {
            setError(galleryStatus_,
                QString::fromUtf8(core::user_message(outcome.error().code).data()));
            galleryStatus_->setVisible(true);
            return;
        }
        refreshTagFilter();
        refreshGallery();
    });
    adminWatcher_->setFuture(QtConcurrent::run([vault, video_id, applied, desired] {
        for (const auto& name : desired) {
            if (!applied.contains(name)) {
                (void)vault->add_tag(video_id, name.toUtf8().constData());
            }
        }
        auto tags = vault->list_tags();
        if (tags) {
            for (const auto& tag : tags.value()) {
                const QString name = QString::fromStdString(tag.name);
                if (applied.contains(name) && !desired.contains(name)) {
                    (void)vault->remove_tag(video_id, tag.id);
                }
            }
        }
        return std::make_shared<core::Result<bool>>(true);
    }));
}

void MainWindow::showGalleryContextMenu(const QPoint& global_position) {
    std::int64_t video_id = -1;
    if (galleryStack_->currentWidget() == detailsTree_) {
        auto* item = detailsTree_->itemAt(
            detailsTree_->viewport()->mapFromGlobal(global_position));
        if (item != nullptr) {
            video_id = item->data(0, Qt::UserRole).toLongLong();
        }
    } else {
        auto* item = iconList_->itemAt(
            iconList_->viewport()->mapFromGlobal(global_position));
        if (item != nullptr) {
            video_id = item->data(Qt::UserRole).toLongLong();
        }
    }
    if (video_id < 0) {
        return;
    }
    QMenu menu(this);
    QAction* play = menu.addAction(QStringLiteral("Play"));
    QAction* editTags = menu.addAction(QStringLiteral("Edit tags…"));
    QAction* regenerate = menu.addAction(QStringLiteral("Regenerate thumbnail"));
    QAction* remove = menu.addAction(QStringLiteral("Remove"));
    QAction* chosen = menu.exec(global_position);
    if (chosen == play) {
        beginPlayback(video_id);
    } else if (chosen == editTags) {
        beginEditTags(video_id);
    } else if (chosen == regenerate) {
        beginGenerateThumbnail(video_id);
    } else if (chosen == remove) {
        beginRemoveSelected(video_id);
    }
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
    detailsTree_->clear();
    iconList_->clear();
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
        std::vector<core::VideoInfo> visible;
        visible.reserve(videos.size());
        for (const auto& video : videos) {
            bool tag_matches = tagFilterName_.isEmpty();
            if (!tag_matches) {
                for (const auto& tag : video.tags) {
                    if (QString::fromStdString(tag).compare(tagFilterName_, Qt::CaseInsensitive) == 0) {
                        tag_matches = true;
                        break;
                    }
                }
            }
            if (!tag_matches) {
                continue;
            }
            bool search_matches = searchText_.isEmpty();
            if (!search_matches) {
                for (const auto& tag : video.tags) {
                    if (QString::fromStdString(tag).contains(searchText_, Qt::CaseInsensitive)) {
                        search_matches = true;
                        break;
                    }
                }
                if (!search_matches && QString::fromStdString(video.display_name)
                        .contains(searchText_, Qt::CaseInsensitive)) {
                    search_matches = true;
                }
            }
            if (!search_matches) {
                continue;
            }
            visible.push_back(video);
        }
        std::uint64_t total_bytes = 0U;
        for (const auto& video : visible) {
            total_bytes += video.original_size;
            const auto video_id = video.id;
            const QString name = QString::fromStdString(video.display_name);
            const QString size_text = QStringLiteral("%1 MB").arg(
                static_cast<double>(video.original_size) / (1024.0 * 1024.0), 0, 'f', 2);
            QString tags_text;
            for (std::size_t i = 0U; i < video.tags.size(); ++i) {
                if (i > 0U) {
                    tags_text += QStringLiteral(", ");
                }
                tags_text += QString::fromStdString(video.tags[i]);
            }
            const QString imported_text = QString::fromStdString(
                std::to_string(video.imported_at));

            // Icon/list view item.
            auto* iconItem = new QListWidgetItem(name, iconList_);
            iconItem->setData(Qt::UserRole, static_cast<qlonglong>(video_id));
            iconItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);

            // Details view item.
            auto* treeItem = new QTreeWidgetItem(detailsTree_);
            treeItem->setText(0, name);
            treeItem->setText(1, size_text);
            treeItem->setText(2, QStringLiteral("…"));
            treeItem->setText(5, tags_text);
            treeItem->setText(6, imported_text);
            treeItem->setData(0, Qt::UserRole, static_cast<qlonglong>(video_id));

            // Attach a stored thumbnail asynchronously (both views).
            auto* thumbWatcher =
                new QFutureWatcher<std::shared_ptr<core::Result<core::ThumbnailInfo>>>(this);
            connect(thumbWatcher, &QFutureWatcherBase::finished, this,
                [this, vault, video_id, iconItem, treeItem, thumbWatcher] {
                    const auto outcome = *thumbWatcher->result();
                    thumbWatcher->deleteLater();
                    if (!outcome || outcome.value().bytes.empty()) {
                        return;
                    }
                    QPixmap pixmap;
                    if (pixmap.loadFromData(outcome.value().bytes.data(),
                            static_cast<int>(outcome.value().bytes.size()))) {
                        iconItem->setIcon(QIcon(pixmap.scaled(
                            128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                        treeItem->setIcon(0, QIcon(pixmap.scaled(
                            32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                    }
                });
            thumbWatcher->setFuture(QtConcurrent::run([vault, video_id] {
                return std::make_shared<core::Result<core::ThumbnailInfo>>(
                    vault->thumbnail(video_id));
            }));

            // Media metadata for the details columns, asynchronously.
            auto* mediaWatcher =
                new QFutureWatcher<std::shared_ptr<core::Result<core::MediaInfo>>>(this);
            connect(mediaWatcher, &QFutureWatcherBase::finished, this,
                [this, vault, video_id, treeItem, mediaWatcher] {
                    const auto outcome = *mediaWatcher->result();
                    mediaWatcher->deleteLater();
                    if (!outcome) {
                        return;
                    }
                    const core::MediaInfo& info = outcome.value();
                    const std::int64_t duration = info.duration_ms;
                    treeItem->setText(2, QStringLiteral("%1:%2")
                        .arg(duration / 60000)
                        .arg((duration / 1000) % 60, 2, 10, QLatin1Char('0')));
                    treeItem->setText(3, QStringLiteral("%1 × %2")
                        .arg(info.width).arg(info.height));
                    treeItem->setText(4, QString::fromStdString(info.codec_name));
                });
            mediaWatcher->setFuture(QtConcurrent::run([vault, video_id] {
                return std::make_shared<core::Result<core::MediaInfo>>(
                    vault->media_info(video_id));
            }));
        }
        if (videos.empty() && tagFilterName_.isEmpty() && searchText_.isEmpty()) {
            setError(galleryStatus_, QStringLiteral("No videos imported yet."));
        } else if (visible.empty()) {
            setError(galleryStatus_, QStringLiteral("No videos match the current search or filter."));
        }
        statusCountLabel_->setText(QStringLiteral("%1 videos · %2 MB")
            .arg(visible.size())
            .arg(static_cast<double>(total_bytes) / (1024.0 * 1024.0), 0, 'f', 1));
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

std::int64_t MainWindow::selectedVideoId() const {
    if (galleryStack_->currentWidget() == detailsTree_) {
        auto* item = detailsTree_->currentItem();
        if (item == nullptr) {
            return -1;
        }
        return item->data(0, Qt::UserRole).toLongLong();
    }
    auto* item = iconList_->currentItem();
    if (item == nullptr) {
        return -1;
    }
    return item->data(Qt::UserRole).toLongLong();
}

void MainWindow::beginPlayback(const std::int64_t video_id) {
    if (!vault_ || !vault_->is_unlocked() || video_id < 0) {
        return;
    }
    if (playerWindow_ != nullptr) {
        playerWindow_->raise();
        playerWindow_->activateWindow();
        return;
    }
    auto* window = new PlayerWindow(
        vault_, video_id, QStringLiteral("Video"), this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &QDialog::destroyed, this,
        [this, window] {
            if (playerWindow_ == window) {
                playerWindow_ = nullptr;
            }
        });
    playerWindow_ = window;
    window->show();
}

void MainWindow::beginRemoveSelected(const std::int64_t video_id) {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto target = video_id >= 0 ? video_id : selectedVideoId();
    if (target < 0) {
        setError(galleryStatus_, QStringLiteral("Select a video to remove."));
        galleryStatus_->setVisible(true);
        return;
    }
    const auto vault = vault_;
    importButton_->setEnabled(false);
    changePasswordButton_->setEnabled(false);
    adminWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(adminWatcher_, &QFutureWatcherBase::finished, this, [this] { finishRemoveSelected(); });
    adminWatcher_->setFuture(QtConcurrent::run([vault, target] {
        return std::make_shared<core::Result<bool>>(vault->remove_video(target));
    }));
}

void MainWindow::finishRemoveSelected() {
    auto* completed = adminWatcher_;
    adminWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    importButton_->setEnabled(true);
    changePasswordButton_->setEnabled(true);
    if (!outcome) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    refreshTagFilter();
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
    changePasswordButton_->setEnabled(true);
    if (!outcome) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    setError(galleryStatus_, QStringLiteral("Password changed."));
    galleryStatus_->setVisible(true);
}

void MainWindow::beginGenerateThumbnail(const std::int64_t video_id) {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto target = video_id >= 0 ? video_id : selectedVideoId();
    if (target < 0) {
        setError(galleryStatus_, QStringLiteral("Select a video to thumbnail."));
        galleryStatus_->setVisible(true);
        return;
    }
    const auto vault = vault_;
    importButton_->setEnabled(false);
    changePasswordButton_->setEnabled(false);
    galleryStatus_->setText(QStringLiteral("Generating thumbnail…"));
    galleryStatus_->setVisible(true);
    thumbnailWatcher_ =
        new QFutureWatcher<std::shared_ptr<core::Result<core::ThumbnailInfo>>>(this);
    connect(thumbnailWatcher_, &QFutureWatcherBase::finished, this,
        [this] { finishGenerateThumbnail(); });
    thumbnailWatcher_->setFuture(QtConcurrent::run([vault, target] {
        return std::make_shared<core::Result<core::ThumbnailInfo>>(
            vault->generate_thumbnail(target, 320U));
    }));
}

void MainWindow::finishGenerateThumbnail() {
    auto* completed = thumbnailWatcher_;
    thumbnailWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    importButton_->setEnabled(true);
    changePasswordButton_->setEnabled(true);
    if (!outcome) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    refreshGallery();
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
    tagFilterId_ = -1;
    tagFilterName_.clear();
    searchText_.clear();
    searchEdit_->clear();
    refreshTagFilter();
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
    changePasswordButton_->setEnabled(true);
    detailsTree_->clear();
    iconList_->clear();
    tagFilterId_ = -1;
    tagFilterName_.clear();
    searchText_.clear();
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
