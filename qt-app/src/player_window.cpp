#include "videovault/app/player_window.hpp"

#include <QAudioFormat>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
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
    volume_percent_ = settings.value(QStringLiteral("player/volume"), 100).toInt();
    volume_percent_ = std::clamp(volume_percent_, 0, 100);
    muted_ = settings.value(QStringLiteral("player/muted"), false).toBool();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    surface_ = new QLabel(this);
    surface_->setMinimumSize(320, 200);
    surface_->setAlignment(Qt::AlignCenter);
    surface_->setStyleSheet(QStringLiteral("background-color: #000000;"));
    surface_->setMouseTracking(true);
    surface_->installEventFilter(this);
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

    positionSlider_ = new QSlider(Qt::Horizontal, controlsBar_);
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
    volumeSlider_->setValue(volume_percent_);
    volumeSlider_->setFixedWidth(110);
    fullscreenButton_ = new QPushButton(QStringLiteral("Fullscreen"), controlsBar_);
    row->addWidget(playButton_);
    row->addWidget(positionLabel_);
    row->addStretch(1);
    row->addWidget(muteButton_);
    row->addWidget(volumeSlider_);
    row->addWidget(fullscreenButton_);
    controlsLayout->addWidget(positionSlider_);
    controlsLayout->addLayout(row);
    layout->addWidget(controlsBar_);

    connect(playButton_, &QPushButton::clicked, this, [this] { togglePlayPause(); });
    connect(positionSlider_, &QSlider::sliderReleased, this, [this] {
        doSeek(positionSlider_->value());
    });
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](const int value) {
        volume_percent_ = value;
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

    uiHideTimer_.setSingleShot(true);
    uiHideTimer_.setInterval(2500);
    connect(&uiHideTimer_, &QTimer::timeout, this, [this] {
        if (fullscreen_ && playing_) {
            hideControls();
        }
    });

    QString open_error;
    if (!decoder_.open(vault, video_id, &open_error)) {
        surface_->setText(QStringLiteral("Cannot play this video:\n%1").arg(open_error));
        playButton_->setEnabled(false);
        positionSlider_->setEnabled(false);
        volumeSlider_->setEnabled(false);
        muteButton_->setEnabled(false);
        fullscreenButton_->setEnabled(false);
        return;
    }

    const std::int64_t duration = decoder_.duration_ms();
    positionSlider_->setRange(0, static_cast<int>(std::max<std::int64_t>(duration, 1)));
    positionSlider_->setValue(0);
    updatePositionLabel();

    // Audio: create the sink and output, then prefill the sink with the first
    // ~200 ms of decoded audio before start(). QAudioOutput goes to IdleState
    // (and stays there) when readData() returns 0 on its first pull, so an
    // empty sink at startup leaves the sound dead until an explicit resume.
    std::int64_t initial_ms = 0;
    QImage initial_frame;
    if (decoder_.has_audio()) {
        QAudioFormat format;
        format.setSampleRate(decoder_.audio_sample_rate());
        format.setChannelCount(decoder_.audio_channels());
        format.setSampleSize(16);
        format.setCodec(QStringLiteral("audio/pcm"));
        format.setByteOrder(QAudioFormat::LittleEndian);
        format.setSampleType(QAudioFormat::SignedInt);
        audioSink_ = new AudioSink(this);
        audioOutput_ = new QAudioOutput(format, this);
        // ~125 ms of playback buffered in the device keeps A/V within a
        // fraction of a second instead of up to half a second.
        audioOutput_->setBufferSize(decoder_.audio_sample_rate() / 2);
        while (initial_ms < 200) {
            feedAudio();
            DecodedFrame frame;
            if (!decoder_.decode_next_video_frame(&frame)) {
                break;
            }
            initial_ms = frame.pts_ms;
            initial_frame = frame.image;
        }
        feedAudio();
        applyVolume();
        audioOutput_->start(audioSink_);
    }
    if (!initial_frame.isNull()) {
        lastPtsMs_ = initial_ms;
        wallClockBaseMs_ = initial_ms;
        lastFrame_ = initial_frame;
        showFrame(initial_frame);
        positionSlider_->setValue(static_cast<int>(std::min<std::int64_t>(
            initial_ms, positionSlider_->maximum())));
        updatePositionLabel();
    }

    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    timer_.start();
    playing_ = true;
    wallClockStartMs_ = QDateTime::currentMSecsSinceEpoch();
}

PlayerWindow::~PlayerWindow() {
    timer_.stop();
    if (audioOutput_ != nullptr) {
        audioOutput_->stop();
    }
    decoder_.close();
}

void PlayerWindow::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (!lastFrame_.isNull()) {
        showFrame(lastFrame_);
    }
}

void PlayerWindow::closeEvent(QCloseEvent* event) {
    timer_.stop();
    QDialog::closeEvent(event);
}

void PlayerWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlayPause();
        event->accept();
        return;
    case Qt::Key_Left:
        seekRelative(-5000);
        event->accept();
        return;
    case Qt::Key_Right:
        seekRelative(5000);
        event->accept();
        return;
    case Qt::Key_Up:
        volumeSlider_->setValue(volumeSlider_->value() + 5);
        event->accept();
        return;
    case Qt::Key_Down:
        volumeSlider_->setValue(volumeSlider_->value() - 5);
        event->accept();
        return;
    case Qt::Key_F:
        toggleFullscreen();
        event->accept();
        return;
    case Qt::Key_Escape:
        if (fullscreen_) {
            toggleFullscreen();
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    QDialog::keyPressEvent(event);
}

void PlayerWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    toggleFullscreen();
    event->accept();
}

