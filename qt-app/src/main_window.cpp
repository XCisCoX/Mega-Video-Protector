#include "videovault/app/batch_worker.hpp"
#include "videovault/app/glass.hpp"
#include "videovault/app/main_window.hpp"
#include "videovault/app/player_window.hpp"
#include "videovault/app/settings_dialog.hpp"
#include "videovault/app/share_server.hpp"
#include "videovault/core/vault.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QAction>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QStandardPaths>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QDateTime>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QProgressBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
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
        // Portable best-effort wipe the optimizer cannot elide. Replaces the
        // former Windows-only SecureZeroMemory so the app builds everywhere.
        volatile unsigned char* bytes =
            reinterpret_cast<volatile unsigned char*>(secret.data());
        for (std::size_t i = 0; i < secret.size(); ++i) {
            bytes[i] = 0U;
        }
        secret.clear();
        secret.shrink_to_fit();
    }
}

// Gallery items are recreated from scratch on every refreshGallery() (search
// keystrokes, tag filters, deletes...), so raw QTreeWidgetItem* /
// QListWidgetItem* captured by in-flight async watchers dangle as soon as the
// tree is cleared. Resolve items by their stored video id at completion time
// instead of keeping pointers across the async boundary.
QTreeWidgetItem* findTreeItemById(QTreeWidget* tree, const std::int64_t video_id) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto* item = tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toLongLong() == video_id) {
            return item;
        }
    }
    return nullptr;
}

QListWidgetItem* findListItemById(QListWidget* list, const std::int64_t video_id) {
    for (int i = 0; i < list->count(); ++i) {
        auto* item = list->item(i);
        if (item->data(Qt::UserRole).toLongLong() == video_id) {
            return item;
        }
    }
    return nullptr;
}

// Icon/list cards show the video name plus, in large-icon (IconMode) view, the
// tags underneath so tag-based searches are visible at a glance. The compact
// List mode keeps the single-line name. The display text is derived from data
// roles so it can be rebuilt when the user switches view modes.
void updateIconItemText(QListWidgetItem* item) {
    const QString name = item->data(Qt::UserRole + 1).toString();
    // Icon mode is a picture grid. The name stays in the tooltip, not under the tile.
    if (item->listWidget() != nullptr
        && item->listWidget()->viewMode() == QListView::IconMode) {
        item->setText(QString());
        return;
    }
    item->setText(name);
}

void layoutInstagramGrid(QListWidget* list) {
    if (list == nullptr || list->viewMode() != QListView::IconMode) {
        return;
    }
    // Phone tiles are about a third of a narrow screen. On the desktop that
    // same size means more columns, not three giant squares.
    const int width = std::max(1, list->viewport()->width());
    const int columns = std::max(5, width / 124);
    const int cell = std::max(72, width / columns);
    if (list->gridSize() == QSize(cell, cell)) {
        return;
    }
    list->setGridSize(QSize(cell, cell));
    list->setIconSize(QSize(cell, cell));
}

