#include "videovault/app/player_window.hpp"

extern "C" {
#include <libavutil/error.h>
}

#include <QApplication>
#include <QAudioFormat>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace videovault::app {

namespace {

enum class PlayerIcon { Play, Pause, Volume, Muted, Fullscreen };

// Crisp vector-style icons drawn with QPainter (no external assets).
QPixmap makePlayerIcon(const PlayerIcon kind, const int size, const QColor& color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const double s = static_cast<double>(size);
    switch (kind) {
    case PlayerIcon::Play: {
        QPolygonF triangle;
        triangle << QPointF(s * 0.28, s * 0.18) << QPointF(s * 0.82, s * 0.5)
                 << QPointF(s * 0.28, s * 0.82);
        painter.drawPolygon(triangle);
        break;
    }
    case PlayerIcon::Pause:
        painter.drawRoundedRect(
            QRectF(s * 0.26, s * 0.2, s * 0.16, s * 0.6), s * 0.04, s * 0.04);
        painter.drawRoundedRect(
            QRectF(s * 0.58, s * 0.2, s * 0.16, s * 0.6), s * 0.04, s * 0.04);
        break;
    case PlayerIcon::Volume: {
        QPolygonF speaker;
        speaker << QPointF(s * 0.14, s * 0.38) << QPointF(s * 0.36, s * 0.38)
                << QPointF(s * 0.56, s * 0.2) << QPointF(s * 0.56, s * 0.8)
                << QPointF(s * 0.36, s * 0.62) << QPointF(s * 0.14, s * 0.62);
        painter.drawPolygon(speaker);
        QPen wave(color, std::max(1.2, s * 0.07));
        wave.setCapStyle(Qt::RoundCap);
        painter.setPen(wave);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(s * 0.56, s * 0.28, s * 0.24, s * 0.44), -55 * 16, 110 * 16);
        painter.drawArc(QRectF(s * 0.66, s * 0.16, s * 0.28, s * 0.68), -55 * 16, 110 * 16);
        break;
    }
    case PlayerIcon::Muted: {
        QPolygonF speaker;
        speaker << QPointF(s * 0.14, s * 0.38) << QPointF(s * 0.36, s * 0.38)
                << QPointF(s * 0.56, s * 0.2) << QPointF(s * 0.56, s * 0.8)
                << QPointF(s * 0.36, s * 0.62) << QPointF(s * 0.14, s * 0.62);
        painter.drawPolygon(speaker);
        QPen cross(color, std::max(1.4, s * 0.08));
        cross.setCapStyle(Qt::RoundCap);
        painter.setPen(cross);
        painter.setBrush(Qt::NoBrush);
        const double cx = s * 0.68;
        const double cy = s * 0.5;
        const double r = s * 0.17;
        painter.drawLine(QPointF(cx - r, cy - r), QPointF(cx + r, cy + r));
        painter.drawLine(QPointF(cx - r, cy + r), QPointF(cx + r, cy - r));
        break;
    }
    case PlayerIcon::Fullscreen: {
        QPen bracket(color, std::max(1.4, s * 0.09));
        bracket.setCapStyle(Qt::RoundCap);
        painter.setPen(bracket);
        painter.setBrush(Qt::NoBrush);
        const double m = s * 0.22;
        const double l = s * 0.2;
        painter.drawLine(QPointF(m, m - l), QPointF(m, m));
        painter.drawLine(QPointF(m - l, m), QPointF(m, m));
        painter.drawLine(QPointF(s - m, m - l), QPointF(s - m, m));
        painter.drawLine(QPointF(s - m + l, m), QPointF(s - m, m));
        painter.drawLine(QPointF(m, s - m + l), QPointF(m, s - m));
        painter.drawLine(QPointF(m - l, s - m), QPointF(m, s - m));
        painter.drawLine(QPointF(s - m, s - m + l), QPointF(s - m, s - m));
        painter.drawLine(QPointF(s - m + l, s - m), QPointF(s - m, s - m));
        break;
    }
    }
    return pixmap;
}

const char* kIconButtonStyle =
    "QPushButton { background: transparent; border: none; border-radius: 9px; }"
    "QPushButton:hover { background: rgba(255,255,255,26); }"
    "QPushButton:pressed { background: rgba(255,255,255,42); }";

// Still-image codecs (FFmpeg demuxes jpg/png/... as single-frame "videos").
bool is_image_codec(const std::string& codec_name) {
    static const std::vector<std::string> kImageCodecs{
        "png", "mjpeg", "bmp", "webp", "gif", "tiff"};
    return std::find(kImageCodecs.begin(), kImageCodecs.end(), codec_name)
        != kImageCodecs.end();
}

} // namespace

AudioSink::AudioSink(QObject* parent) : QIODevice(parent) {
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

AudioSink::~AudioSink() = default;

void AudioSink::append(const std::int16_t* samples, const std::size_t count) {
    std::lock_guard<std::mutex> guard(mutex_);
    buffer_.insert(buffer_.end(), samples, samples + count);
}

void AudioSink::clear() {
    std::lock_guard<std::mutex> guard(mutex_);
    buffer_.clear();
    read_cursor_ = 0;
}

std::size_t AudioSink::buffered_samples() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return buffer_.size() - read_cursor_;
}

qint64 AudioSink::readData(char* data, qint64 maxSize) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto byte_count = static_cast<std::size_t>(std::max<qint64>(maxSize, 0));
    const auto available = (buffer_.size() - read_cursor_) * sizeof(std::int16_t);
    const auto take = std::min(byte_count, available);
    if (take > 0) {
        std::memcpy(data, buffer_.data() + read_cursor_, take);
        read_cursor_ += take / sizeof(std::int16_t);
    }
    return static_cast<qint64>(take);
}

