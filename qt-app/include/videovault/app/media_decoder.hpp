#pragma once

#include <QImage>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace videovault::core {
class Vault;
} // namespace videovault::core

namespace videovault::app {

// A decoded video frame with its presentation time (ms).
struct DecodedFrame {
    QImage image;
    std::int64_t pts_ms{0};
};

// In-process FFmpeg player engine. It streams the plaintext of an encrypted
// video through `Vault::read_video_range` (bounded, per-chunk authenticated)
// via a custom AVIO context — nothing is ever written to disk and no
// whole-file plaintext buffer is materialized. Widget- and audio-device-free:
// the player window owns the QAudioOutput and paints the emitted frames.
class MediaDecoder {
public:
    MediaDecoder();
    ~MediaDecoder();

    MediaDecoder(const MediaDecoder&) = delete;
    MediaDecoder& operator=(const MediaDecoder&) = delete;

    // Opens the video for streaming. On failure returns false and fills
    // `error` with a human-readable message.
    bool open(
        const std::shared_ptr<videovault::core::Vault>& vault,
        std::int64_t video_id,
        QString* error);

    bool is_open() const;
    std::int64_t duration_ms() const;
    int video_width() const;
    int video_height() const;

    // True when an audio stream was found and its decoder is open.
    bool has_audio() const;
    int audio_sample_rate() const;
    int audio_channels() const;

    // Seeks to a presentation time (ms). Buffered audio is discarded.
    bool seek_to(std::int64_t ms);

    // Decodes packets until the next video frame is produced (audio packets
    // append s16le PCM to the internal buffer). Returns false at EOF or on
    // decode failure.
    bool decode_next_video_frame(DecodedFrame* frame);

    // Interleaved s16le PCM decoded since the last call, in the format
    // reported by audio_sample_rate()/audio_channels().
    std::vector<std::int16_t> take_audio_samples();

    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace videovault::app