bool PlayerWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == surface_ && event->type() == QEvent::MouseMove) {
        showControls();
        if (fullscreen_) {
            uiHideTimer_.start();
        }
    }
    return QDialog::eventFilter(watched, event);
}

void PlayerWindow::togglePlayPause() {
    playing_ = !playing_;
    playButton_->setText(playing_ ? QStringLiteral("Pause") : QStringLiteral("Play"));
    if (playing_) {
        // Restart from the beginning when the video ended.
        if (lastPtsMs_ >= decoder_.duration_ms() - 250) {
            doSeek(0);
        }
        wallClockBaseMs_ = video_position_ms();
        wallClockStartMs_ = QDateTime::currentMSecsSinceEpoch();
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
    const std::int64_t target = std::clamp<std::int64_t>(
        video_position_ms() + delta_ms, 0, decoder_.duration_ms());
    doSeek(target);
}

void PlayerWindow::doSeek(const std::int64_t target_ms) {
    decoder_.seek_to(target_ms);
    if (audioSink_ != nullptr) {
        audioSink_->clear();
    }
    // Decode the first frame at the target so the seek is visible instantly
    // (audio samples from this read go back into the refilled sink).
    std::int64_t shown_ms = target_ms;
    QImage shown;
    DecodedFrame frame;
    if (decoder_.decode_next_video_frame(&frame)) {
        shown_ms = frame.pts_ms;
        shown = frame.image;
    }
    feedAudio();
    if (audioOutput_ != nullptr) {
        // stop()+start() restarts the processing clock (processedUSecs -> 0)
        // so the offset-based clock below stays exact after the seek.
        audioOutput_->stop();
        audioOutput_->start(audioSink_);
        if (!playing_) {
            audioOutput_->suspend();
        }
    }
    audioClockOffsetMs_ = target_ms;
    wallClockBaseMs_ = shown_ms;
    wallClockStartMs_ = QDateTime::currentMSecsSinceEpoch();
    lastPtsMs_ = shown_ms;
    positionSlider_->setValue(static_cast<int>(std::min<std::int64_t>(
        shown_ms, positionSlider_->maximum())));
    updatePositionLabel();
    if (!shown.isNull()) {
        showFrame(shown);
    }
}

std::int64_t PlayerWindow::current_playhead_ms() const {
    // The audio clock is authoritative only while the device is actively
    // consuming samples; while it is idle/stopped (startup, seek reset) it
    // reports 0, which would race the video ahead at decode speed.
    if (audioOutput_ != nullptr && audioOutput_->state() == QAudio::ActiveState) {
        return audioClockOffsetMs_ + audioOutput_->processedUSecs() / 1000;
    }
    return wallClockBaseMs_ + (QDateTime::currentMSecsSinceEpoch() - wallClockStartMs_);
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
        audioOutput_->setVolume(muted_ ? 0.0f : (volume_percent_ / 100.0f));
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
    if (fullscreen_ && playing_) {
        controlsBar_->hide();
        setCursor(Qt::BlankCursor);
    }
}

std::int64_t PlayerWindow::audio_position_ms() const {
    if (audioOutput_ == nullptr) {
        return -1;
    }
    return audioClockOffsetMs_ + audioOutput_->processedUSecs() / 1000;
}

std::int64_t PlayerWindow::video_position_ms() const {
    return lastPtsMs_;
}

void PlayerWindow::feedAudio() {
    if (audioSink_ == nullptr) {
        return;
    }
    auto samples = decoder_.take_audio_samples();
    if (!samples.empty()) {
        audioSink_->append(samples.data(), samples.size());
    }
    // QAudioOutput parks itself in IdleState and stops pulling when a read
    // comes back empty; any subsequent data is ignored until an explicit
    // resume. Wake it whenever there is a chance new samples just arrived.
    if (audioOutput_ != nullptr
        && (audioOutput_->state() == QAudio::IdleState
            || audioOutput_->state() == QAudio::StoppedState)) {
        audioOutput_->start(audioSink_);
    }
}

void PlayerWindow::tick() {
    if (!playing_) {
        return;
    }
    const std::int64_t target = current_playhead_ms();

    // Decode frames up to the playhead. The video must never run ahead of the
    // audio clock, so skip decoding entirely once lastPtsMs_ already reached
    // the target — otherwise a single decoded frame per 16 ms tick plays
    // low-fps content at 2-6x real speed and A/V drift grows unbounded.
    for (int guard = 0; guard < 16; ++guard) {
        feedAudio();
        if (lastPtsMs_ >= target) {
            break;
        }
        DecodedFrame frame;
        if (!decoder_.decode_next_video_frame(&frame)) {
            // EOF: pause at the end.
            playing_ = false;
            playButton_->setText(QStringLiteral("Play"));
            positionSlider_->setValue(positionSlider_->maximum());
            updatePositionLabel();
            return;
        }
        lastPtsMs_ = frame.pts_ms;
        showFrame(frame.image);
        positionSlider_->setValue(static_cast<int>(std::min<std::int64_t>(
            frame.pts_ms, positionSlider_->maximum())));
        if (frame.pts_ms >= target) {
            break;
        }
    }
    updatePositionLabel();
}

void PlayerWindow::showFrame(const QImage& image) {
    lastFrame_ = image;
    if (image.isNull()) {
        return;
    }
    const QSize fitted = image.size().scaled(
        surface_->size(), Qt::KeepAspectRatio);
    surface_->setPixmap(QPixmap::fromImage(image.scaled(
        fitted, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
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
        .arg(formatTime(video_position_ms()), formatTime(decoder_.duration_ms())));
}

} // namespace videovault::app