qint64 AudioSink::writeData(const char*, qint64) {
    return -1; // read-only sink
}

PlayerWindow::PlayerWindow(
    const std::shared_ptr<videovault::core::Vault>& vault,
    const std::int64_t video_id,
    const QString& title,
    QWidget* parent)
    : QDialog(parent), vault_(vault), video_id_(video_id), title_(title) {
    setWindowTitle(QStringLiteral("Play — %1").arg(title));
    resize(960, 620);
    setMinimumSize(480, 320);
    setMouseTracking(true);

    QSettings settings;
    volumePercent_ = settings.value(QStringLiteral("player/volume"), 100).toInt();
    volumePercent_ = std::clamp(volumePercent_, 0, 100);
    muted_ = settings.value(QStringLiteral("player/muted"), false).toBool();
    const int cache_mib =
        settings.value(QStringLiteral("player/streamCacheMib"), 32).toInt();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    surface_ = new VideoSurface(this);
    surface_->setMinimumSize(320, 200);
    surface_->setText(QStringLiteral("Loading…"));
    // App-level key interception: the player's shortcuts (Space, arrows, +-,
    // F) work regardless of which child widget holds focus.
    qApp->installEventFilter(this);
    layout->addWidget(surface_, 1);

    // Overlay chrome: title bar on top, center play button, bottom controls —
    // all direct children of the surface, so the empty video area keeps its
    // own mouse handling (click-to-seek) while the controls work normally.
    topOverlay_ = new QWidget(surface_);
    topOverlay_->setAttribute(Qt::WA_StyledBackground, true);
    topOverlay_->setStyleSheet(QStringLiteral(
        "background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        " stop:0 rgba(8,10,14,200), stop:1 rgba(8,10,14,0));"));
    topOverlay_->setFixedHeight(64);
    titleLabel_ = new QLabel(title, topOverlay_);
    titleLabel_->setStyleSheet(QStringLiteral(
        "color: rgba(255,255,255,225); font-size: 14px; font-weight: 600;"));
    auto* topLayout = new QVBoxLayout(topOverlay_);
    topLayout->setContentsMargins(16, 10, 16, 6);
    topLayout->addWidget(titleLabel_);

    centerPlayButton_ = new QPushButton(surface_);
    centerPlayButton_->setFixedSize(72, 72);
    centerPlayButton_->setCursor(Qt::PointingHandCursor);
    centerPlayButton_->setIconSize(QSize(36, 36));
    centerPlayButton_->setIcon(makePlayerIcon(PlayerIcon::Play, 36, Qt::white));
    centerPlayButton_->setFocusPolicy(Qt::NoFocus);
    centerPlayButton_->setStyleSheet(QStringLiteral(
        "QPushButton { border-radius: 36px; background: rgba(0,0,0,150);"
        " border: 1px solid rgba(255,255,255,70); }"
        "QPushButton:hover { background: rgba(91,124,250,190); }"));
    connect(centerPlayButton_, &QPushButton::clicked, this, [this] {
        togglePlayPause();
    });

    controlsOverlay_ = new QWidget(surface_);
    controlsOverlay_->setAttribute(Qt::WA_StyledBackground, true);
    controlsOverlay_->setStyleSheet(QStringLiteral(
        "background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        " stop:0 rgba(8,10,14,0), stop:1 rgba(8,10,14,205));"));
    auto* controlsLayout = new QVBoxLayout(controlsOverlay_);
    controlsLayout->setContentsMargins(14, 8, 14, 10);
    controlsLayout->setSpacing(4);

    positionSlider_ = new SeekSlider(controlsOverlay_);
    positionSlider_->setRange(0, 1);
    positionSlider_->setTracking(false);
    positionSlider_->setFixedHeight(30);
    controlsLayout->addWidget(positionSlider_);

    auto* row = new QHBoxLayout();
    row->setSpacing(6);
    playButton_ = new QPushButton(controlsOverlay_);
    playButton_->setFixedSize(38, 38);
    playButton_->setCursor(Qt::PointingHandCursor);
    playButton_->setIconSize(QSize(18, 18));
    playButton_->setIcon(makePlayerIcon(PlayerIcon::Pause, 18, Qt::white));
    playButton_->setStyleSheet(QString::fromLatin1(kIconButtonStyle));
    playButton_->setFocusPolicy(Qt::NoFocus);
    positionLabel_ = new QLabel(QStringLiteral("0:00 / 0:00"), controlsOverlay_);
    positionLabel_->setStyleSheet(QStringLiteral(
        "color: rgba(255,255,255,205); font-size: 12px; font-weight: 500;"));
    row->addWidget(playButton_);
    row->addWidget(positionLabel_);
    row->addStretch(1);

    muteButton_ = new QPushButton(controlsOverlay_);
    muteButton_->setFixedSize(38, 38);
    muteButton_->setCursor(Qt::PointingHandCursor);
    muteButton_->setIconSize(QSize(18, 18));
    muteButton_->setIcon(makePlayerIcon(PlayerIcon::Volume, 18, Qt::white));
    muteButton_->setStyleSheet(QString::fromLatin1(kIconButtonStyle));
    muteButton_->setFocusPolicy(Qt::NoFocus);
    muteButton_->setCheckable(true);
    muteButton_->setChecked(muted_);

    cacheCombo_ = new QComboBox(controlsOverlay_);
    const std::vector<std::pair<QString, int>> cache_options{
        {QStringLiteral("4 MB"), 4}, {QStringLiteral("8 MB"), 8},
        {QStringLiteral("16 MB"), 16}, {QStringLiteral("32 MB"), 32},
        {QStringLiteral("64 MB"), 64}, {QStringLiteral("128 MB"), 128}};
    int cache_index = 2; // 16 MB fallback
    for (std::size_t i = 0U; i < cache_options.size(); ++i) {
        cacheCombo_->addItem(cache_options[i].first, cache_options[i].second);
        if (cache_options[i].second == cache_mib) {
            cache_index = static_cast<int>(i);
        }
    }
    cacheCombo_->setCurrentIndex(cache_index);
    cacheCombo_->setToolTip(QStringLiteral(
        "How much of the video to buffer in memory while streaming"));
    cacheCombo_->setStyleSheet(QStringLiteral(
        "QComboBox { background: rgba(255,255,255,18); border: none;"
        " border-radius: 8px; color: rgba(255,255,255,185);"
        " padding: 4px 8px; min-height: 26px; font-size: 11px; }"
        "QComboBox::drop-down { border: none; width: 18px; }"
        "QComboBox QAbstractItemView { background: #171c25; color: #edf1f6;"
        " border: 1px solid #2b3444; selection-background-color: #5b7cfa; }"));

    fullscreenButton_ = new QPushButton(controlsOverlay_);
    fullscreenButton_->setFixedSize(38, 38);
    fullscreenButton_->setCursor(Qt::PointingHandCursor);
    fullscreenButton_->setIconSize(QSize(18, 18));
    fullscreenButton_->setIcon(makePlayerIcon(PlayerIcon::Fullscreen, 18, Qt::white));
    fullscreenButton_->setStyleSheet(QString::fromLatin1(kIconButtonStyle));
    fullscreenButton_->setFocusPolicy(Qt::NoFocus);
    fullscreenButton_->setToolTip(QStringLiteral("Fullscreen (F)"));

    row->addWidget(muteButton_);
    row->addWidget(cacheCombo_);
    row->addWidget(fullscreenButton_);
    controlsLayout->addLayout(row);

    // Image-viewer row (hidden until an image is opened; zoom controls).
    auto* imageRow = new QHBoxLayout();
    imageRow->setSpacing(6);
    zoomOutButton_ = new QPushButton(QStringLiteral("−"), controlsOverlay_);
    zoomOutButton_->setToolTip(QStringLiteral("Zoom out (-)"));
    zoomLabel_ = new QLabel(QStringLiteral("Fit"), controlsOverlay_);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    zoomInButton_ = new QPushButton(QStringLiteral("+"), controlsOverlay_);
    zoomInButton_->setToolTip(QStringLiteral("Zoom in (+)"));
    fitButton_ = new QPushButton(QStringLiteral("Fit"), controlsOverlay_);
    fitButton_->setToolTip(QStringLiteral("Fit to window (0)"));
    for (auto* button : {zoomOutButton_, zoomInButton_, fitButton_}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: rgba(255,255,255,18); border: none;"
            " border-radius: 8px; color: rgba(255,255,255,220);"
            " padding: 4px 12px; min-height: 26px; font-size: 12px; }"
            "QPushButton:hover { background: rgba(255,255,255,38); }"));
    }
    zoomLabel_->setStyleSheet(QStringLiteral(
        "color: rgba(255,255,255,205); font-size: 12px; min-width: 46px;"));
    connect(zoomOutButton_, &QPushButton::clicked, this, [this] {
        zoomImage(0.8, QPointF(surface_->width() / 2.0, surface_->height() / 2.0));
    });
    connect(zoomInButton_, &QPushButton::clicked, this, [this] {
        zoomImage(1.25, QPointF(surface_->width() / 2.0, surface_->height() / 2.0));
    });
    connect(fitButton_, &QPushButton::clicked, this, [this] { resetImageFit(); });
    imageRow->addWidget(zoomOutButton_);
    imageRow->addWidget(zoomLabel_);
    imageRow->addWidget(zoomInButton_);
    imageRow->addWidget(fitButton_);
    imageRow->addStretch(1);
    controlsLayout->addLayout(imageRow);
    zoomOutButton_->hide();
    zoomLabel_->hide();
    zoomInButton_->hide();
    fitButton_->hide();

    // Position the chrome strips and the center button over the surface.
    layoutChrome();

    // Volume popup (PotPlayer-style: appears above the mute button on hover).
    volumePopup_ = new QFrame(this);
    volumePopup_->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    volumePopup_->setAttribute(Qt::WA_StyledBackground, true);
    volumePopup_->setStyleSheet(QStringLiteral(
        "QFrame { background: #171c25; border: 1px solid #2b3444;"
        " border-radius: 10px; }"
        "QSlider::groove:horizontal { height: 4px; background: #364154;"
        " border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #5b7cfa; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 14px; margin: -5px 0;"
        " border-radius: 7px; background: #ffffff; }"));
    auto* volLayout = new QHBoxLayout(volumePopup_);
    volLayout->setContentsMargins(12, 10, 12, 10);
    volLayout->setSpacing(8);
    auto* volIcon = new QLabel(volumePopup_);
    volIcon->setPixmap(makePlayerIcon(PlayerIcon::Volume, 16, QColor(238, 242, 248)));
    volLayout->addWidget(volIcon);
    volumeSlider_ = new QSlider(Qt::Horizontal, volumePopup_);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(volumePercent_);
    volumeSlider_->setFixedWidth(140);
    volLayout->addWidget(volumeSlider_);
    volumeHideTimer_.setSingleShot(true);
    volumeHideTimer_.setInterval(350);
    connect(&volumeHideTimer_, &QTimer::timeout, this, [this] {
        volumePopup_->hide();
    });
    muteButton_->installEventFilter(this);
    volumePopup_->installEventFilter(this);

    connect(playButton_, &QPushButton::clicked, this, [this] { togglePlayPause(); });
    connect(positionSlider_, &QSlider::sliderPressed, this, [this] {
        seeking_.store(true);
    });
    connect(positionSlider_, &QSlider::valueChanged, this, [this](const int value) {
        if (seeking_.load()) {
            // Live position preview while dragging.
            positionLabel_->setText(QStringLiteral("%1 / %2")
                .arg(formatTime(value), formatTime(durationMs_.load())));
        }
    });
    connect(positionSlider_, &QSlider::sliderReleased, this, [this] {
        doSeek(positionSlider_->value());
        seeking_.store(false);
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](const int value) {
        volumePercent_ = value;
        applyVolume();
        QSettings settings;
        settings.setValue(QStringLiteral("player/volume"), value);
        volumeHideTimer_.start();
    });
    connect(muteButton_, &QPushButton::toggled, this, [this](const bool checked) {
        muted_ = checked;
        applyVolume();
        QSettings settings;
        settings.setValue(QStringLiteral("player/muted"), checked);
    });
    connect(fullscreenButton_, &QPushButton::clicked, this, [this] { toggleFullscreen(); });
    connect(cacheCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int) {
            const std::size_t bytes = static_cast<std::size_t>(
                cacheCombo_->currentData().toInt()) << 20;
            decoder_.set_stream_cache_bytes(bytes);
            QSettings settings;
            settings.setValue(QStringLiteral("player/streamCacheMib"),
                cacheCombo_->currentData().toInt());
        });

    uiHideTimer_.setSingleShot(true);
    uiHideTimer_.setInterval(2500);
    connect(&uiHideTimer_, &QTimer::timeout, this, [this] {
        // Hide the chrome while the video plays and the mouse stays still.
        if (playing_.load()) {
            hideChrome();
        }
    });

    decoder_.set_stream_cache_bytes(
        static_cast<std::size_t>(cacheCombo_->currentData().toInt()) << 20);
    surfaceSize_ = surface_->size();

    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    timer_.start();
    playing_.store(true);
    worker_ = std::thread(&PlayerWindow::workerLoop, this);
}

