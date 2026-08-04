#include "videovault/app/player_window.hpp"

#include <QAudioFormat>
#include <QCloseEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QPixmap>
#include <QResizeEvent>
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

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    surface_ = new QLabel(this);
    surface_->setMinimumSize(320, 200);
    surface_->setAlignment(Qt::AlignCenter);
    surface_->setStyleSheet(QStringLiteral("background-color: #000000;"));
    layout->addWidget(surface_, 1);

    auto* controls = new QHBoxLayout();
    playButton_ = new QPushButton(QStringLiteral("Pause"), this);
    positionSlider_ = new QSlider(Qt::Horizontal, this);
    positionLabel_ = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    positionLabel_->setMinimumWidth(120);
    positionLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    controls->addWidget(playButton_);
    controls->addWidget(positionSlider_, 1);
    controls->addWidget(positionLabel_);
    layout->addLayout(controls);

    connect(playButton_, &QPushButton::clicked, this, [this] { togglePlayPause(); });
    connect(positionSlider_, &QSlider::sliderReleased, this, [this] { sliderReleased(); });

    QString open_error;
    if (!decoder_.open(vault, video_id, &open_error)) {
        surface_->setText(QStringLiteral("Cannot play this video:\n%1").arg(open_error));
        playButton_->setEnabled(false);
        positionSlider_->setEnabled(false);
        return;
    }

    const std::int64_t duration = decoder_.duration_ms();
    positionSlider_->setRange(0, static_cast<int>(std::max<std::int64_t>(duration, 1)));
    positionSlider_->setValue(0);
    updatePositionLabel();

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
        audioOutput_->setBufferSize(decoder_.audio_sample_rate() * 2);
        audioOutput_->start(audioSink_);
    }

    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    timer_.start();
    playing_ = true;
    wallClockBaseMs_ = 0;
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

void PlayerWindow::togglePlayPause() {
    playing_ = !playing_;
    playButton_->setText(playing_ ? QStringLiteral("Pause") : QStringLiteral("Play"));
    if (playing_) {
        wallClockBaseMs_ = video_position_ms();
        wallClockStartMs_ = QDateTime::currentMSecsSinceEpoch();
        if (audioOutput_ != nullptr) {
            audioOutput_->resume();
        }
    } else if (audioOutput_ != nullptr) {
        audioOutput_->suspend();
    }
}

void PlayerWindow::sliderReleased() {
    const std::int64_t target = positionSlider_->value();
    decoder_.seek_to(target);
    if (audioSink_ != nullptr) {
        audioSink_->clear();
    }
    if (audioOutput_ != nullptr) {
        audioOutput_->reset();
    }
    wallClockBaseMs_ = target;
    wallClockStartMs_ = QDateTime::currentMSecsSinceEpoch();
    lastPtsMs_ = target;
    updatePositionLabel();
}

std::int64_t PlayerWindow::audio_position_ms() const {
    if (audioOutput_ == nullptr) {
        return -1;
    }
    return audioOutput_->processedUSecs() / 1000;
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
}

void PlayerWindow::tick() {
    if (!playing_) {
        return;
    }
    const std::int64_t audio_pos = audio_position_ms();
    const std::int64_t target = audio_pos >= 0
        ? audio_pos
        : wallClockBaseMs_ + (QDateTime::currentMSecsSinceEpoch() - wallClockStartMs_);

    // Decode (and render) frames until the decode position passes the target.
    for (int guard = 0; guard < 16; ++guard) {
        feedAudio();
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

void PlayerWindow::updatePositionLabel() {
    const std::int64_t position = video_position_ms();
    const std::int64_t duration = decoder_.duration_ms();
    const auto format = [](const std::int64_t ms) {
        const auto total_seconds = ms / 1000;
        return QStringLiteral("%1:%2")
            .arg(total_seconds / 60)
            .arg(total_seconds % 60, 2, 10, QLatin1Char('0'));
    };
    positionLabel_->setText(QStringLiteral("%1 / %2")
        .arg(format(position), format(duration)));
}

} // namespace videovault::app
