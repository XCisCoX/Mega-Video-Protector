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

namespace videovault::core {
class Vault;
} // namespace videovault::core

namespace videovault::app {

// Pull-mode PCM sink fed by the decoder thread of the player window.
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
    std::vector<std::int16_t> buffer_;
    std::size_t read_cursor_{0};
    std::mutex mutex_;
};

// Full-screen-friendly video player dialog. Streams the encrypted video in
// memory through MediaDecoder (Vault::read_video_range -> FFmpeg custom AVIO);
// audio is fed to a QAudioOutput in pull mode from the same decode stream.
class PlayerWindow final : public QDialog {
    Q_OBJECT
public:
    explicit PlayerWindow(
        const std::shared_ptr<videovault::core::Vault>& vault,
        std::int64_t video_id,
        const QString& title,
        QWidget* parent = nullptr);
    ~PlayerWindow() override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void togglePlayPause();
    void sliderReleased();
    void tick();

private:
    void showFrame(const QImage& image);
    void updatePositionLabel();
    std::int64_t audio_position_ms() const;
    std::int64_t video_position_ms() const;
    void feedAudio();

    MediaDecoder decoder_;
    std::shared_ptr<videovault::core::Vault> vault_;
    std::int64_t video_id_{0};

    QLabel* surface_{nullptr};
    QPushButton* playButton_{nullptr};
    QSlider* positionSlider_{nullptr};
    QLabel* positionLabel_{nullptr};
    QTimer timer_;
    QImage lastFrame_;

    QAudioOutput* audioOutput_{nullptr};
    AudioSink* audioSink_{nullptr};
    bool playing_{false};
    std::int64_t wallClockStartMs_{0};
    std::int64_t wallClockBaseMs_{0};
    std::int64_t lastPtsMs_{0};
};

} // namespace videovault::app