PlayerWindow::~PlayerWindow() {
    qApp->removeEventFilter(this);
    timer_.stop();
    quit_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    decoder_.close();
    if (audioOutput_ != nullptr) {
        audioOutput_->stop();
    }
}

void PlayerWindow::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    layoutChrome();
    {
        std::lock_guard<std::mutex> guard(sizeMutex_);
        surfaceSize_ = surface_->size();
    }
    surface_->update();
}

void PlayerWindow::closeEvent(QCloseEvent* event) {
    timer_.stop();
    QDialog::closeEvent(event);
}

void PlayerWindow::keyPressEvent(QKeyEvent* event) {
    // The app-level event filter normally consumes player keys before they
    // reach the focused widget; this path covers the dialog itself having
    // focus (e.g. no child focused).
    if (handleKey(event)) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

bool PlayerWindow::handleKey(QKeyEvent* event) {
    if (imageMode_) {
        switch (event->key()) {
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            zoomImage(1.25, QPointF(surface_->width() / 2.0, surface_->height() / 2.0));
            return true;
        case Qt::Key_Minus:
            zoomImage(0.8, QPointF(surface_->width() / 2.0, surface_->height() / 2.0));
            return true;
        case Qt::Key_0:
            resetImageFit();
            return true;
        case Qt::Key_1:
            setImageZoom100();
            return true;
        case Qt::Key_Space:
        case Qt::Key_Left:
        case Qt::Key_Right:
            // No seek/play semantics for still images.
            return true;
        default:
            break;
        }
    }
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlayPause();
        return true;
    case Qt::Key_Left:
        seekRelative(-5000);
        return true;
    case Qt::Key_Right:
        seekRelative(5000);
        return true;
    case Qt::Key_Up:
        setVolumePercent(volumePercent_ + 5);
        return true;
    case Qt::Key_Down:
        setVolumePercent(volumePercent_ - 5);
        return true;
    case Qt::Key_F:
        toggleFullscreen();
        return true;
    case Qt::Key_Escape:
        if (fullscreen_) {
            toggleFullscreen();
        } else {
            close();
        }
        return true;
    default:
        return false;
    }
}

void PlayerWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    // A double-click means fullscreen, not two play/pause toggles.
    clickPending_ = false;
    toggleFullscreen();
    event->accept();
}

bool PlayerWindow::eventFilter(QObject* watched, QEvent* event) {
    // The filter is installed on qApp so the player's shortcuts work no
    // matter which child widget holds focus: the dialog's own keyPressEvent
    // never fires while a child (slider/button) consumes keys first.
    QObject* current = watched;
    while (current != nullptr && current != this) {
        current = current->parent();
    }
    if (current == this) {
        if (event->type() == QEvent::KeyPress
            && handleKey(static_cast<QKeyEvent*>(event))) {
            return true; // consumed by the player
        }
        if (event->type() == QEvent::MouseMove) {
            // Any mouse movement over the player brings the chrome back and
            // postpones the auto-hide.
            showChrome();
            uiHideTimer_.start();
        }
        if (watched == muteButton_ && event->type() == QEvent::Enter) {
            volumeHideTimer_.stop();
            showVolumePopup();
        }
        if (watched == muteButton_ && event->type() == QEvent::Leave) {
            volumeHideTimer_.start();
        }
        if (watched == volumePopup_ && event->type() == QEvent::Enter) {
            volumeHideTimer_.stop();
        }
        if (watched == volumePopup_ && event->type() == QEvent::Leave) {
            volumeHideTimer_.start();
        }
        if (imageMode_ && watched == surface_) {
            // Image viewer: wheel zooms (anchored at the cursor), left-drag
            // pans, and a plain click is consumed (no play/pause toggle).
            if (event->type() == QEvent::Wheel) {
                const auto* wheel = static_cast<QWheelEvent*>(event);
                if (wheel->angleDelta().y() != 0) {
                    zoomImage(wheel->angleDelta().y() > 0 ? 1.25 : 0.8,
                        wheel->pos());
                    return true;
                }
            }
            if (event->type() == QEvent::MouseButtonPress) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    panning_ = true;
                    panStartPos_ = mouse->pos();
                    panStartOffset_ = imageOffset_;
                    if (imageZoom_ <= 0.0) {
                        // Starting a drag from the fitted view: pin the fitted
                        // geometry so the pan has a concrete scale.
                        QImage frame;
                        {
                            std::lock_guard<std::mutex> guard(frameMutex_);
                            frame = latestFrame_;
                        }
                        if (!frame.isNull()) {
                            const double fit = std::min(
                                static_cast<double>(surface_->width()) / frame.width(),
                                static_cast<double>(surface_->height()) / frame.height());
                            imageZoom_ = fit;
                            panStartOffset_ = QPointF(
                                (surface_->width() - frame.width() * fit) / 2.0,
                                (surface_->height() - frame.height() * fit) / 2.0);
                            imageOffset_ = panStartOffset_;
                            surface_->setView(imageZoom_, imageOffset_);
                            updateZoomLabel();
                        }
                    }
                    return true;
                }
            }
            if (event->type() == QEvent::MouseMove && panning_) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->buttons() & Qt::LeftButton) {
                    imageOffset_ = panStartOffset_
                        + QPointF(mouse->pos() - panStartPos_);
                    surface_->setView(imageZoom_, imageOffset_);
                    return true;
                }
            }
            if (event->type() == QEvent::MouseButtonRelease && panning_) {
                const auto* mouse = static_cast<QMouseEvent*>(event);
                if (mouse->button() == Qt::LeftButton) {
                    panning_ = false;
                    return true;
                }
            }
        }
        if (watched == surface_ && event->type() == QEvent::MouseButtonRelease
            && !imageMode_) {
            // Single click on the video toggles play/pause (a delayed
            // single-click so a double-click still toggles fullscreen).
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                clickPending_ = true;
                QTimer::singleShot(250, this, [this] {
                    if (!clickPending_) {
                        return;
                    }
                    clickPending_ = false;
                    togglePlayPause();
                });
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

// Runs on the decode worker thread. Streams through MediaDecoder (whose
// read-ahead window may block the worker on refills — never the UI), paces
// frames against a monotonic real-time clock, and publishes frames/audio for
// the UI thread to consume.
void PlayerWindow::workerLoop() {
    QString open_error;
    if (!decoder_.open(vault_, video_id_, &open_error)) {
        {
            std::lock_guard<std::mutex> guard(errorMutex_);
            openError_ = open_error;
        }
        openFailed_.store(true);
        playing_.store(false);
        return;
    }

    durationMs_.store(decoder_.duration_ms());
    videoWidth_.store(decoder_.video_width());
    videoHeight_.store(decoder_.video_height());
    // A still-image codec (png/mjpeg/...) is opened as an image viewer:
    // single frame, zoom/pan instead of playback controls. Duration alone
    // cannot detect this — FFmpeg reports ~40 ms for a 1-frame image.
    imageModeFlag_.store(is_image_codec(decoder_.video_codec_name()));
    if (decoder_.has_audio()) {
        audioSampleRate_.store(decoder_.audio_sample_rate());
        audioChannels_.store(decoder_.audio_channels());
        audioReady_.store(true);
    }

    QElapsedTimer clock;
    clock.start();
    std::int64_t clock_base_ms = 0;
    std::int64_t clock_start_elapsed = clock.elapsed();
    std::int64_t last_pts = 0;

    while (!quit_.load()) {
        if (seekRequested_.exchange(false)) {
            const std::int64_t target = seekTargetMs_.load();
            const int seek_status = decoder_.seek_to(target);
            if (seek_status < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(seek_status, errbuf, sizeof(errbuf));
                qWarning() << "seek to" << target << "ms failed:" << errbuf;
                {
                    std::lock_guard<std::mutex> guard(seekErrorMutex_);
                    seekErrorMessage_ = QStringLiteral("Seek failed: %1")
                        .arg(QString::fromUtf8(errbuf, -1));
                }
                seekErrorPending_.store(true);
            }
            clock_base_ms = target;
            clock_start_elapsed = clock.elapsed();
            // The demuxer lands on the keyframe at/before the target. Decode
            // forward and skip (without publishing) until the frame is within
            // a quarter second of the target, then show it — otherwise sparse
            // keyframes make the seek look like it "didn't move" (it would
            // show a frame seconds earlier). Gap audio is discarded: the user
            // asked to jump, not to hear the skipped part.
            std::int64_t shown_pts = target;
            QImage shown;
            for (int guard = 0; guard < 120; ++guard) {
                (void)decoder_.take_audio_samples();
                DecodedFrame frame;
                if (!decoder_.decode_next_video_frame(&frame)) {
                    break;
                }
                if (frame.pts_ms >= target - 250) {
                    shown = frame.image;
                    shown_pts = frame.pts_ms;
                    if (frame.pts_ms >= target) {
                        break;
                    }
                }
            }
            last_pts = shown_pts;
            playheadMs_.store(shown_pts);
            if (!shown.isNull()) {
                publishFrame(shown, shown_pts);
            }
            continue;
        }
        if (rebaseRequested_.exchange(false)) {
            // Re-anchor the clock at the current position (pause/resume).
            clock_base_ms = last_pts;
            clock_start_elapsed = clock.elapsed();
        }
        if (!playing_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const std::int64_t elapsed = clock.elapsed();
        const std::int64_t target = clock_base_ms + (elapsed - clock_start_elapsed);
        if (last_pts >= target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        feedAudio();
        // A/V hold: if the sink is holding more than ~0.8 s of audio, the
        // device is draining slower than real time — hold the video instead of
        // reading Qt's flaky processedUSecs clock (which throttled playback
        // whenever the UI thread was busy publishing frames).
        const int rate = audioSampleRate_.load();
        const int channels = audioChannels_.load();
        if (rate > 0 && channels > 0) {
            AudioSink* sink = audioSinkAtomic_.load();
            if (sink != nullptr
                && sink->buffered_samples()
                    > static_cast<std::size_t>(rate) * static_cast<std::size_t>(channels) * 4U / 5U) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
        }
        DecodedFrame frame;
        if (!decoder_.decode_next_video_frame(&frame)) {
            // EOF: park until the user seeks or plays again.
            ended_.store(true);
            playing_.store(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        last_pts = frame.pts_ms;
        playheadMs_.store(last_pts);
        publishFrame(frame.image, last_pts);
    }
    decoder_.close();
}

void PlayerWindow::publishFrame(const QImage& image, const std::int64_t pts_ms) {
    QSize size;
    {
        std::lock_guard<std::mutex> guard(sizeMutex_);
        size = surfaceSize_;
    }
    if (!size.isEmpty() && !imageModeFlag_.load()) {
        // sws converts the next frames straight to the surface size, so the
        // published image is already display-sized (no second scale pass).
        decoder_.set_display_size(size.width(), size.height());
    } else if (imageModeFlag_.load()) {
        // Images keep their full resolution so zooming shows real pixels.
        decoder_.set_display_size(videoWidth_.load(), videoHeight_.load());
    }
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        latestFrame_ = image;
    }
    frameDirty_.store(true);
}

void PlayerWindow::feedAudio() {
    AudioSink* sink = audioSinkAtomic_.load();
    if (sink == nullptr) {
        return;
    }
    auto samples = decoder_.take_audio_samples();
    if (!samples.empty()) {
        sink->append(samples.data(), samples.size());
    }
    // Backpressure: never let the sink buffer more than ~1 s of audio while
    // the decoder catches up after a stall or seek.
    const int rate = audioSampleRate_.load();
    if (rate > 0 && sink->buffered_samples() > static_cast<std::size_t>(rate)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void PlayerWindow::requestSeek(const std::int64_t target_ms) {
    seekTargetMs_.store(target_ms);
    seekRequested_.store(true);
}

void PlayerWindow::doSeek(const std::int64_t target_ms) {
    // A manual seek means the video is no longer "ended" — otherwise the
    // tick would keep snapping the slider to the end and pressing Play after
    // the video had finished would force a restart from 0.
    ended_.store(false);
    requestSeek(target_ms);
    if (audioSink_ != nullptr) {
        audioSink_->clear();
    }
    if (audioOutput_ != nullptr) {
        audioOutput_->stop();
        audioOutput_->start(audioSink_);
        if (!playing_.load()) {
            audioOutput_->suspend();
        }
    }
    audioOffsetMs_ = target_ms;
    playheadMs_.store(target_ms);
    positionSlider_->setValue(static_cast<int>(std::min<std::int64_t>(
        target_ms, positionSlider_->maximum())));
    updatePositionLabel();
}

void PlayerWindow::togglePlayPause() {
    if (ended_.load()) {
        ended_.store(false);
        requestSeek(0);
        if (audioSink_ != nullptr) {
            audioSink_->clear();
        }
    }
    playing_.store(!playing_.load());
    playIconPlaying_ = playing_.load();
    playButton_->setIcon(makePlayerIcon(
        playing_.load() ? PlayerIcon::Pause : PlayerIcon::Play, 18, Qt::white));
    updateCenterButton();
    if (playing_.load()) {
        rebaseRequested_.store(true);
        if (audioOutput_ != nullptr) {
            audioOutput_->resume();
        }
        uiHideTimer_.start();
    } else {
        if (audioOutput_ != nullptr) {
            audioOutput_->suspend();
        }
        uiHideTimer_.stop();
        showChrome();
    }
}

void PlayerWindow::seekRelative(const std::int64_t delta_ms) {
    const std::int64_t duration = durationMs_.load();
    const std::int64_t target = std::clamp<std::int64_t>(
        playheadMs_.load() + delta_ms, 0, duration > 0 ? duration : playheadMs_.load());
    doSeek(target);
}

void PlayerWindow::setVolumePercent(const int percent) {
    volumeSlider_->setValue(std::clamp(percent, 0, 100));
}

void PlayerWindow::toggleMute() {
    muteButton_->setChecked(!muteButton_->isChecked());
}

void PlayerWindow::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        showChrome();
        setCursor(Qt::BlankCursor);
        showFullScreen();
        uiHideTimer_.start();
    } else {
        showChrome();
        showNormal();
        unsetCursor();
        uiHideTimer_.stop();
    }
}

void PlayerWindow::applyVolume() {
    if (audioOutput_ != nullptr) {
        audioOutput_->setVolume(muted_ ? 0.0f : (volumePercent_ / 100.0f));
    }
    muteButton_->setIcon(makePlayerIcon(
        (muted_ || volumePercent_ == 0) ? PlayerIcon::Muted : PlayerIcon::Volume,
        18, Qt::white));
}

// Shows the chrome (top bar + bottom controls; the center button follows the
// playback state). Instant show/hide — no opacity effect, which broke mouse
// event routing to the controls on Windows.
void PlayerWindow::showChrome() {
    chromeVisible_ = true;
    topOverlay_->show();
    controlsOverlay_->show();
    updateCenterButton();
}

// Hides the chrome (only while the video is playing; pausing re-shows it).
void PlayerWindow::hideChrome() {
    if (!chromeVisible_) {
        return;
    }
    chromeVisible_ = false;
    topOverlay_->hide();
    controlsOverlay_->hide();
    centerPlayButton_->hide();
}

// Positions the chrome strips and the center button over the surface.
void PlayerWindow::layoutChrome() {
    if (topOverlay_ == nullptr) {
        return;
    }
    const int w = surface_->width();
    const int h = surface_->height();
    topOverlay_->setGeometry(0, 0, w, 64);
    controlsOverlay_->setGeometry(0, h - 96, w, 96);
    centerPlayButton_->move((w - centerPlayButton_->width()) / 2,
        (h - centerPlayButton_->height()) / 2);
}

// The big center button is visible whenever playback is not running (and
// never in image mode).
void PlayerWindow::updateCenterButton() {
    if (imageMode_) {
        centerPlayButton_->hide();
        return;
    }
    const bool show = !playing_.load() || ended_.load();
    centerPlayButton_->setVisible(show && chromeVisible_);
}

// Switches the chrome to the image viewer: playback controls hidden, zoom
// controls shown, chrome stays put (no auto-hide), fitted view.
void PlayerWindow::enterImageMode() {
    imageMode_ = true;
    positionSlider_->hide();
    positionLabel_->hide();
    playButton_->hide();
    muteButton_->hide();
    cacheCombo_->hide();
    zoomOutButton_->show();
    zoomLabel_->show();
    zoomInButton_->show();
    fitButton_->show();
    uiHideTimer_.stop();
    updateCenterButton();
    showChrome();
    resetImageFit();
}

// Zooms by `factor`, keeping the image point under `cursor_pos` fixed. From
// the fitted view the fitted geometry is used as the starting scale.
void PlayerWindow::zoomImage(const double factor, const QPointF& cursor_pos) {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull()) {
        return;
    }
    double old_zoom = imageZoom_;
    QPointF old_offset = imageOffset_;
    if (old_zoom <= 0.0) {
        const double fit = std::min(
            static_cast<double>(surface_->width()) / frame.width(),
            static_cast<double>(surface_->height()) / frame.height());
        old_zoom = fit;
        old_offset = QPointF(
            (surface_->width() - frame.width() * fit) / 2.0,
            (surface_->height() - frame.height() * fit) / 2.0);
    }
    const double new_zoom = std::clamp(old_zoom * factor, 0.02, 64.0);
    const QPointF image_point = (cursor_pos - old_offset) / old_zoom;
    imageZoom_ = new_zoom;
    imageOffset_ = cursor_pos - image_point * new_zoom;
    surface_->setView(imageZoom_, imageOffset_);
    updateZoomLabel();
}

// Back to the aspect-fit view.
void PlayerWindow::resetImageFit() {
    imageZoom_ = 0.0;
    imageOffset_ = QPointF();
    surface_->setView(0.0, QPointF());
    updateZoomLabel();
}

// 100% zoom (one image pixel per screen pixel), centered.
void PlayerWindow::setImageZoom100() {
    QImage frame;
    {
        std::lock_guard<std::mutex> guard(frameMutex_);
        frame = latestFrame_;
    }
    if (frame.isNull()) {
        return;
    }
    imageZoom_ = 1.0;
    imageOffset_ = QPointF(
        (surface_->width() - frame.width()) / 2.0,
        (surface_->height() - frame.height()) / 2.0);
    surface_->setView(imageZoom_, imageOffset_);
    updateZoomLabel();
}

void PlayerWindow::updateZoomLabel() {
    zoomLabel_->setText(imageZoom_ <= 0.0
        ? QStringLiteral("Fit")
        : QStringLiteral("%1%").arg(qRound(imageZoom_ * 100.0)));
}

// Places the volume popup directly above the mute button.
void PlayerWindow::showVolumePopup() {
    volumePopup_->adjustSize();
    const QPoint top_center =
        muteButton_->mapToGlobal(QPoint(muteButton_->width() / 2, 0));
    volumePopup_->move(top_center.x() - volumePopup_->width() / 2,
        top_center.y() - volumePopup_->height() - 8);
    volumePopup_->show();
}
// Sizes the window so the video surface matches the video's pixel dimensions
// (never upscaling; capped to 80% of the screen area for large videos). Runs
// once per player window, from the UI timer, as soon as the worker reports
// the dimensions.
void PlayerWindow::fitToVideoSize() {
    const int video_w = videoWidth_.load();
    const int video_h = videoHeight_.load();
    if (video_w <= 0 || video_h <= 0 || autoSized_) {
        return;
    }
    autoSized_ = true;
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    const double scale = std::min(1.0, std::min(
        static_cast<double>(screen.width()) * 0.8 / video_w,
        static_cast<double>(screen.height()) * 0.8 / video_h));
    const int target_w = std::max(1, static_cast<int>(video_w * scale));
    const int target_h = std::max(1, static_cast<int>(video_h * scale));
    QSize surface_size;
    {
        std::lock_guard<std::mutex> guard(sizeMutex_);
        surface_size = surfaceSize_;
    }
    if (surface_size.isEmpty()) {
        surface_size = surface_->size();
    }
    // Resize the dialog by the delta between the target surface size and the
    // current surface size so the controls bar keeps its natural height.
    resize(width() + (target_w - surface_size.width()),
           height() + (target_h - surface_size.height()));
}

void PlayerWindow::tick() {
    // Fit the window to the video's pixel size once, as soon as the worker
    // reports the dimensions (before the first frame is even shown).
    fitToVideoSize();

    // Switch to the image viewer as soon as the worker reports an image.
    if (imageModeFlag_.load() && !imageMode_) {
        enterImageMode();
    }

    // Keep the center button and the play/pause icon in sync with state
    // changes that originate on the worker thread (e.g. EOF).
    updateCenterButton();
    if (!imageMode_) {
        const bool playing = playing_.load();
        if (playing != playIconPlaying_) {
            playIconPlaying_ = playing;
            playButton_->setIcon(makePlayerIcon(
                playing ? PlayerIcon::Pause : PlayerIcon::Play, 18, Qt::white));
        }
    }

    // Open-failure feedback.
    if (openFailed_.load() && !errorShown_) {
        errorShown_ = true;
        QString message;
        {
            std::lock_guard<std::mutex> guard(errorMutex_);
            message = openError_;
        }
        surface_->setText(QStringLiteral("Cannot play this video:\n%1").arg(message));
        playButton_->setEnabled(false);
        positionSlider_->setEnabled(false);
    }

    // Transient overlay for a failed seek (the worker logged the reason).
    if (seekErrorPending_.exchange(false)) {
        QString message;
        {
            std::lock_guard<std::mutex> guard(seekErrorMutex_);
            message = seekErrorMessage_;
        }
        surface_->setOverlay(message);
        QTimer::singleShot(3500, this, [this] { surface_->clearOverlay(); });
    }

    // Create the audio device once the worker has opened the stream.
    if (!audioSetupDone_ && audioReady_.load()) {
        audioSetupDone_ = true;
        QAudioFormat format;
        format.setSampleRate(audioSampleRate_.load());
        format.setChannelCount(audioChannels_.load());
        format.setSampleSize(16);
        format.setCodec(QStringLiteral("audio/pcm"));
        format.setByteOrder(QAudioFormat::LittleEndian);
        format.setSampleType(QAudioFormat::SignedInt);
        audioSink_ = new AudioSink(this);
        audioSinkAtomic_.store(audioSink_);
        audioOutput_ = new QAudioOutput(format, this);
        audioOutput_->setBufferSize(audioSampleRate_.load() / 2);
        applyVolume();
        audioOutput_->start(audioSink_);
        if (!playing_.load()) {
            audioOutput_->suspend();
        }
    }

    // QAudioOutput parks in IdleState (and stops pulling) after an empty read;
    // wake it while there may be fresh samples.
    if (audioOutput_ != nullptr && playing_.load()
        && (audioOutput_->state() == QAudio::IdleState
            || audioOutput_->state() == QAudio::StoppedState)) {
        audioOutput_->start(audioSink_);
    }

    // Paint the latest frame from the worker.
    if (frameDirty_.exchange(false)) {
        QImage image;
        {
            std::lock_guard<std::mutex> guard(frameMutex_);
            image = latestFrame_;
        }
        if (!image.isNull()) {
            lastFrame_ = image;
            surface_->setFrame(image);
        }
    }

    // Seek-bar range becomes known once the worker reports the duration.
    const std::int64_t duration = durationMs_.load();
    if (duration > 0 && positionSlider_->maximum() != static_cast<int>(duration)) {
        positionSlider_->setRange(0, static_cast<int>(std::max<std::int64_t>(duration, 1)));
        // Clicking the slider groove jumps by a page: ~5% of the video
        // (minimum 2 s) so a single click actually moves the position.
        positionSlider_->setPageStep(static_cast<int>(std::max<std::int64_t>(
            duration / 20, 2000)));
    }

    const std::int64_t pts = playheadMs_.load();
    // Never fight the user's slider drag: the tick must not overwrite the
    // position while the thumb is being moved (that made scrubbing snap back
    // and look like the seek never happened).
    if (!seeking_.load()) {
        positionSlider_->setValue(static_cast<int>(std::min<std::int64_t>(
            pts, positionSlider_->maximum())));
    }
    updatePositionLabel();

    if (ended_.load() && duration > 0 && !seeking_.load()) {
        playButton_->setText(QStringLiteral("Play"));
        positionSlider_->setValue(positionSlider_->maximum());
    }
}

QString PlayerWindow::formatTime(const std::int64_t ms) const {
    const auto total_seconds = ms / 1000;
    const auto hours = total_seconds / 3600;
    const auto minutes = (total_seconds % 3600) / 60;
    const auto seconds = total_seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

void PlayerWindow::updatePositionLabel() {
    positionLabel_->setText(QStringLiteral("%1 / %2")
        .arg(formatTime(playheadMs_.load()), formatTime(durationMs_.load())));
}

} // namespace videovault::app