// Square cover tiles, a hairline gap, and a small duration on the picture.
class InstagramDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override {
        const auto* view = qobject_cast<const QListView*>(option.widget);
        if (view != nullptr && view->viewMode() == QListView::ListMode) {
            painter->save();
            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
            const QRect row = option.rect;
            const bool selected = option.state.testFlag(QStyle::State_Selected);
            const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
            if (selected) {
                painter->fillRect(row, QColor(51, 144, 236, 70));
            } else if (hovered) {
                painter->fillRect(row, QColor(255, 255, 255, 16));
            }
            const QRect thumb(row.left() + 14, row.top() + (row.height() - 44) / 2, 44, 44);
            painter->fillRect(thumb, QColor(28, 28, 30));
            const QPixmap source = index.data(Qt::UserRole + 4).value<QPixmap>();
            if (!source.isNull()) {
                const QPixmap scaled = source.scaled(
                    thumb.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                const int cropX = std::max(0, (scaled.width() - thumb.width()) / 2);
                const int cropY = std::max(0, (scaled.height() - thumb.height()) / 2);
                painter->drawPixmap(thumb, scaled, QRect(cropX, cropY, thumb.width(), thumb.height()));
            }
            const int textLeft = thumb.right() + 12;
            const QRect textRect(textLeft, row.top() + 8, row.width() - textLeft - 16, row.height() - 16);
            QFont nameFont(QStringLiteral("Segoe UI"));
            nameFont.setPointSize(10);
            painter->setFont(nameFont);
            const QString name = index.data(Qt::UserRole + 1).toString();
            const QFontMetrics nameMetrics(nameFont);
            painter->setPen(Qt::white);
            painter->drawText(
                QRect(textRect.left(), textRect.top(), textRect.width(), nameMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                nameMetrics.elidedText(name, Qt::ElideRight, textRect.width()));
            const QString sizeText = index.data(Qt::UserRole + 5).toString();
            const QString tags = index.data(Qt::UserRole + 2).toString();
            const QString meta = tags.isEmpty() ? sizeText : QStringLiteral("%1  ·  %2").arg(sizeText, tags);
            QFont metaFont(QStringLiteral("Segoe UI"));
            metaFont.setPointSize(9);
            painter->setFont(metaFont);
            const QFontMetrics metaMetrics(metaFont);
            painter->setPen(QColor(142, 142, 147));
            painter->drawText(
                QRect(textRect.left(), textRect.bottom() - metaMetrics.height(), textRect.width(), metaMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                metaMetrics.elidedText(meta, Qt::ElideRight, textRect.width()));
            painter->restore();
            return;
        }
        if (view == nullptr || view->viewMode() != QListView::IconMode) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QRect tile = option.rect.adjusted(1, 1, -2, -2);
        painter->fillRect(tile, QColor(22, 22, 24));
        const QPixmap source = index.data(Qt::UserRole + 4).value<QPixmap>();
        if (!source.isNull()) {
            const QPixmap scaled = source.scaled(
                tile.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            const int cropX = std::max(0, (scaled.width() - tile.width()) / 2);
            const int cropY = std::max(0, (scaled.height() - tile.height()) / 2);
            painter->drawPixmap(
                tile, scaled, QRect(cropX, cropY, tile.width(), tile.height()));
        }
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (hovered && !selected) {
            painter->fillRect(tile, QColor(255, 255, 255, 28));
        }
        if (selected) {
            painter->fillRect(tile, QColor(0, 0, 0, 70));
            painter->setPen(QPen(QColor(51, 144, 236), 3));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(tile.adjusted(1, 1, -2, -2));
        }
        const QString duration = index.data(Qt::UserRole + 3).toString();
        if (!duration.isEmpty()) {
            QFont font = painter->font();
            font.setPixelSize(12);
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            const QRect text = tile.adjusted(8, 0, -8, -6);
            painter->setPen(QColor(0, 0, 0, 180));
            painter->drawText(text.translated(0, 1), Qt::AlignLeft | Qt::AlignBottom, duration);
            painter->setPen(Qt::white);
            painter->drawText(text, Qt::AlignLeft | Qt::AlignBottom, duration);
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto* view = qobject_cast<const QListView*>(option.widget);
        if (view != nullptr && view->viewMode() == QListView::ListMode) {
            return QSize(std::max(1, option.rect.width()), 62);
        }
        if (view != nullptr && view->viewMode() == QListView::IconMode
            && view->gridSize().isValid()) {
            return view->gridSize();
        }
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

class IconGrid final : public QListWidget {
public:
    using QListWidget::QListWidget;

    void setChromeInset(const int top, const int bottom) {
        if (chromeTop_ == top && chromeBottom_ == bottom) {
            return;
        }
        chromeTop_ = top;
        chromeBottom_ = bottom;
        setViewportMargins(0, top, 0, bottom);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QListWidget::resizeEvent(event);
        layoutInstagramGrid(this);
    }

private:
    int chromeTop_{-1};
    int chromeBottom_{-1};
};

enum class CaptionGlyph { Minimize, Maximize, Restore, Close };

class CaptionButton final : public QAbstractButton {
public:
    explicit CaptionButton(const CaptionGlyph glyph, QWidget* parent = nullptr)
        : QAbstractButton(parent), glyph_(glyph) {
        setFixedSize(46, 32);
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
    }

    void setGlyph(const CaptionGlyph glyph) {
        glyph_ = glyph;
        update();
    }

protected:
    void enterEvent(QEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool close = glyph_ == CaptionGlyph::Close;
        if (underMouse()) {
            painter.fillRect(rect(), close ? QColor(232, 17, 35) : QColor(255, 255, 255, 24));
        }
        painter.setPen(QPen(Qt::white, 1.0));
        painter.setBrush(Qt::NoBrush);
        const QPointF center(width() / 2.0, height() / 2.0);
        switch (glyph_) {
        case CaptionGlyph::Minimize:
            painter.drawLine(QPointF(center.x() - 5, center.y()), QPointF(center.x() + 5, center.y()));
            break;
        case CaptionGlyph::Maximize:
            painter.drawRoundedRect(QRectF(center.x() - 5, center.y() - 5, 10, 10), 1.5, 1.5);
            break;
        case CaptionGlyph::Restore: {
            painter.drawRoundedRect(QRectF(center.x() - 5, center.y() - 2, 8, 8), 1.2, 1.2);
            painter.drawLine(QPointF(center.x() - 2, center.y() - 4), QPointF(center.x() + 5, center.y() - 4));
            painter.drawLine(QPointF(center.x() + 5, center.y() - 4), QPointF(center.x() + 5, center.y() + 3));
            break;
        }
        case CaptionGlyph::Close:
            painter.drawLine(QPointF(center.x() - 5, center.y() - 5), QPointF(center.x() + 5, center.y() + 5));
            painter.drawLine(QPointF(center.x() + 5, center.y() - 5), QPointF(center.x() - 5, center.y() + 5));
            break;
        }
    }

private:
    CaptionGlyph glyph_;
};

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
#ifdef Q_OS_WIN
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint
        | Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
#endif
    resize(1080, 700);
    setMinimumSize(780, 520);
    setAcceptDrops(true); // drag & drop video import

    autoLockTimer_->setSingleShot(true);
    autoLockTimer_->setInterval(kAutoLockMilliseconds);
    connect(autoLockTimer_, &QTimer::timeout, this, [this] {
        // Never auto-lock in the middle of an import/restore/remove batch:
        // lock() blocks on the vault mutex a worker currently holds and the
        // batch would fail with "vault is locked".
        // A running share keeps its own vault, so locking this window does
        // not cut the phone off. Skip only while a batch holds the UI vault.
        if (!batchBusy_) {
            lockVault();
        }
    });
    // Batch worker thread for imports/restores: the UI thread never blocks;
    // progress and completion arrive as queued signals.
    batchThread_ = new QThread(this);
    batchWorker_ = new BatchWorker();
    batchWorker_->moveToThread(batchThread_);
    connect(batchThread_, &QThread::finished, batchWorker_, &QObject::deleteLater);
    connect(batchWorker_, &BatchWorker::progress, this,
        [this](const qlonglong done, const qlonglong total) {
            if (progressBar_ != nullptr && total > 0) {
                progressBar_->setRange(0, 1000);
                progressBar_->setValue(static_cast<int>((done * 1000) / total));
            }
        });
    connect(batchWorker_, &BatchWorker::fileFinished, this,
        [this](const int done, const int total) {
            galleryStatus_->setText(QStringLiteral("%1 %2 / %3…")
                .arg(batchLabel_, QString::number(done), QString::number(total)));
        });
    connect(batchWorker_, &BatchWorker::finished, this,
        [this](const bool ok, const QString& message, const int count) {
            finishBatch(ok, message, count);
        });
    batchThread_->start();
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
    if (shareServer_) {
        shareServer_->setOnRunning({});
        shareServer_.reset();
    }
    // The player is an unparented top-level window (so it can be covered by
    // the main window), so close it explicitly to stop its worker thread
    // before the vault goes away.
    if (playerWindow_ != nullptr) {
        playerWindow_->close();
    }
    // Stop the batch worker before the vault goes away: quit the event loop,
    // then wait for the current batch item to finish (bounded).
    if (batchThread_ != nullptr) {
        batchThread_->quit();
        batchThread_->wait(5000);
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
    if (galleryStack_ != nullptr && watched == galleryStack_->parentWidget()
        && event->type() == QEvent::Resize) {
        layoutLibraryChrome();
    }
    if (event->type() == QEvent::KeyPress && vault_ && vault_->is_unlocked()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && key->modifiers() == Qt::NoModifier) {
            QWidget* focus = QApplication::focusWidget();
            bool inGallery = false;
            for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
                if (qobject_cast<QLineEdit*>(widget) != nullptr
                    || qobject_cast<QComboBox*>(widget) != nullptr
                    || qobject_cast<QPushButton*>(widget) != nullptr) {
                    inGallery = false;
                    break;
                }
                if (widget == detailsTree_ || widget == iconList_) {
                    inGallery = true;
                    break;
                }
            }
            if (inGallery) {
                const std::int64_t videoId = selectedVideoId();
                if (videoId >= 0) {
                    beginPlayback(videoId);
                    return true;
                }
            }
        }
    }
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

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) {
        if (captionBar_ != nullptr) {
            captionBar_->setVisible(!isFullScreen());
        }
        if (captionMax_ != nullptr) {
            static_cast<CaptionButton*>(captionMax_)->setGlyph(
                isMaximized() ? CaptionGlyph::Restore : CaptionGlyph::Maximize);
        }
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    layoutInstagramGrid(iconList_);
#ifdef Q_OS_WIN
    if (!frameReady_) {
        frameReady_ = true;
        HWND hwnd = reinterpret_cast<HWND>(winId());
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        style |= WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        const MARGINS shadow{0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(hwnd, &shadow);
        const int corner = 2; // DWMWCP_ROUND, Windows 11
        DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
            // The system hands a maximized window a rect that spills past the
            // monitor. Inset it by the frame so it sits on the work area
            // instead of sliding under the taskbar or leaving a gap.
            if (IsZoomed(msg->hwnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                const int pad = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                params->rgrc[0].left += pad;
                params->rgrc[0].top += pad;
                params->rgrc[0].right -= pad;
                params->rgrc[0].bottom -= pad;
            }
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCHITTEST) {
            const QPoint global(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
            const QPoint pos = mapFromGlobal(global);
            const bool zoomed = IsZoomed(msg->hwnd);
            // Buttons win over the resize border, or the close control cannot be clicked.
            if (captionBar_ != nullptr) {
                const QPoint inCaption = captionBar_->mapFromGlobal(global);
                if (captionBar_->rect().contains(inCaption)) {
                    QWidget* child = captionBar_->childAt(inCaption);
                    if (qobject_cast<QAbstractButton*>(child) != nullptr) {
                        *result = HTCLIENT;
                    } else if (!zoomed && inCaption.y() < 4) {
                        *result = HTTOP;
                    } else {
                        *result = HTCAPTION;
                    }
                    return true;
                }
            }
            if (!zoomed) {
                constexpr int border = 6;
                const bool left = pos.x() < border;
                const bool right = pos.x() >= width() - border;
                const bool bottom = pos.y() >= height() - border;
                if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
                if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
                if (left) { *result = HTLEFT; return true; }
                if (right) { *result = HTRIGHT; return true; }
                if (bottom) { *result = HTBOTTOM; return true; }
            }
        }
        if (msg->message == WM_GETMINMAXINFO) {
            auto* info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
            info->ptMinTrackSize.x = minimumWidth();
            info->ptMinTrackSize.y = minimumHeight();
            return false;
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::buildInterface() {
    auto* shell = new QWidget(this);
    auto* shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);

#ifdef Q_OS_WIN
    captionBar_ = new QWidget(shell);
    captionBar_->setFixedHeight(32);
    captionBar_->setObjectName(QStringLiteral("captionBar"));
    captionBar_->setAttribute(Qt::WA_StyledBackground, true);
    captionBar_->setStyleSheet(QStringLiteral(
        "QWidget#captionBar { background: #000000; border: none; }"));
    auto* captionLayout = new QHBoxLayout(captionBar_);
    captionLayout->setContentsMargins(14, 0, 0, 0);
    captionLayout->setSpacing(0);
    auto* captionTitle = new QLabel(QStringLiteral("Mega Video Protect"), captionBar_);
    captionTitle->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QFont captionFont(QStringLiteral("Segoe UI"));
    captionFont.setPointSize(9);
    captionTitle->setFont(captionFont);
    captionTitle->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 210); background: transparent;"));
    captionLayout->addWidget(captionTitle);
    captionLayout->addStretch(1);
    auto* minimize = new CaptionButton(CaptionGlyph::Minimize, captionBar_);
    captionMax_ = new CaptionButton(CaptionGlyph::Maximize, captionBar_);
    auto* closeButton = new CaptionButton(CaptionGlyph::Close, captionBar_);
    captionLayout->addWidget(minimize);
    captionLayout->addWidget(captionMax_);
    captionLayout->addWidget(closeButton);
    connect(minimize, &QAbstractButton::clicked, this, [this] { showMinimized(); });
    connect(captionMax_, &QAbstractButton::clicked, this, [this] {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    connect(closeButton, &QAbstractButton::clicked, this, &QWidget::close);
    shellLayout->addWidget(captionBar_);
#endif

    contentStack_ = new QStackedWidget(shell);
    pages_ = new QStackedWidget(contentStack_);
    setupPage_ = buildSetupPage();
    loginPage_ = buildLoginPage();
    unlockedPage_ = buildUnlockedPage();
    pages_->addWidget(setupPage_);
    pages_->addWidget(loginPage_);
    pages_->addWidget(unlockedPage_);
    contentStack_->addWidget(pages_);
    shellLayout->addWidget(contentStack_, 1);
    setCentralWidget(shell);
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

#ifdef Q_OS_ANDROID
    // Android has no desktop file dialogs and no user-visible filesystem;
    // the vault lives in the app-private sandbox (no storage permission
    // needed, wiped with the app). The Browse buttons become no-ops because
    // QFileDialog returns an empty path on Android.
    const QString sandbox =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    setupLocation_->setText(QDir::toNativeSeparators(sandbox));
    loginLocation_->setText(QDir::toNativeSeparators(sandbox));
#endif

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
    page->installEventFilter(this);

    // Translucent command bar. Icon and list views scroll underneath it.
    libraryTop_ = new FrostedBar(FrostedBar::Band::Top, page);
    auto* toolbar = libraryTop_;
    auto* toolbarLayout = new QVBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(16, 10, 12, 10);
    toolbarLayout->setSpacing(8);

    viewModeCombo_ = new QComboBox(toolbar);
    viewModeCombo_->addItem(QStringLiteral("Details"));
    viewModeCombo_->addItem(QStringLiteral("Icons"));
    viewModeCombo_->addItem(QStringLiteral("List"));
    viewModeCombo_->hide();
    tagFilterCombo_ = new QComboBox(toolbar);
    tagFilterCombo_->setMinimumWidth(150);
    searchEdit_ = new QLineEdit(toolbar);
    searchEdit_->setPlaceholderText(QStringLiteral("Search tags or names…"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(220);
    importButton_ = new QPushButton(QStringLiteral("Import"), toolbar);
    importButton_->setProperty("primary", true);
    importButton_->setToolTip(QStringLiteral("Import a video or picture"));
    importFolderButton_ = new QPushButton(QStringLiteral("Folder"), toolbar);
    importFolderButton_->setToolTip(QStringLiteral(
        "Import every video file from a folder (and drop files here to import)"));
    shareButton_ = new QPushButton(QStringLiteral("Share"), toolbar);
    shareButton_->setToolTip(QStringLiteral(
        "Share this vault on Wi-Fi over an encrypted connection. The phone must enter the vault password. "
        "Locking this PC does not stop sharing."));
    settingsButton_ = new QPushButton(QStringLiteral("Settings"), toolbar);
    settingsButton_->setToolTip(QStringLiteral(
        "Change the password, manage tags, and adjust player settings"));
    auto* lockButton = new QPushButton(QStringLiteral("Lock"), toolbar);
    lockButton->setToolTip(QStringLiteral(
        "Lock this PC. A phone that already entered the password keeps access until you stop sharing."));

    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(8);
    auto* libraryTitle = new QLabel(QStringLiteral("Library"), toolbar);
    QFont libraryFont(QStringLiteral("Segoe UI"));
    libraryFont.setPointSize(16);
    libraryTitle->setFont(libraryFont);
    libraryTitle->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
    titleRow->addWidget(libraryTitle);
    titleRow->addStretch(1);
    titleRow->addWidget(importButton_);
    titleRow->addWidget(importFolderButton_);
    titleRow->addWidget(shareButton_);
    titleRow->addWidget(settingsButton_);
    titleRow->addWidget(lockButton);
    toolbarLayout->addLayout(titleRow);

    searchEdit_->setPlaceholderText(QStringLiteral("Search"));
    searchEdit_->setMinimumWidth(160);
    auto* detailsMode = new QPushButton(QStringLiteral("Details"), toolbar);
    auto* iconsMode = new QPushButton(QStringLiteral("Icons"), toolbar);
    auto* listMode = new QPushButton(QStringLiteral("List"), toolbar);
    viewModeGroup_ = new QButtonGroup(toolbar);
    viewModeGroup_->setExclusive(true);
    const QString modeStyle = QStringLiteral(
        "QPushButton { min-height: 32px; padding: 0 12px; border-radius: 16px;"
        " background: transparent; border: none; color: #8e8e93; }"
        "QPushButton:checked { background: rgba(51, 144, 236, 90); color: white; }");
    int modeId = 0;
    for (auto* button : {detailsMode, iconsMode, listMode}) {
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(modeStyle);
        viewModeGroup_->addButton(button, modeId);
        ++modeId;
    }
    sortButton_ = new QPushButton(toolbar);
    sortButton_->setFocusPolicy(Qt::NoFocus);
    sortButton_->setCursor(Qt::PointingHandCursor);
    sortButton_->setStyleSheet(QStringLiteral(
        "QPushButton { min-height: 32px; padding: 0 12px; border-radius: 16px;"
        " background: transparent; border: none; color: #3390ec; }"));
    auto* filterRow = new QHBoxLayout();
    filterRow->setSpacing(8);
    filterRow->addWidget(searchEdit_, 1);
    filterRow->addWidget(detailsMode);
    filterRow->addWidget(iconsMode);
    filterRow->addWidget(listMode);
    filterRow->addWidget(sortButton_);
    filterRow->addWidget(tagFilterCombo_);
    toolbarLayout->addLayout(filterRow);

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
    detailsTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    detailsTree_->setAcceptDrops(false);
    detailsTree_->header()->setStretchLastSection(true);
    detailsTree_->setSortingEnabled(true);
    iconList_ = new IconGrid(page);
    iconList_->setObjectName(QStringLiteral("gallery"));
    iconList_->setItemDelegate(new InstagramDelegate(iconList_));
    iconList_->setViewMode(QListView::IconMode);
    iconList_->setSpacing(0);
    iconList_->setWordWrap(false);
    iconList_->setResizeMode(QListView::Adjust);
    iconList_->setMovement(QListView::Static);
    iconList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    iconList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    iconList_->setAcceptDrops(false);

    galleryStack_ = new QStackedWidget(page);
    galleryStack_->addWidget(detailsTree_);
    galleryStack_->addWidget(iconList_);

    libraryBottom_ = new FrostedBar(FrostedBar::Band::Bottom, page);
    auto* statusBar = libraryBottom_;
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 4, 12, 4);
    statusLayout->setSpacing(12);
    unlockedLocation_ = new QLabel(statusBar);
    unlockedLocation_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    unlockedLocation_->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180);"));
    statusCountLabel_ = new QLabel(statusBar);
    statusCountLabel_->setStyleSheet(QStringLiteral("color: rgba(255, 255, 255, 180);"));
    progressBar_ = new QProgressBar(statusBar);
    progressBar_->setFixedWidth(200);
    progressBar_->setTextVisible(true);
    progressBar_->setFormat(QStringLiteral("%v / %m"));
    progressBar_->hide();
    galleryStatus_ = errorLabel(statusBar);
    statusLayout->addWidget(unlockedLocation_);
    statusLayout->addStretch(1);
    statusLayout->addWidget(progressBar_);
    statusLayout->addWidget(statusCountLabel_);
    statusLayout->addWidget(galleryStatus_);

    connect(lockButton, &QPushButton::clicked, this, [this] { lockVault(); });
    connect(importButton_, &QPushButton::clicked, this, [this] { beginImport(); });
    connect(importFolderButton_, &QPushButton::clicked, this, [this] { beginImportFolder(); });
    connect(shareButton_, &QPushButton::clicked, this, [this] { openShare(); });
    connect(settingsButton_, &QPushButton::clicked, this, [this] { openSettings(); });
    connect(viewModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
        this, [this](const int index) { setViewMode(index); });
    connect(viewModeGroup_, QOverload<int>::of(&QButtonGroup::buttonClicked),
        this, [this](const int index) {
            if (viewModeCombo_->currentIndex() != index) {
                viewModeCombo_->setCurrentIndex(index);
            }
        });
    connect(sortButton_, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        const QStringList labels{
            QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Duration"),
            QStringLiteral("Resolution"), QStringLiteral("Codec"), QStringLiteral("Tags"),
            QStringLiteral("Imported")};
        for (int i = 0; i < labels.size(); ++i) {
            QString text = labels.at(i);
            if (i == sortKey_) {
                text += sortAscending_ ? QStringLiteral("  ↑") : QStringLiteral("  ↓");
            }
            QAction* action = menu.addAction(text);
            action->setData(i);
        }
        QAction* chosen = menu.exec(sortButton_->mapToGlobal(QPoint(0, sortButton_->height())));
        if (chosen != nullptr) {
            applyGallerySort(chosen->data().toInt());
        }
    });
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
    sortKey_ = settings.value(QStringLiteral("gallery/sortKey"), 0).toInt();
    sortAscending_ = settings.value(QStringLiteral("gallery/sortAscending"), true).toBool();
    if (sortKey_ < 0 || sortKey_ > 6) {
        sortKey_ = 0;
    }
    updateSortButton();
    const int saved_view = settings.value(QStringLiteral("gallery/viewMode"), 1).toInt();
    viewModeCombo_->setCurrentIndex(saved_view);
    setViewMode(saved_view);
    layoutLibraryChrome();
    return page;
}

void MainWindow::layoutLibraryChrome() {
    QWidget* page = galleryStack_ != nullptr ? galleryStack_->parentWidget() : nullptr;
    if (page == nullptr || libraryTop_ == nullptr || libraryBottom_ == nullptr) {
        return;
    }
    const int topHeight = std::max(libraryTop_->sizeHint().height(), 44);
    const int bottomHeight = std::max(libraryBottom_->sizeHint().height(), 28);
    const bool details = galleryStack_->currentWidget() == detailsTree_;
    if (details) {
        libraryTop_->setGeometry(0, 0, page->width(), topHeight);
        libraryBottom_->setGeometry(0, std::max(0, page->height() - bottomHeight), page->width(), bottomHeight);
        galleryStack_->setGeometry(
            0, topHeight, page->width(), std::max(0, page->height() - topHeight - bottomHeight));
    } else {
        galleryStack_->setGeometry(page->rect());
        libraryTop_->setGeometry(0, 0, page->width(), topHeight);
        libraryBottom_->setGeometry(0, std::max(0, page->height() - bottomHeight), page->width(), bottomHeight);
        galleryStack_->lower();
    }
    libraryTop_->raise();
    libraryBottom_->raise();
    // Keep the first and last rows fully visible. The bars float over the
    // gallery, so without this inset the top icons sit halfway underneath.
    if (iconList_ != nullptr) {
        static_cast<IconGrid*>(iconList_)->setChromeInset(
            details ? 0 : topHeight, details ? 0 : bottomHeight);
    }
    auto* area = qobject_cast<QAbstractScrollArea*>(galleryStack_->currentWidget());
    libraryTop_->follow(area);
    libraryBottom_->follow(area);
}

void MainWindow::setViewMode(const int index) {
    switch (index) {
    case 0: // Details
        galleryStack_->setCurrentWidget(detailsTree_);
        break;
    case 1: // Icons: three square columns, picture only.
        iconList_->setViewMode(QListView::IconMode);
        iconList_->setSpacing(0);
        iconList_->setWordWrap(false);
        galleryStack_->setCurrentWidget(iconList_);
        layoutInstagramGrid(iconList_);
        break;
    default: // List
        iconList_->setViewMode(QListView::ListMode);
        iconList_->setIconSize(QSize(44, 44));
        iconList_->setGridSize(QSize());
        iconList_->setSpacing(0);
        iconList_->setUniformItemSizes(true);
        galleryStack_->setCurrentWidget(iconList_);
        break;
    }
    if (index == 1) {
        iconList_->setUniformItemSizes(false);
    }
    if (sortButton_ != nullptr) {
        sortButton_->setVisible(index != 0);
    }
    if (viewModeGroup_ != nullptr) {
        if (QAbstractButton* button = viewModeGroup_->button(index)) {
            button->setChecked(true);
        }
    }
    // Large-icon cards carry a name + tags caption; rebuild the captions so
    // they match the mode that was just selected.
    for (int i = 0; i < iconList_->count(); ++i) {
        updateIconItemText(iconList_->item(i));
    }
    // Relayout from the top so icons never start half-clipped after a mode
    // switch (Qt IconMode keeps the old scroll offset and item layout).
    iconList_->scrollToTop();
    iconList_->doItemsLayout();
    layoutLibraryChrome();
    QSettings settings;
    settings.setValue(QStringLiteral("gallery/viewMode"), index);
}

void MainWindow::updateSortButton() {
    if (sortButton_ == nullptr) {
        return;
    }
    const QStringList labels{
        QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Duration"),
        QStringLiteral("Resolution"), QStringLiteral("Codec"), QStringLiteral("Tags"),
        QStringLiteral("Imported")};
    const int key = std::clamp(sortKey_, 0, labels.size() - 1);
    sortButton_->setText(QStringLiteral("%1  %2")
        .arg(sortAscending_ ? QStringLiteral("↑") : QStringLiteral("↓"), labels.at(key)));
}

void MainWindow::applyGallerySort(const int key) {
    if (sortKey_ == key) {
        sortAscending_ = !sortAscending_;
    } else {
        sortKey_ = key;
        sortAscending_ = true;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("gallery/sortKey"), sortKey_);
    settings.setValue(QStringLiteral("gallery/sortAscending"), sortAscending_);
    updateSortButton();
    sortGalleryItems();
}

void MainWindow::sortGalleryItems() {
    if (iconList_ == nullptr) {
        return;
    }
    QList<qlonglong> selected;
    for (auto* item : iconList_->selectedItems()) {
        selected.append(item->data(Qt::UserRole).toLongLong());
    }
    const qlonglong currentId = iconList_->currentItem() == nullptr
        ? -1
        : iconList_->currentItem()->data(Qt::UserRole).toLongLong();
    QList<QListWidgetItem*> items;
    while (iconList_->count() > 0) {
        items.append(iconList_->takeItem(0));
    }
    const int key = sortKey_;
    const bool ascending = sortAscending_;
    std::stable_sort(items.begin(), items.end(), [key, ascending](QListWidgetItem* left, QListWidgetItem* right) {
        auto textCompare = [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive);
        };
        auto numberCompare = [](const qlonglong a, const qlonglong b) {
            return a < b ? -1 : (a > b ? 1 : 0);
        };
        int compared = 0;
        switch (key) {
        case 1:
            compared = numberCompare(
                left->data(Qt::UserRole + 6).toLongLong(),
                right->data(Qt::UserRole + 6).toLongLong());
            break;
        case 2:
            compared = numberCompare(
                left->data(Qt::UserRole + 7).toLongLong(),
                right->data(Qt::UserRole + 7).toLongLong());
            break;
        case 3:
            compared = numberCompare(
                left->data(Qt::UserRole + 8).toLongLong(),
                right->data(Qt::UserRole + 8).toLongLong());
            break;
        case 4:
            compared = textCompare(
                left->data(Qt::UserRole + 11).toString(),
                right->data(Qt::UserRole + 11).toString());
            break;
        case 5:
            compared = textCompare(
                left->data(Qt::UserRole + 2).toString(),
                right->data(Qt::UserRole + 2).toString());
            break;
        case 6:
            compared = numberCompare(
                left->data(Qt::UserRole + 9).toLongLong(),
                right->data(Qt::UserRole + 9).toLongLong());
            break;
        default:
            compared = textCompare(
                left->data(Qt::UserRole + 1).toString(),
                right->data(Qt::UserRole + 1).toString());
            break;
        }
        if (compared == 0) {
            compared = numberCompare(
                left->data(Qt::UserRole).toLongLong(),
                right->data(Qt::UserRole).toLongLong());
        }
        if (!ascending) {
            compared = -compared;
        }
        return compared < 0;
    });
    QListWidgetItem* currentItem = nullptr;
    for (auto* item : items) {
        iconList_->addItem(item);
        const qlonglong id = item->data(Qt::UserRole).toLongLong();
        if (selected.contains(id)) {
            item->setSelected(true);
        }
        if (id == currentId) {
            currentItem = item;
        }
    }
    if (currentItem != nullptr) {
        iconList_->setCurrentItem(currentItem);
    }
}

void MainWindow::refreshTagFilter() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto generation = ++tagGeneration_;
    const auto vault = vault_;
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<core::Result<std::vector<core::TagInfo>>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, generation, vault, watcher] {
        const auto outcome = *watcher->result();
        watcher->deleteLater();
        if (generation != tagGeneration_ || vault_ != vault) {
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

void MainWindow::beginEditTags(const std::vector<std::int64_t>& video_ids) {
    if (!vault_ || !vault_->is_unlocked() || video_ids.empty()) {
        return;
    }
    const auto vault = vault_;
    if (tagEditorWatcher_ != nullptr) {
        tagEditorWatcher_->waitForFinished();
    }
    tagEditorWatcher_ = new QFutureWatcher<std::shared_ptr<TagEditorData>>(this);
    connect(tagEditorWatcher_, &QFutureWatcherBase::finished, this,
        [this] { finishEditTags(); });
    tagEditorWatcher_->setFuture(QtConcurrent::run([vault, video_ids] {
        auto data = std::make_shared<TagEditorData>();
        data->video_ids = video_ids;
        auto all = vault->list_tags();
        if (all) {
            data->all_tags = all.value();
        }
        // A tag starts checked only when EVERY selected video carries it, so
        // the dialog applies one exact tag set to all of them: checking adds
        // the tag to every video, unchecking removes it from every video.
        // Tags held by only some videos are left alone unless the user
        // explicitly checks or unchecks them.
        QSet<QString> shared;
        std::vector<core::TagInfo> first_video_tags;
        bool first = true;
        for (const auto id : video_ids) {
            auto own = vault->tags_for_video(id);
            if (!own) {
                continue;
            }
            QSet<QString> names;
            for (const auto& tag : own.value()) {
                names.insert(QString::fromStdString(tag.name));
            }
            if (first) {
                shared = names;
                first_video_tags = own.value();
                first = false;
            } else {
                shared.intersect(names);
            }
        }
        for (const auto& tag : first_video_tags) {
            if (shared.contains(QString::fromStdString(tag.name))) {
                data->video_tags.push_back(tag);
            }
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

    if (data->video_ids.size() > 1U) {
        auto* hint = new QLabel(
            QStringLiteral("Applying to %1 selected videos — checked tags are set on all of them.")
                .arg(data->video_ids.size()), &dialog);
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }

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
    const auto video_ids = data->video_ids;
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    adminWatcher_ = new QFutureWatcher<std::shared_ptr<core::Result<bool>>>(this);
    connect(adminWatcher_, &QFutureWatcherBase::finished, this, [this] {
        auto* completed = adminWatcher_;
        adminWatcher_ = nullptr;
        const auto outcome = *completed->result();
        completed->deleteLater();
        importButton_->setEnabled(true);
        importFolderButton_->setEnabled(true);
        settingsButton_->setEnabled(true);
        if (!outcome) {
            setError(galleryStatus_,
                QString::fromUtf8(core::user_message(outcome.error().code).data()));
            galleryStatus_->setVisible(true);
            return;
        }
        refreshTagFilter();
        refreshGallery();
    });
    adminWatcher_->setFuture(QtConcurrent::run([vault, video_ids, applied, desired] {
        for (const auto& name : desired) {
            if (!applied.contains(name)) {
                for (const auto id : video_ids) {
                    (void)vault->add_tag(id, name.toUtf8().constData());
                }
            }
        }
        auto tags = vault->list_tags();
        if (tags) {
            for (const auto& tag : tags.value()) {
                const QString name = QString::fromStdString(tag.name);
                if (applied.contains(name) && !desired.contains(name)) {
                    for (const auto id : video_ids) {
                        (void)vault->remove_tag(id, tag.id);
                    }
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
    QAction* restore = menu.addAction(QStringLiteral("Restore to folder…"));
    QAction* regenerate = menu.addAction(QStringLiteral("Regenerate thumbnail"));
    QAction* remove = menu.addAction(QStringLiteral("Remove"));
    QAction* chosen = menu.exec(global_position);
    if (chosen == play) {
        beginPlayback(video_id);
    } else if (chosen == editTags) {
        // Apply to the whole selection when multiple items are selected;
        // fall back to the clicked video when nothing is selected.
        auto ids = selectedVideoIds();
        if (ids.empty()) {
            ids.push_back(video_id);
        }
        beginEditTags(ids);
    } else if (chosen == restore) {
        // Restore the clicked video; if the selection holds multiple items,
        // restore all of them.
        const auto selected = selectedVideoIds();
        if (selected.empty()) {
            QItemSelectionModel* model = galleryStack_->currentWidget() == detailsTree_
                ? detailsTree_->selectionModel()
                : iconList_->selectionModel();
            if (model != nullptr) {
                model->clearSelection();
            }
        }
        beginRestoreSelected();
    } else if (chosen == regenerate) {
        // Operate on the whole selection (multi-select supported).
        beginGenerateThumbnail();
    } else if (chosen == remove) {
        beginRemoveSelected();
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
    const QStringList selected = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Import media"), {},
        QStringLiteral("Media files (*.mp4 *.mkv *.avi *.mov *.wmv *.webm *.m4v *.ts *.flv *.3gp *.mpg *.mpeg"
                       " *.jpg *.jpeg *.png *.webp *.bmp *.gif);;All files (*)"));
    if (selected.isEmpty()) {
        return;
    }
    std::vector<std::filesystem::path> sources;
    sources.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& entry : selected) {
        sources.push_back(pathFromText(entry));
    }
    beginImportMany(std::move(sources));
}

void MainWindow::beginImportFolder() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    setError(galleryStatus_, {});
    const QString selected = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Import every video from a folder"), {},
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return;
    }
    QDir directory(selected);
    const QStringList names = directory.entryList(
        {"*.mp4", "*.mkv", "*.avi", "*.mov", "*.wmv", "*.webm", "*.m4v",
         "*.ts", "*.flv", "*.3gp", "*.mpg", "*.mpeg",
         "*.jpg", "*.jpeg", "*.png", "*.webp", "*.bmp", "*.gif",
         "*.MP4", "*.MKV", "*.AVI", "*.MOV", "*.WMV", "*.WEBM", "*.M4V",
         "*.TS", "*.FLV", "*.3GP", "*.MPG", "*.MPEG",
         "*.JPG", "*.JPEG", "*.PNG", "*.WEBP", "*.BMP", "*.GIF"},
        QDir::Files | QDir::Readable, QDir::Name);
    if (names.isEmpty()) {
        setError(galleryStatus_, QStringLiteral("No media files were found in that folder."));
        galleryStatus_->setVisible(true);
        return;
    }
    std::vector<std::filesystem::path> sources;
    sources.reserve(static_cast<std::size_t>(names.size()));
    for (const auto& name : names) {
        sources.push_back(pathFromText(directory.filePath(name)));
    }
    beginImportMany(std::move(sources));
}

void MainWindow::beginImportMany(std::vector<std::filesystem::path> sources) {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Importing");
    setError(galleryStatus_, {});
    const auto vault = vault_;
    const auto shared_sources =
        std::make_shared<std::vector<std::filesystem::path>>(std::move(sources));
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    galleryStatus_->setText(QStringLiteral("Importing…"));
    galleryStatus_->setVisible(true);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    // Run on the batch worker thread; progress/completion arrive as queued
    // signals so the UI stays fully responsive during the import.
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, sources = std::move(shared_sources)] {
            worker->importFiles(sources, vault);
        },
        Qt::QueuedConnection);
}

void MainWindow::finishBatch(const bool ok, const QString& message, const int count) {
    Q_UNUSED(count)
    batchBusy_ = false;
    importButton_->setEnabled(true);
    importFolderButton_->setEnabled(true);
    settingsButton_->setEnabled(true);
    progressBar_->hide();
    if (!ok) {
        setError(galleryStatus_, message);
        return;
    }
    galleryStatus_->setText(message);
    galleryStatus_->setVisible(true);
    refreshTagFilter();
    refreshGallery();
}

void MainWindow::openSettings() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    setError(galleryStatus_, {});
    auto* dialog = new SettingsDialog(vault_, this);
    connect(dialog, &SettingsDialog::settingsChanged, this, [this] {
        refreshTagFilter();
        refreshGallery();
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::beginRestoreSelected() {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Restoring");
    setError(galleryStatus_, {});
    const auto ids = selectedVideoIds();
    if (ids.empty()) {
        batchBusy_ = false;
        setError(galleryStatus_, QStringLiteral("Select one or more videos to restore."));
        galleryStatus_->setVisible(true);
        return;
    }
    const QString target = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Restore selected videos to folder"), {},
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (target.isEmpty()) {
        batchBusy_ = false;
        return;
    }
    const auto vault = vault_;
    const auto shared_ids = std::make_shared<std::vector<std::int64_t>>(ids);
    const auto shared_dir = std::make_shared<std::filesystem::path>(pathFromText(target));
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    galleryStatus_->setText(QStringLiteral("Restoring…"));
    galleryStatus_->setVisible(true);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    // Run on the batch worker thread; progress/completion arrive as queued
    // signals so the UI stays fully responsive during the restore.
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, ids = std::move(shared_ids), dir = std::move(shared_dir)] {
            worker->restoreVideos(ids, dir, vault);
        },
        Qt::QueuedConnection);
}

std::vector<std::int64_t> MainWindow::selectedVideoIds() const {
    std::vector<std::int64_t> ids;
    QList<QTreeWidgetItem*> tree_selection;
    QList<QListWidgetItem*> list_selection;
    if (galleryStack_->currentWidget() == detailsTree_) {
        tree_selection = detailsTree_->selectedItems();
    } else {
        list_selection = iconList_->selectedItems();
    }
    ids.reserve(static_cast<std::size_t>(
        tree_selection.size() + list_selection.size()));
    for (auto* item : tree_selection) {
        ids.push_back(item->data(0, Qt::UserRole).toLongLong());
    }
    for (auto* item : list_selection) {
        ids.push_back(item->data(Qt::UserRole).toLongLong());
    }
    return ids;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (vault_ && vault_->is_unlocked() && event->mimeData()->hasUrls()) {
        const auto urls = event->mimeData()->urls();
        for (const auto& url : urls) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event) {
    std::vector<std::filesystem::path> sources;
    const auto urls = event->mimeData()->urls();
    for (const auto& url : urls) {
        if (url.isLocalFile()) {
            sources.push_back(pathFromText(url.toLocalFile()));
        }
    }
    if (!sources.empty()) {
        event->acceptProposedAction();
        beginImportMany(std::move(sources));
        return;
    }
    QMainWindow::dropEvent(event);
}

void MainWindow::refreshGallery() {
    detailsTree_->clear();
    iconList_->clear();
    setError(galleryStatus_, {});
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    const auto generation = ++galleryGeneration_;
    const auto vault = vault_;
    auto* watcher = new QFutureWatcher<std::vector<core::VideoInfo>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, generation, vault, watcher] {
        if (generation != galleryGeneration_ || vault_ != vault
            || !vault_->is_unlocked()) {
            // A newer refresh superseded this snapshot (e.g. a delete landed
            // mid-list); publishing it would resurrect removed videos.
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
            auto* iconItem = new QListWidgetItem(iconList_);
            iconItem->setData(Qt::UserRole, static_cast<qlonglong>(video_id));
            iconItem->setData(Qt::UserRole + 1, name);
            iconItem->setData(Qt::UserRole + 2, tags_text);
            iconItem->setData(Qt::UserRole + 5, size_text);
            iconItem->setData(Qt::UserRole + 6, static_cast<qlonglong>(video.original_size));
            iconItem->setData(Qt::UserRole + 9, static_cast<qlonglong>(video.imported_at));
            iconItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
            iconItem->setToolTip(QStringLiteral("Name:%1\nSize: %2\nTags:%3\nCreated:%4").
            arg(name).
            arg(size_text).
            arg(tags_text).
            arg(QDateTime::fromSecsSinceEpoch(imported_text.toInt()).toLocalTime().toString("yyyy-MM-dd HH:mm:ss")));
            updateIconItemText(iconItem);

            // Details view item.
            QTreeWidgetItem* treeItem = new QTreeWidgetItem(detailsTree_);
            treeItem->setText(0, name);
            treeItem->setText(1, size_text);
            treeItem->setText(2, QStringLiteral("…"));
            treeItem->setText(5, tags_text);
            treeItem->setText(6, QDateTime::fromSecsSinceEpoch(imported_text.toInt()).toLocalTime().toString("yyyy-MM-dd HH:mm:ss"));
            treeItem->setData(0, Qt::UserRole, static_cast<qlonglong>(video_id));

            treeItem->setToolTip(0, QStringLiteral("Name:%1\nSize: %2\nTags:%3\nCreated:%4").
            arg(name).
            arg(size_text).
            arg(tags_text).
            arg(QDateTime::fromSecsSinceEpoch(imported_text.toInt()).toLocalTime().toString("yyyy-MM-dd HH:mm:ss")));
            // Attach a stored thumbnail asynchronously (both views).
            auto* thumbWatcher =
                new QFutureWatcher<std::shared_ptr<core::Result<core::ThumbnailInfo>>>(this);
            connect(thumbWatcher, &QFutureWatcherBase::finished, this,
                [this, video_id, thumbWatcher] {
                    const auto outcome = *thumbWatcher->result();
                    thumbWatcher->deleteLater();
                    if (!outcome || outcome.value().bytes.empty()) {
                        return;
                    }
                    auto* iconItem = findListItemById(iconList_, video_id);
                    auto* treeItem = findTreeItemById(detailsTree_, video_id);
                    if (iconItem == nullptr && treeItem == nullptr) {
                        // Item was cleared out by a newer refresh before the
                        // thumbnail landed; nothing to attach it to.
                        return;
                    }
                    QPixmap pixmap;
                    if (pixmap.loadFromData(outcome.value().bytes.data(),
                            static_cast<int>(outcome.value().bytes.size()))) {
                        if (iconItem != nullptr) {
                            iconItem->setData(Qt::UserRole + 4, pixmap.scaled(
                                720, 720, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                            iconItem->setIcon(QIcon(pixmap.scaled(
                                32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                        }
                        if (treeItem != nullptr) {
                            treeItem->setIcon(0, QIcon(pixmap.scaled(
                                32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                        }
                    }
                });
            thumbWatcher->setFuture(QtConcurrent::run([vault, video_id] {
                auto stored = vault->thumbnail(video_id);
                // Import stores a thumbnail only when generation succeeded.
                // Clips whose keyframe index hid the only IDR were saved with
                // none; build one now so the gallery fills in without a
                // manual "Regenerate thumbnail".
                if (!stored || stored.value().bytes.empty()) {
                    stored = vault->generate_thumbnail(video_id, 320U);
                }
                return std::make_shared<core::Result<core::ThumbnailInfo>>(
                    std::move(stored));
            }));

            // Media metadata for the details columns, asynchronously.
            auto* mediaWatcher =
                new QFutureWatcher<std::shared_ptr<core::Result<core::MediaInfo>>>(this);
            connect(mediaWatcher, &QFutureWatcherBase::finished, this,
                [this, video_id, mediaWatcher] {
                    const auto outcome = *mediaWatcher->result();
                    mediaWatcher->deleteLater();
                    if (!outcome) {
                        return;
                    }
                    const core::MediaInfo& info = outcome.value();
                    const std::int64_t duration = info.duration_ms;
                    const QString durationText = QStringLiteral("%1:%2")
                        .arg(duration / 60000)
                        .arg((duration / 1000) % 60, 2, 10, QLatin1Char('0'));
                    if (auto* iconItem = findListItemById(iconList_, video_id)) {
                        if (duration > 500) {
                            iconItem->setData(Qt::UserRole + 3, durationText);
                        }
                        iconItem->setData(Qt::UserRole + 7, static_cast<qlonglong>(duration));
                        iconItem->setData(
                            Qt::UserRole + 8,
                            static_cast<qlonglong>(info.width) * static_cast<qlonglong>(info.height));
                        iconItem->setData(Qt::UserRole + 11, QString::fromStdString(info.codec_name));
                        if (sortKey_ == 2 || sortKey_ == 3 || sortKey_ == 4) {
                            sortGalleryItems();
                        }
                    }
                    auto* treeItem = findTreeItemById(detailsTree_, video_id);
                    if (treeItem == nullptr) {
                        // Row was cleared out by a newer refresh (e.g. a search
                        // keystroke) before the metadata landed.
                        return;
                    }
                    treeItem->setText(2, durationText);
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
        sortGalleryItems();
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

void MainWindow::embedPlayer() {
    if (playerWindow_ == nullptr || contentStack_ == nullptr) {
        return;
    }
    playerWindow_->prepareDock();
    if (contentStack_->indexOf(playerWindow_) < 0) {
        contentStack_->addWidget(playerWindow_);
    }
    contentStack_->setCurrentWidget(playerWindow_);
    playerWindow_->show();
    playerWindow_->setFocus();
}

void MainWindow::popPlayer() {
    if (playerWindow_ == nullptr || contentStack_ == nullptr) {
        return;
    }
    contentStack_->removeWidget(playerWindow_);
    contentStack_->setCurrentWidget(pages_);
    playerWindow_->detach();
}

void MainWindow::restoreGalleryFocus() {
    if (galleryStack_ == nullptr || contentStack_ == nullptr || pages_ == nullptr) {
        return;
    }
    if (contentStack_->currentWidget() != pages_) {
        return;
    }
    const bool details = galleryStack_->currentWidget() == detailsTree_;
    QAbstractItemView* view = details
        ? static_cast<QAbstractItemView*>(detailsTree_)
        : static_cast<QAbstractItemView*>(iconList_);
    if (view == nullptr) {
        return;
    }
    if (!view->currentIndex().isValid() && view->model() != nullptr
        && view->model()->rowCount() > 0) {
        view->setCurrentIndex(view->model()->index(0, 0));
    }
    view->setFocus(Qt::OtherFocusReason);
}

void MainWindow::beginPlayback(const std::int64_t video_id) {
    if (!vault_ || !vault_->is_unlocked() || video_id < 0) {
        return;
    }
    QString title = QStringLiteral("Video");
    int index = 0;
    int count = 0;
    bool hasPrevious = false;
    bool hasNext = false;
    const bool details = galleryStack_ != nullptr && galleryStack_->currentWidget() == detailsTree_;
    const int items = details
        ? (detailsTree_ != nullptr ? detailsTree_->topLevelItemCount() : 0)
        : (iconList_ != nullptr ? iconList_->count() : 0);
    int found = -1;
    for (int i = 0; i < items; ++i) {
        const qlonglong id = details
            ? detailsTree_->topLevelItem(i)->data(0, Qt::UserRole).toLongLong()
            : iconList_->item(i)->data(Qt::UserRole).toLongLong();
        if (id == video_id) {
            found = i;
            title = details
                ? detailsTree_->topLevelItem(i)->text(0)
                : iconList_->item(i)->data(Qt::UserRole + 1).toString();
            break;
        }
    }
    count = items;
    if (found >= 0) {
        index = found + 1;
        hasPrevious = found > 0;
        hasNext = found + 1 < items;
    }
    if (title.isEmpty()) {
        title = QStringLiteral("Video");
    }

    auto applyClip = [&](PlayerWindow* window) {
        window->presentClip(title, index, count, hasPrevious, hasNext);
    };
    if (playerWindow_ != nullptr) {
        playerWindow_->playVideo(vault_, video_id);
        applyClip(playerWindow_);
        if (playerWindow_->isDetached()) {
            playerWindow_->raise();
            playerWindow_->activateWindow();
        } else {
            embedPlayer();
        }
        return;
    }
    auto* window = new PlayerWindow(vault_, video_id, title, contentStack_);
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &QDialog::destroyed, this,
        [this, window] {
            if (playerWindow_ == window) {
                playerWindow_ = nullptr;
            }
            if (contentStack_ != nullptr && pages_ != nullptr) {
                contentStack_->setCurrentWidget(pages_);
            }
            // The gallery was hidden while the player had the stack, so Qt
            // cannot hand focus back to it. Put it back on the selected item.
            QTimer::singleShot(0, this, [this] { restoreGalleryFocus(); });
        });
    connect(window, &PlayerWindow::detachRequested, this, [this] { popPlayer(); });
    connect(window, &PlayerWindow::dockRequested, this, [this] { embedPlayer(); });
    connect(window, &PlayerWindow::previousRequested, this, [this] { stepPlayback(-1); });
    connect(window, &PlayerWindow::nextRequested, this, [this] { stepPlayback(1); });
    connect(window, &PlayerWindow::playbackFinished, this, [this] { stepPlayback(1); });
    playerWindow_ = window;
    applyClip(window);
    embedPlayer();
}

void MainWindow::stepPlayback(const int delta) {
    if (playerWindow_ == nullptr || delta == 0) {
        return;
    }
    const std::int64_t current = playerWindow_->videoId();
    const bool details = galleryStack_ != nullptr && galleryStack_->currentWidget() == detailsTree_;
    const int items = details
        ? (detailsTree_ != nullptr ? detailsTree_->topLevelItemCount() : 0)
        : (iconList_ != nullptr ? iconList_->count() : 0);
    int found = -1;
    for (int i = 0; i < items; ++i) {
        const qlonglong id = details
            ? detailsTree_->topLevelItem(i)->data(0, Qt::UserRole).toLongLong()
            : iconList_->item(i)->data(Qt::UserRole).toLongLong();
        if (id == current) {
            found = i;
            break;
        }
    }
    const int target = found + delta;
    if (found < 0 || target < 0 || target >= items) {
        return;
    }
    const qlonglong nextId = details
        ? detailsTree_->topLevelItem(target)->data(0, Qt::UserRole).toLongLong()
        : iconList_->item(target)->data(Qt::UserRole).toLongLong();
    beginPlayback(nextId);
}

void MainWindow::beginRemoveSelected() {
    if (!vault_ || !vault_->is_unlocked() || batchBusy_) {
        return;
    }
    setError(galleryStatus_, {});
    const auto ids = selectedVideoIds();
    if (ids.empty()) {
        setError(galleryStatus_, QStringLiteral("Select one or more videos to remove."));
        galleryStatus_->setVisible(true);
        return;
    }
    const auto confirm = QMessageBox::question(
        this, QStringLiteral("Remove videos"),
        QStringLiteral("Remove %1 video(s) from the vault? This cannot be undone.")
            .arg(ids.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirm != QMessageBox::Yes) {
        return;
    }
    batchBusy_ = true;
    batchLabel_ = QStringLiteral("Removing");
    const auto vault = vault_;
    const auto shared_ids = std::make_shared<std::vector<std::int64_t>>(ids);
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    galleryStatus_->setText(QStringLiteral("Removing…"));
    galleryStatus_->setVisible(true);
    progressBar_->setRange(0, 1000);
    progressBar_->setValue(0);
    progressBar_->show();
    // Run on the batch worker thread; progress/completion arrive as queued
    // signals so the UI stays fully responsive during the removal.
    QMetaObject::invokeMethod(batchWorker_,
        [worker = batchWorker_, vault, ids = std::move(shared_ids)] {
            worker->removeVideos(ids, vault);
        },
        Qt::QueuedConnection);
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
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
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
    importFolderButton_->setEnabled(true);
    settingsButton_->setEnabled(true);
    if (!outcome) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    setError(galleryStatus_, QStringLiteral("Password changed."));
    galleryStatus_->setVisible(true);
}

void MainWindow::beginGenerateThumbnail(const std::vector<std::int64_t>& ids) {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    auto targets = ids;
    if (targets.empty()) {
        targets = selectedVideoIds();
    }
    if (targets.empty()) {
        setError(galleryStatus_, QStringLiteral("Select one or more videos to thumbnail."));
        galleryStatus_->setVisible(true);
        return;
    }
    const auto vault = vault_;
    const auto shared_ids = std::make_shared<std::vector<std::int64_t>>(std::move(targets));
    importButton_->setEnabled(false);
    importFolderButton_->setEnabled(false);
    settingsButton_->setEnabled(false);
    galleryStatus_->setText(QStringLiteral("Generating thumbnails…"));
    galleryStatus_->setVisible(true);
    thumbnailWatcher_ =
        new QFutureWatcher<std::shared_ptr<core::Result<int>>>(this);
    connect(thumbnailWatcher_, &QFutureWatcherBase::finished, this,
        [this] { finishGenerateThumbnail(); });
    thumbnailWatcher_->setFuture(QtConcurrent::run([vault, shared_ids] {
        int regenerated = 0;
        for (const auto video_id : *shared_ids) {
            auto outcome = vault->generate_thumbnail(video_id, 320U);
            if (outcome) {
                ++regenerated;
            }
        }
        return std::make_shared<core::Result<int>>(regenerated);
    }));
}

void MainWindow::finishGenerateThumbnail() {
    auto* completed = thumbnailWatcher_;
    thumbnailWatcher_ = nullptr;
    const auto outcome = *completed->result();
    completed->deleteLater();
    importButton_->setEnabled(true);
    importFolderButton_->setEnabled(true);
    settingsButton_->setEnabled(true);
    if (!outcome) {
        const QString message =
            QString::fromUtf8(core::user_message(outcome.error().code).data());
        setError(galleryStatus_, message);
        return;
    }
    galleryStatus_->setText(
        QStringLiteral("Regenerated %1 thumbnail(s).").arg(outcome.value()));
    galleryStatus_->setVisible(true);
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

void MainWindow::openShare() {
    if (!vault_ || !vault_->is_unlocked()) {
        return;
    }
    if (!shareServer_) {
        shareServer_ = std::make_unique<ShareServer>();
        shareServer_->setOnRunning([this](bool sharing) {
            if (shareButton_ != nullptr) {
                shareButton_->setText(sharing ? QStringLiteral("Sharing…") : QStringLiteral("Share"));
            }
        });
    }
    if (!shareServer_->running()) {
        QString error;
        if (!shareServer_->start(vault_->root_path(), &error)) {
            QMessageBox::warning(this, QStringLiteral("Share with phone"), error);
            return;
        }
    }
    shareServer_->showDialog(this);
}

void MainWindow::lockVault() {
    autoLockTimer_->stop();
    // Sharing stays up. The phone keeps its own unlocked vault until Stop sharing.
    std::filesystem::path root;
    if (vault_) {
        root = vault_->root_path();
        vault_->lock();
        vault_.reset();
    }
    importButton_->setEnabled(true);
    importFolderButton_->setEnabled(true);
    settingsButton_->setEnabled(true);
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
