#pragma once

#include "videovault/app/media_decoder.hpp"

#include <QAudioOutput>
#include <QColor>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSize>
#include <QSlider>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QAudioFormat;
class QComboBox;
class QKeyEvent;
class QMouseEvent;

namespace videovault::app {

// Pull-mode QIODevice that QAudioOutput reads decoded PCM from. The decode
// worker appends s16le samples; the audio thread drains them at its own pace.
// Thread-safe (append may be called from the worker thread).
class AudioSink final : public QIODevice {
    Q_OBJECT
public:
    explicit AudioSink(QObject* parent = nullptr);
    ~AudioSink() override;

    void append(const std::int16_t* samples, std::size_t count);
    void clear();
    // Number of interleaved s16 samples still buffered.
    std::size_t buffered_samples() const;

    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;

private:
    mutable std::mutex mutex_;
    std::vector<std::int16_t> buffer_;
    std::size_t read_cursor_{0};
};

// Video display surface: paints the latest frame directly (no QPixmap
// conversion), aspect-fit on black, with an optional centered text overlay
// ("Loading…" / error messages) and a transient bottom overlay for events
// like failed seeks. The worker's frames are already display-sized.
class VideoSurface final : public QWidget {
    Q_OBJECT
public:
    explicit VideoSurface(QWidget* parent = nullptr)
        : QWidget(parent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMouseTracking(true);
    }

    void setFrame(const QImage& image) {
        frame_ = image;
        update();
    }

    void setText(const QString& text) {
        text_ = text;
        frame_ = QImage();
        update();
    }

    void setOverlay(const QString& text) {
        overlayText_ = text;
        update();
    }

    void clearOverlay() {
        overlayText_.clear();
        update();
    }

    const QImage& frame() const { return frame_; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (!frame_.isNull()) {
            const QSize fitted = frame_.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect target(
                QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2),
                fitted);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(target, frame_);
            if (!overlayText_.isEmpty()) {
                // Transient message band at the bottom of the picture.
                const QRect band(0, height() - 34, width(), 34);
                painter.fillRect(band, QColor(0, 0, 0, 170));
                painter.setPen(QColor(255, 220, 120));
                painter.drawText(band, Qt::AlignCenter, overlayText_);
            }
        } else if (!text_.isEmpty()) {
            painter.setPen(QColor(200, 200, 200));
            painter.drawText(rect(), Qt::AlignCenter, text_);
        }
    }

private:
    QImage frame_;
    QString text_;
    QString overlayText_;
};

// Seek slider that jumps to the exact clicked position (instead of QSlider's
// default "page step" nudge), then drags normally from there.
class SeekSlider final : public QSlider {
    Q_OBJECT
public:
    explicit SeekSlider(QWidget* parent = nullptr)
        : QSlider(Qt::Horizontal, parent) {}

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && maximum() > minimum()) {
            const double fraction = std::clamp(
                static_cast<double>(event->pos().x())
                    / static_cast<double>(std::max(1, width())),
                0.0, 1.0);
            setValue(minimum()
                + static_cast<int>(fraction * (maximum() - minimum())));
        }
        QSlider::mousePressEvent(event);
    }
};

// PotPlayer-style window with a worker-thread decode pipeline: the worker
// streams through MediaDecoder (read-ahead window cache) and paces frames
// against a monotonic real-time clock, so decode stalls or slow scaling can
// never freeze or speed up playback. The UI thread only paints the latest
// frame, updates the seek bar, and owns the audio device.
class PlayerWindow final : public QDialog {
    Q_OBJECT
public:
    PlayerWindow(
        const std::shared_ptr<videovault::core::Vault>& vault,
        std::int64_t video_id,
        const QString& title,
        QWidget* parent = nullptr);
    ~PlayerWindow() override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void workerLoop();
    void publishFrame(const QImage& image, std::int64_t pts_ms);
    void feedAudio();
    void requestSeek(std::int64_t target_ms);
    void doSeek(std::int64_t target_ms);
    void togglePlayPause();
    void seekRelative(std::int64_t delta_ms);
    void setVolumePercent(int percent);
    void toggleMute();
    void toggleFullscreen();
    bool handleKey(QKeyEvent* event);
    void applyVolume();
    void showControls();
    void hideControls();
    void tick();
    void updatePositionLabel();
    QString formatTime(std::int64_t ms) const;

    std::shared_ptr<videovault::core::Vault> vault_;
    std::int64_t video_id_{0};
    MediaDecoder decoder_;

    VideoSurface* surface_{nullptr};
    QWidget* controlsBar_{nullptr};
    QPushButton* playButton_{nullptr};
    QSlider* positionSlider_{nullptr};
    QLabel* positionLabel_{nullptr};
    QPushButton* muteButton_{nullptr};
    QSlider* volumeSlider_{nullptr};
    QComboBox* cacheCombo_{nullptr};
    QPushButton* fullscreenButton_{nullptr};

    // Audio device (UI thread only).
    AudioSink* audioSink_{nullptr};
    QAudioOutput* audioOutput_{nullptr};
    std::atomic<AudioSink*> audioSinkAtomic_{nullptr};
    std::int64_t audioOffsetMs_{0};
    bool audioSetupDone_{false};

    // Decode worker communication.
    std::thread worker_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> ended_{false};
    std::atomic<bool> seeking_{false};
    std::atomic<bool> openFailed_{false};
    std::atomic<bool> errorShown_{false};
    std::atomic<bool> seekRequested_{false};
    std::atomic<bool> rebaseRequested_{false};
    std::atomic<std::int64_t> seekTargetMs_{0};
    std::atomic<std::int64_t> playheadMs_{0};
    std::atomic<std::int64_t> durationMs_{0};
    std::atomic<int> audioSampleRate_{0};
    std::atomic<int> audioChannels_{0};
    std::atomic<bool> audioReady_{false};

    // Frame slot (worker writes, UI reads on its timer).
    std::mutex frameMutex_;
    QImage latestFrame_;
    std::atomic<bool> frameDirty_{false};

    // Surface size for worker-side decode scaling.
    std::mutex sizeMutex_;
    QSize surfaceSize_;

    // Open error (worker -> UI).
    std::mutex errorMutex_;
    QString openError_;

    // Seek failure (worker -> UI overlay).
    std::mutex seekErrorMutex_;
    QString seekErrorMessage_;
    std::atomic<bool> seekErrorPending_{false};

    QTimer timer_;
    QTimer uiHideTimer_;

    bool fullscreen_{false};
    bool muted_{false};
    bool clickSeekPending_{false};
    int volumePercent_{100};
    QImage lastFrame_;
};

} // namespace videovault::app
