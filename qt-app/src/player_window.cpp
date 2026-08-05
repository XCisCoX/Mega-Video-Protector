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
#include <QSettings>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace videovault::app {

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
    : QDialog(parent), vault_(vault), video_id_(video_id) {
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

    // Controls bar (PotPlayer-style bottom bar).
    controlsBar_ = new QWidget(this);
    controlsBar_->setObjectName(QStringLiteral("playerControls"));
    controlsBar_->setStyleSheet(QStringLiteral(
        "QWidget#playerControls { background-color: #14181f; border-top: 1px solid #2b3444; }"
        "QWidget#playerControls QPushButton { min-height: 30px; padding: 0 12px; }"));
    auto* controlsLayout = new QVBoxLayout(controlsBar_);
    controlsLayout->setContentsMargins(12, 6, 12, 8);
    controlsLayout->setSpacing(6);

    positionSlider_ = new SeekSlider(controlsBar_);
    positionSlider_->setRange(0, 1);
    positionSlider_->setTracking(false);

    auto* row = new QHBoxLayout();
    row->setSpacing(10);
    playButton_ = new QPushButton(QStringLiteral("Pause"), controlsBar_);
    playButton_->setMinimumWidth(72);
    positionLabel_ = new QLabel(QStringLiteral("0:00 / 0:00"), controlsBar_);
    positionLabel_->setMinimumWidth(110);
    positionLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    muteButton_ = new QPushButton(QStringLiteral("Mute"), controlsBar_);
    muteButton_->setCheckable(true);
    muteButton_->setChecked(muted_);
    volumeSlider_ = new QSlider(Qt::Horizontal, controlsBar_);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(volumePercent_);
    volumeSlider_->setFixedWidth(110);
    auto* cacheLabel = new QLabel(QStringLiteral("Cache:"), controlsBar_);
    cacheCombo_ = new QComboBox(controlsBar_);
    const std::vector<std::pair<QString, int>> cache_options = {
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
    fullscreenButton_ = new QPushButton(QStringLiteral("Fullscreen"), controlsBar_);
    row->addWidget(playButton_);
    row->addWidget(positionLabel_);
    row->addStretch(1);
    row->addWidget(muteButton_);
    row->addWidget(volumeSlider_);
    row->addWidget(cacheLabel);
    row->addWidget(cacheCombo_);
    row->addWidget(fullscreenButton_);
    controlsLayout->addWidget(positionSlider_);
    controlsLayout->addLayout(row);
    layout->addWidget(controlsBar_);

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
        if (fullscreen_ && playing_.load()) {
            hideControls();
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
    // A double-click means fullscreen, not two seeks.
    clickSeekPending_ = false;
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
        if (event->type() == QEvent::Wheel) {
            // Mouse wheel seeks ±5 s (up = forward).
            const auto* wheel = static_cast<QWheelEvent*>(event);
            if (wheel->angleDelta().y() != 0) {
                seekRelative(wheel->angleDelta().y() > 0 ? 5000 : -5000);
                return true;
            }
        }
        if (watched == surface_ && event->type() == QEvent::MouseMove) {
            showControls();
            if (fullscreen_) {
                uiHideTimer_.start();
            }
        }
        if (watched == surface_ && event->type() == QEvent::MouseButtonRelease) {
            // Click on the video seeks to the clicked fraction (a delayed
            // single-click so a double-click still toggles fullscreen).
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && durationMs_.load() > 0) {
                const double fraction = std::clamp(
                    static_cast<double>(mouse->pos().x())
                        / static_cast<double>(std::max(1, surface_->width())),
                    0.0, 1.0);
                clickSeekPending_ = true;
                QTimer::singleShot(250, this, [this, fraction] {
                    if (!clickSeekPending_) {
                        return;
                    }
                    clickSeekPending_ = false;
                    doSeek(static_cast<std::int64_t>(
                        fraction * static_cast<double>(durationMs_.load())));
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
    if (!size.isEmpty()) {
        // sws converts the next frames straight to the surface size, so the
        // published image is already display-sized (no second scale pass).
        decoder_.set_display_size(size.width(), size.height());
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
    playButton_->setText(playing_.load()
        ? QStringLiteral("Pause") : QStringLiteral("Play"));
    if (playing_.load()) {
        rebaseRequested_.store(true);
        if (audioOutput_ != nullptr) {
            audioOutput_->resume();
        }
        if (fullscreen_) {
            uiHideTimer_.start();
        }
    } else {
        if (audioOutput_ != nullptr) {
            audioOutput_->suspend();
        }
        uiHideTimer_.stop();
        showControls();
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
        controlsBar_->hide();
        setCursor(Qt::BlankCursor);
        showFullScreen();
        uiHideTimer_.start();
    } else {
        controlsBar_->show();
        showControls();
        showNormal();
        unsetCursor();
        uiHideTimer_.stop();
    }
}

void PlayerWindow::applyVolume() {
    if (audioOutput_ != nullptr) {
        audioOutput_->setVolume(muted_ ? 0.0f : (volumePercent_ / 100.0f));
    }
    muteButton_->setText(muted_ ? QStringLiteral("Unmute") : QStringLiteral("Mute"));
}

void PlayerWindow::showControls() {
    if (!fullscreen_) {
        controlsBar_->show();
        unsetCursor();
        return;
    }
    controlsBar_->show();
    setCursor(Qt::ArrowCursor);
    uiHideTimer_.start();
}

void PlayerWindow::hideControls() {
    if (fullscreen_ && playing_.load()) {
        controlsBar_->hide();
        setCursor(Qt::BlankCursor);
    }
}

void PlayerWindow::tick() {
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
