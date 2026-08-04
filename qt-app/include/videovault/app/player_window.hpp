#pragma once

#include "videovault/app/media_decoder.hpp"

#include <QAudioOutput>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class QAudioFormat;
class QKeyEvent;
class QMouseEvent;

namespace videovault::app {

// Pull-mode QIODevice that QAudioOutput reads decoded PCM from. The decode
// loop appends s16le samples; the audio thread drains them at its own pace.
class AudioSink final : public QIODevice {
    Q_OBJECT
public:
    explicit AudioSink(QObject* parent = nullptr);
    ~AudioSink() override;

    void append(const std::int16_t* samples, std::size_t count);
    void clear();

    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;

private:
    std::mutex mutex_;
    std::vector<std::int16_t> buffer_;
    std::size_t read_cursor_{0};
};

// PotPlayer-style window: black video surface, seek bar with live time,
// volume slider with mute, fullscreen (double-click / button / F), and
// keyboard shortcuts (Space play/pause, Left/Right ±5 s, Up/Down volume).
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
    void togglePlayPause();
    void seekRelative(std::int64_t delta_ms);
    void doSeek(std::int64_t target_ms);
    std::int64_t current_playhead_ms() const;
    void setVolumePercent(int percent);
    void toggleMute();
    void toggleFullscreen();
    void applyVolume();
    void showControls();
    void hideControls();
    std::int64_t audio_position_ms() const;
    std::int64_t video_position_ms() const;
    void feedAudio();
    void tick();
    void showFrame(const QImage& image);
    void updatePositionLabel();
    QString formatTime(std::int64_t ms) const;

    std::shared_ptr<videovault::core::Vault> vault_;
    std::int64_t video_id_{0};
    MediaDecoder decoder_;

    QLabel* surface_{nullptr};
    QWidget* controlsBar_{nullptr};
    QPushButton* playButton_{nullptr};
    QSlider* positionSlider_{nullptr};
    QLabel* positionLabel_{nullptr};
    QPushButton* muteButton_{nullptr};
    QSlider* volumeSlider_{nullptr};
    QPushButton* fullscreenButton_{nullptr};

    AudioSink* audioSink_{nullptr};
    QAudioOutput* audioOutput_{nullptr};
    QTimer timer_;
    QTimer uiHideTimer_;

    bool playing_{false};
    bool fullscreen_{false};
    bool muted_{false};
    int volume_percent_{100};
    std::int64_t audioClockOffsetMs_{0};
    std::int64_t wallClockBaseMs_{0};
    std::int64_t wallClockStartMs_{0};
    std::int64_t lastPtsMs_{0};
    QImage lastFrame_;
};

} // namespace videovault::app
