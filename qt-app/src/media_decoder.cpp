#include "videovault/app/media_decoder.hpp"

#include "videovault/core/vault.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QDebug>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace videovault::app {
namespace {

constexpr std::size_t kAvioBufferSize = 128U * 1024U;
constexpr std::int64_t kMaxAudioSamples = 1U << 20; // ~8.7 min of s16le stereo 48k
constexpr std::uint64_t kDefaultStreamCacheBytes = 32U << 20; // 32 MiB

// Same dark surface the gallery thumbnails are composited onto, so a
// transparent PNG looks the same in the viewer as it does in the gallery.
constexpr unsigned char kImageBackgroundR = 16;
constexpr unsigned char kImageBackgroundG = 21;
constexpr unsigned char kImageBackgroundB = 29;

bool format_has_alpha_or_palette(const AVPixelFormat format) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    if (desc == nullptr) {
        return false;
    }
    return (desc->flags & (AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_PAL)) != 0;
}

void composite_bgra(
    unsigned char* pixels,
    const int linesize,
    const int width,
    const int height) {
    for (int y = 0; y < height; ++y) {
        auto* row = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(linesize);
        for (int x = 0; x < width; ++x) {
            unsigned char* pixel = row + static_cast<std::size_t>(x) * 4U;
            const unsigned int alpha = pixel[3];
            if (alpha == 255U) {
                continue;
            }
            const unsigned int inverse = 255U - alpha;
            pixel[0] = static_cast<unsigned char>(
                (pixel[0] * alpha + kImageBackgroundB * inverse) / 255U);
            pixel[1] = static_cast<unsigned char>(
                (pixel[1] * alpha + kImageBackgroundG * inverse) / 255U);
            pixel[2] = static_cast<unsigned char>(
                (pixel[2] * alpha + kImageBackgroundR * inverse) / 255U);
            pixel[3] = 255;
        }
    }
}

QString av_error_message(const char* operation, const int status) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, buffer, sizeof(buffer));
    return QStringLiteral("%1: %2").arg(
        QString::fromUtf8(operation), QString::fromUtf8(buffer));
}

struct AvFormatDeleter {
    void operator()(AVFormatContext* context) const {
        avformat_close_input(&context);
    }
};
using AvFormatPtr = std::unique_ptr<AVFormatContext, AvFormatDeleter>;

struct AvIoDeleter {
    void operator()(AVIOContext* context) const { avio_context_free(&context); }
};
using AvIoPtr = std::unique_ptr<AVIOContext, AvIoDeleter>;

struct AvCodecDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
using AvCodecPtr = std::unique_ptr<AVCodecContext, AvCodecDeleter>;

struct AvFrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

struct AvPacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
using AvPacketPtr = std::unique_ptr<AVPacket, AvPacketDeleter>;

struct SwsDeleter {
    void operator()(SwsContext* context) const { sws_freeContext(context); }
};
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

struct SwrDeleter {
    void operator()(SwrContext* context) const { swr_free(&context); }
};
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

// Read-ahead window cache: AVIO reads are served from one large in-memory
// window of the plaintext package instead of one Vault::read_video_range call
// (fresh package open + per-chunk decrypt) per tiny read. The window size is
// the user-selectable streaming memory budget; windows are 1 MiB aligned and
// refilled with a single big range read as the playhead approaches the end,
// so playback never hits the disk/crypto path again until the window rolls.
class StreamCache {
public:
    explicit StreamCache(const std::uint64_t budget) { set_budget(budget); }

    // The mutex is not movable; the window state is moved over under the
    // source's lock. This object is moved only during open(), before any
    // concurrent access.
    StreamCache(StreamCache&& other) noexcept
        : budget_(other.budget()),
          window_start_(other.window_start_),
          data_(std::move(other.data_)) {}

    StreamCache(const StreamCache&) = delete;
    StreamCache& operator=(const StreamCache&) = delete;

    void set_budget(const std::uint64_t budget) {
        std::lock_guard<std::mutex> guard(mutex_);
        budget_ = std::max<std::uint64_t>(budget, kMinBudget);
    }

    std::uint64_t budget() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return budget_;
    }

    // Serves up to `size` bytes at `position` (file bounds in `file_size`).
    // Returns the bytes copied (0 at/after EOF, -1 on an I/O error).
    std::int64_t read(
        const std::shared_ptr<videovault::core::Vault>& vault,
        const std::int64_t video_id,
        const std::int64_t position,
        unsigned char* buffer,
        const std::size_t size,
        const std::uint64_t file_size) {
        if (size == 0U) {
            return 0;
        }
        if (position < 0 || static_cast<std::uint64_t>(position) >= file_size) {
            return 0;
        }
        const auto pos = static_cast<std::uint64_t>(position);
        const std::uint64_t window_budget = budget();
        if (pos < window_start_ || pos >= window_start_ + data_.size()) {
            if (!load_window(vault, video_id, pos, file_size, window_budget)) {
                return -1;
            }
        }
        const auto buffered = window_start_ + data_.size();
        const auto take = std::min(size, static_cast<std::size_t>(buffered - pos));
        std::memcpy(buffer, data_.data() + static_cast<std::size_t>(pos - window_start_), take);
        // Low-water refill: once the read is within a quarter of the budget of
        // the buffered end, pull the rest of the window into memory so the
        // next reads never block on a fresh range read.
        if (pos + take + window_budget / 4U >= buffered) {
            extend_window(vault, video_id, file_size, window_budget);
        }
        return static_cast<std::int64_t>(take);
    }

    void clear() {
        data_.clear();
        window_start_ = kNoWindow;
    }

private:
    static constexpr std::uint64_t kAlignment = 1U << 20; // 1 MiB
    static constexpr std::uint64_t kMinBudget = 4U << 20; // 4 MiB
    static constexpr std::uint64_t kNoWindow =
        std::numeric_limits<std::uint64_t>::max();

    bool load_window(
        const std::shared_ptr<videovault::core::Vault>& vault,
        const std::int64_t video_id,
        const std::uint64_t position,
        const std::uint64_t file_size,
        const std::uint64_t budget) {
        data_.clear();
        window_start_ = position & ~(kAlignment - 1U);
        return extend_window(vault, video_id, file_size, budget);
    }

    bool extend_window(
        const std::shared_ptr<videovault::core::Vault>& vault,
        const std::int64_t video_id,
        const std::uint64_t file_size,
        const std::uint64_t budget) {
        const auto window_end = std::min(window_start_ + budget, file_size);
        const auto buffered = window_start_ + data_.size();
        if (window_end <= buffered) {
            return true; // window fully loaded
        }
        auto got = vault->read_video_range(
            video_id, buffered, static_cast<std::size_t>(window_end - buffered));
        if (!got) {
            return false;
        }
        data_.insert(data_.end(), got.value().begin(), got.value().end());
        return true;
    }

    mutable std::mutex mutex_;
    std::uint64_t budget_{kDefaultStreamCacheBytes};
    std::uint64_t window_start_{kNoWindow};
    std::vector<unsigned char> data_;
};

// Random-access plaintext source over the encrypted package. Reads are served
// from the read-ahead window cache; each window load authenticates every
// touched chunk through Vault::read_video_range exactly once.
class VaultSource {
public:
    VaultSource(
        std::shared_ptr<videovault::core::Vault> vault,
        const std::int64_t video_id,
        const std::uint64_t cache_budget)
        : vault_(std::move(vault)), video_id_(video_id), cache_(cache_budget) {}

    int read(unsigned char* buffer, const int size) {
        if (position_ >= plaintext_size_) {
            return AVERROR_EOF;
        }
        const auto got = cache_.read(
            vault_, video_id_, static_cast<std::int64_t>(position_), buffer,
            static_cast<std::size_t>(size), plaintext_size_);
        if (got < 0) {
            qWarning() << "vault read failed while buffering the video stream";
            return AVERROR(EIO);
        }
        if (got == 0) {
            return AVERROR_EOF;
        }
        position_ += static_cast<std::uint64_t>(got);
        return static_cast<int>(got);
    }

    void set_cache_budget(const std::uint64_t budget) { cache_.set_budget(budget); }

    std::int64_t seek(const std::int64_t offset, const int whence) {
        std::int64_t target = 0;
        switch (whence) {
        case AVSEEK_SIZE:
            return static_cast<std::int64_t>(plaintext_size_);
        case SEEK_SET:
            target = offset;
            break;
        case SEEK_CUR:
            target = static_cast<std::int64_t>(position_) + offset;
            break;
        case SEEK_END:
            target = static_cast<std::int64_t>(plaintext_size_) + offset;
            break;
        default:
            return -1;
        }
        if (target < 0) {
            target = 0;
        }
        position_ = static_cast<std::uint64_t>(target);
        return target;
    }

    void set_plaintext_size(const std::uint64_t size) { plaintext_size_ = size; }

private:
    std::shared_ptr<videovault::core::Vault> vault_;
    std::int64_t video_id_{0};
    std::uint64_t plaintext_size_{0};
    std::uint64_t position_{0};
    StreamCache cache_;
};

int source_read_packet(void* opaque, unsigned char* buffer, const int size) {
    return static_cast<VaultSource*>(opaque)->read(buffer, size);
}

std::int64_t source_seek(void* opaque, const std::int64_t offset, const int whence) {
    return static_cast<VaultSource*>(opaque)->seek(offset, whence);
}

// Owns the format context and the AVIO/source it reads from. The format
// context must die before the AVIO it references.
struct FormatHandle {
    VaultSource source;
    AvIoPtr io;
    AvFormatPtr format;
};

std::int64_t frame_pts_ms(const AVFrame* frame, const AVRational& time_base) {
    const std::int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
        ? frame->best_effort_timestamp
        : frame->pts;
    if (pts == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(pts, time_base, AVRational{1, 1000});
}

} // namespace

class MediaDecoder::Impl {
public:
    ~Impl() { close(); }

    bool open(
        const std::shared_ptr<videovault::core::Vault>& vault,
        const std::int64_t video_id,
        QString* error) {
        close();

        auto info = vault->media_info(video_id);
        if (!info) {
            if (error != nullptr) {
                *error = QString::fromUtf8(info.error().technical_detail.c_str());
            }
            return false;
        }

        // The package plaintext size for the AVIO size callback. The encrypted
        // .vvp file is LARGER than the plaintext (header + per-chunk AEAD
        // overhead); using it would skew SEEK_END/AVSEEK_SIZE and the stream
        // cache's EOF clamping. The original imported size is the plaintext
        // byte count.
        auto package = vault->package_path(video_id);
        if (!package) {
            if (error != nullptr) {
                *error = QStringLiteral("Unable to resolve the video package.");
            }
            return false;
        }
        std::error_code size_error;
        const auto package_size =
            std::filesystem::file_size(package.value(), size_error);
        if (size_error) {
            if (error != nullptr) {
                *error = QStringLiteral("The video package is missing.");
            }
            return false;
        }
        std::uint64_t plaintext_size = package_size;
        auto videos = vault->list_videos();
        if (videos) {
            for (const auto& video : videos.value()) {
                if (video.id == video_id) {
                    plaintext_size = video.original_size;
                    break;
                }
            }
        }

        handle_ = std::make_unique<FormatHandle>(
            FormatHandle{VaultSource(vault, video_id, cache_budget_), nullptr, nullptr});
        handle_->source.set_plaintext_size(plaintext_size);

        // Each attempt gets a fresh AVIO. A failed avformat_open_input() has
        // already consumed the previous buffer, and it frees the format
        // context itself (the pointer comes back null — freeing it again is
        // the double-free that crashed image opens).
        auto open_input = [&](const char* name_hint) -> int {
            handle_->format.reset();
            handle_->io.reset();
            handle_->source.seek(0, SEEK_SET);
            auto* buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
            if (buffer == nullptr) {
                return AVERROR(ENOMEM);
            }
            AVIOContext* raw_io = avio_alloc_context(
                buffer, static_cast<int>(kAvioBufferSize), 0, &handle_->source,
                source_read_packet, nullptr, source_seek);
            if (raw_io == nullptr) {
                av_free(buffer);
                return AVERROR(ENOMEM);
            }
            handle_->io.reset(raw_io);
            // avio_alloc_context zeroes `seekable`; without this flag FFmpeg
            // refuses every avio_seek (AVERROR(ENOSYS)), which made both the
            // player's seek_to() and the thumbnail seek silently fail (falling
            // back to the first frame). VaultSource::seek is functional — the
            // stream must advertise that.
            raw_io->seekable = AVIO_SEEKABLE_NORMAL;

            AVFormatContext* raw_format = avformat_alloc_context();
            if (raw_format == nullptr) {
                return AVERROR(ENOMEM);
            }
            raw_format->pb = handle_->io.get();
            raw_format->flags |= AVFMT_FLAG_CUSTOM_IO;
            const int open_status = avformat_open_input(
                &raw_format, name_hint, nullptr, nullptr);
            if (open_status < 0) {
                return open_status;
            }
            handle_->format.reset(raw_format);
            return 0;
        };

        // Containers probe by content. Bare stills often do not: image2 is
        // keyed by filename extension, and this AVIO has no path. The same
        // hints the thumbnail path uses let a PNG/WebP/BMP open in the viewer.
        static const char* const kImageHints[] = {
            nullptr, "image.png", "image.jpg", "image.webp", "image.gif",
            "image.bmp", "image.tiff",
        };
        int status = AVERROR_INVALIDDATA;
        for (const char* hint : kImageHints) {
            status = open_input(hint);
            if (status >= 0) {
                break;
            }
        }
        if (status < 0 || handle_->format == nullptr) {
            handle_.reset();
            if (error != nullptr) {
                *error = av_error_message("open media container", status);
            }
            return false;
        }
        AVFormatContext* raw_format = handle_->format.get();

        status = avformat_find_stream_info(raw_format, nullptr);
        if (status < 0) {
            if (error != nullptr) {
                *error = av_error_message("read stream information", status);
            }
            return false;
        }

        for (unsigned int index = 0U; index < raw_format->nb_streams; ++index) {
            AVStream* stream = raw_format->streams[index];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_ == -1) {
                video_stream_ = static_cast<int>(index);
                video_time_base_ = stream->time_base;
                const AVCodec* decoder =
                    avcodec_find_decoder(stream->codecpar->codec_id);
                if (decoder == nullptr) {
                    if (error != nullptr) {
                        *error = QStringLiteral("No decoder is available for the video codec.");
                    }
                    return false;
                }
                video_codec_.reset(avcodec_alloc_context3(decoder));
                status = avcodec_parameters_to_context(
                    video_codec_.get(), stream->codecpar);
                if (status < 0 || avcodec_open2(video_codec_.get(), decoder, nullptr) < 0) {
                    if (error != nullptr) {
                        *error = QStringLiteral("Unable to open the video decoder.");
                    }
                    return false;
                }
                video_width_ = video_codec_->width;
                video_height_ = video_codec_->height;
                rebuild_scaler();
            } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_ == -1) {
                audio_stream_ = static_cast<int>(index);
                const AVCodec* decoder =
                    avcodec_find_decoder(stream->codecpar->codec_id);
                if (decoder == nullptr) {
                    continue;
                }
                audio_codec_.reset(avcodec_alloc_context3(decoder));
                status = avcodec_parameters_to_context(
                    audio_codec_.get(), stream->codecpar);
                if (status < 0 || avcodec_open2(audio_codec_.get(), decoder, nullptr) < 0) {
                    audio_codec_.reset();
                    audio_stream_ = -1;
                    continue;
                }
                AVChannelLayout stereo_layout{};
                av_channel_layout_default(&stereo_layout, 2);
                SwrContext* raw_swr = nullptr;
                const auto swr_status = swr_alloc_set_opts2(
                    &raw_swr, &stereo_layout, AV_SAMPLE_FMT_S16,
                    audio_codec_->sample_rate, &audio_codec_->ch_layout,
                    audio_codec_->sample_fmt, audio_codec_->sample_rate, 0, nullptr);
                av_channel_layout_uninit(&stereo_layout);
                if (swr_status < 0 || swr_init(raw_swr) < 0) {
                    if (raw_swr != nullptr) {
                        swr_free(&raw_swr);
                    }
                    swr_.reset();
                    audio_codec_.reset();
                    audio_stream_ = -1;
                    continue;
                }
                swr_.reset(raw_swr);
                audio_sample_rate_ = audio_codec_->sample_rate;
                audio_channels_ = 2;
            }
        }

        if (video_stream_ == -1) {
            if (error != nullptr) {
                *error = QStringLiteral("The video has no decodable video stream.");
            }
            return false;
        }

        if (raw_format->duration != AV_NOPTS_VALUE && raw_format->duration > 0) {
            duration_ms_ = raw_format->duration * 1000 / AV_TIME_BASE;
        } else if (raw_format->streams[video_stream_]->duration != AV_NOPTS_VALUE) {
            duration_ms_ = frame_pts_ms_from_ts(
                raw_format->streams[video_stream_]->duration,
                raw_format->streams[video_stream_]->time_base);
        }
        return true;
    }

    void close() {
        handle_.reset();
        video_codec_.reset();
        audio_codec_.reset();
        scaler_.reset();
        swr_.reset();
        audio_samples_.clear();
        video_stream_ = -1;
        audio_stream_ = -1;
        duration_ms_ = 0;
        video_width_ = 0;
        video_height_ = 0;
        audio_sample_rate_ = 0;
        audio_channels_ = 0;
        scaler_src_fmt_ = AV_PIX_FMT_NONE;
        scaler_src_w_ = 0;
        scaler_src_h_ = 0;
    }

    bool is_open() const { return handle_ != nullptr && handle_->format != nullptr; }

    bool seek_to(const std::int64_t ms) {
        if (!is_open() || video_stream_ < 0) {
            last_seek_status_ = AVERROR(EINVAL);
            return false;
        }
        const auto target_ts = av_rescale_q(
            ms, AVRational{1, 1000}, handle_->format->streams[video_stream_]->time_base);
        // Backward seek lands on the keyframe at/before the target — the
        // classic, most compatible API; fall back to the range form.
        last_seek_status_ = av_seek_frame(
            handle_->format.get(), video_stream_, target_ts, AVSEEK_FLAG_BACKWARD);
        if (last_seek_status_ < 0) {
            last_seek_status_ = avformat_seek_file(
                handle_->format.get(), video_stream_, INT64_MIN, target_ts, target_ts, 0);
        }
        if (last_seek_status_ < 0) {
            return false;
        }
        // Drop packets the demuxer buffered before the seek; without this,
        // av_read_frame keeps serving the pre-seek stream.
        avformat_flush(handle_->format.get());
        if (video_codec_) {
            avcodec_flush_buffers(video_codec_.get());
        }
        if (audio_codec_) {
            avcodec_flush_buffers(audio_codec_.get());
        }
        audio_samples_.clear();
        return true;
    }

    int last_seek_status_{0};

    bool decode_next_video_frame(DecodedFrame* frame) {
        if (!is_open()) {
            return false;
        }
        while (true) {
            AvPacketPtr packet(av_packet_alloc());
            const int read_status = av_read_frame(handle_->format.get(), packet.get());
            if (read_status < 0) {
                return false; // EOF or error
            }
            const int stream_index = packet->stream_index;
            if (stream_index == video_stream_) {
                const int send_status = avcodec_send_packet(video_codec_.get(), packet.get());
                av_packet_unref(packet.get());
                if (send_status < 0) {
                    continue;
                }
                AvFramePtr decoded(av_frame_alloc());
                const int receive_status = avcodec_receive_frame(video_codec_.get(), decoded.get());
                if (receive_status == AVERROR(EAGAIN)) {
                    continue;
                }
                if (receive_status < 0) {
                    return false;
                }
                frame->image = convert_frame(decoded.get());
                frame->pts_ms = frame_pts_ms(decoded.get(), video_time_base_);
                return true;
            }
            if (stream_index == audio_stream_ && audio_codec_ && swr_) {
                const int send_status = avcodec_send_packet(audio_codec_.get(), packet.get());
                av_packet_unref(packet.get());
                if (send_status < 0) {
                    continue;
                }
                while (true) {
                    AvFramePtr decoded(av_frame_alloc());
                    const int receive_status = avcodec_receive_frame(audio_codec_.get(), decoded.get());
                    if (receive_status == AVERROR(EAGAIN) || receive_status == AVERROR_EOF) {
                        break;
                    }
                    if (receive_status < 0) {
                        break;
                    }
                    append_audio(decoded.get());
                }
            } else {
                av_packet_unref(packet.get());
            }
        }
    }

    std::vector<std::int16_t> take_audio_samples() {
        std::vector<std::int16_t> samples;
        samples.swap(audio_samples_);
        return samples;
    }

    std::int64_t duration_ms() const { return duration_ms_; }
    int video_width() const { return video_width_; }
    int video_height() const { return video_height_; }
    std::string video_codec_name() const {
        return video_codec_
            ? std::string(avcodec_get_name(video_codec_->codec_id))
            : std::string();
    }
    bool has_audio() const { return audio_stream_ != -1 && audio_codec_ && swr_; }
    int audio_sample_rate() const { return audio_sample_rate_; }
    int audio_channels() const { return audio_channels_; }

    void set_stream_cache_bytes(const std::size_t bytes) {
        cache_budget_ = std::max<std::size_t>(bytes, std::size_t{4U << 20});
        if (handle_) {
            handle_->source.set_cache_budget(cache_budget_);
        }
    }

    std::size_t stream_cache_bytes() const {
        return static_cast<std::size_t>(cache_budget_);
    }

private:
    static std::int64_t frame_pts_ms_from_ts(
        const std::int64_t ts, const AVRational& time_base) {
        if (ts == AV_NOPTS_VALUE) {
            return 0;
        }
        return av_rescale_q(ts, time_base, AVRational{1, 1000});
    }

    QImage convert_frame(const AVFrame* source) {
        const auto src_fmt = static_cast<AVPixelFormat>(source->format);
        // PNG (and other stills) often report AV_PIX_FMT_NONE on the codec
        // until the first frame is decoded, and the frame format can differ
        // from the codec's (palette vs RGBA). Rebuild from the frame itself.
        if (!scaler_ || src_fmt != scaler_src_fmt_
            || source->width != scaler_src_w_ || source->height != scaler_src_h_) {
            scaler_src_fmt_ = src_fmt;
            scaler_src_w_ = source->width;
            scaler_src_h_ = source->height;
            if (video_width_ <= 0) {
                video_width_ = source->width;
            }
            if (video_height_ <= 0) {
                video_height_ = source->height;
            }
            rebuild_scaler();
        }
        if (!scaler_) {
            return QImage();
        }
        const int output_width = display_width_ > 0 ? display_width_ : video_width_;
        const int output_height = display_height_ > 0 ? display_height_ : video_height_;
        if (output_width <= 0 || output_height <= 0) {
            return QImage();
        }
        AvFramePtr rgb(av_frame_alloc());
        rgb->format = AV_PIX_FMT_BGRA;
        rgb->width = output_width;
        rgb->height = output_height;
        if (av_frame_get_buffer(rgb.get(), 0) < 0) {
            return QImage();
        }
        sws_scale(scaler_.get(), source->data, source->linesize, 0, source->height,
            rgb->data, rgb->linesize);
        if (format_has_alpha_or_palette(src_fmt)) {
            // Format_RGB32 is painted with the alpha byte on Windows, so a
            // transparent PNG would show as holes instead of the picture.
            composite_bgra(rgb->data[0], rgb->linesize[0], output_width, output_height);
        }
        QImage image(
            rgb->data[0], output_width, output_height,
            rgb->linesize[0], QImage::Format_RGB32);
        return image.copy();
    }

    // The player tells the decoder the surface size so sws converts straight
    // to it (one scale instead of decode-scale + display-scale). Rebuilds the
    // scaler only when the size actually changes. Public so MediaDecoder can
    // forward it; only the decode worker thread calls it.
public:
    void set_display_size(const int width, const int height) {
        if (width > 0 && height > 0
            && (width != display_width_ || height != display_height_)) {
            display_width_ = width;
            display_height_ = height;
            rebuild_scaler();
        }
    }

    void rebuild_scaler() {
        const int src_w = scaler_src_w_ > 0
            ? scaler_src_w_
            : (video_codec_ ? video_codec_->width : 0);
        const int src_h = scaler_src_h_ > 0
            ? scaler_src_h_
            : (video_codec_ ? video_codec_->height : 0);
        const AVPixelFormat src_fmt = scaler_src_fmt_ != AV_PIX_FMT_NONE
            ? scaler_src_fmt_
            : (video_codec_ ? video_codec_->pix_fmt : AV_PIX_FMT_NONE);
        const int output_width = display_width_ > 0 ? display_width_ : video_width_;
        const int output_height = display_height_ > 0 ? display_height_ : video_height_;
        if (src_w <= 0 || src_h <= 0 || output_width <= 0 || output_height <= 0
            || src_fmt == AV_PIX_FMT_NONE) {
            scaler_.reset();
            return;
        }
        scaler_.reset(sws_getContext(
            src_w, src_h, src_fmt,
            output_width, output_height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (scaler_) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src_fmt);
            const bool yuv = desc != nullptr
                && (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) == 0
                && desc->nb_components >= 3;
            // PNG/gray/palette is full range. Leaving swscale at MPEG limited
            // range dulls the picture the same way the old thumbnails did.
            if (!yuv) {
                const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
                sws_setColorspaceDetails(
                    scaler_.get(), coefficients, 1, coefficients, 1,
                    0, 1 << 16, 1 << 16);
            }
        }
    }

    void append_audio(const AVFrame* decoded) {
        std::uint8_t* output[1] = {nullptr};
        int out_linesize = 0;
        const int out_samples = av_rescale_rnd(
            swr_get_delay(swr_.get(), decoded->sample_rate) + decoded->nb_samples,
            audio_sample_rate_, decoded->sample_rate, AV_ROUND_UP);
        if (av_samples_alloc(output, &out_linesize, audio_channels_, out_samples,
                AV_SAMPLE_FMT_S16, 0) < 0) {
            return;
        }
        const int converted = swr_convert(
            swr_.get(), output, out_samples,
            const_cast<const std::uint8_t**>(decoded->extended_data),
            decoded->nb_samples);
        const auto sample_count = static_cast<std::size_t>(
            std::max(0, converted) * audio_channels_);
        const std::size_t room =
            kMaxAudioSamples > audio_samples_.size()
            ? kMaxAudioSamples - audio_samples_.size()
            : 0U;
        const std::size_t take = std::min(sample_count, room);
        if (take > 0U) {
            const auto* samples = reinterpret_cast<const std::int16_t*>(output[0]);
            audio_samples_.insert(audio_samples_.end(), samples, samples + take);
        }
        av_freep(output);
    }

    std::unique_ptr<FormatHandle> handle_;
    int video_stream_{-1};
    int audio_stream_{-1};
    AVRational video_time_base_{0, 1};
    AvCodecPtr video_codec_;
    AvCodecPtr audio_codec_;
    SwsPtr scaler_;
    SwrPtr swr_;
    std::vector<std::int16_t> audio_samples_;
    std::int64_t duration_ms_{0};
    int video_width_{0};
    int video_height_{0};
    int audio_sample_rate_{0};
    int audio_channels_{0};
    std::uint64_t cache_budget_{kDefaultStreamCacheBytes};
    int display_width_{0};
    int display_height_{0};
    AVPixelFormat scaler_src_fmt_{AV_PIX_FMT_NONE};
    int scaler_src_w_{0};
    int scaler_src_h_{0};
};

MediaDecoder::MediaDecoder() : impl_(std::make_unique<Impl>()) {}
MediaDecoder::~MediaDecoder() = default;

bool MediaDecoder::open(
    const std::shared_ptr<videovault::core::Vault>& vault,
    const std::int64_t video_id,
    QString* error) {
    return impl_->open(vault, video_id, error);
}

bool MediaDecoder::is_open() const { return impl_->is_open(); }
std::int64_t MediaDecoder::duration_ms() const { return impl_->duration_ms(); }
int MediaDecoder::video_width() const { return impl_->video_width(); }
int MediaDecoder::video_height() const { return impl_->video_height(); }
std::string MediaDecoder::video_codec_name() const {
    return impl_->video_codec_name();
}
bool MediaDecoder::has_audio() const { return impl_->has_audio(); }
int MediaDecoder::audio_sample_rate() const { return impl_->audio_sample_rate(); }
int MediaDecoder::audio_channels() const { return impl_->audio_channels(); }
int MediaDecoder::seek_to(const std::int64_t ms) {
    if (impl_ == nullptr) {
        return AVERROR(EINVAL);
    }
    impl_->seek_to(ms);
    return impl_->last_seek_status_;
}
bool MediaDecoder::decode_next_video_frame(DecodedFrame* frame) {
    return impl_->decode_next_video_frame(frame);
}
std::vector<std::int16_t> MediaDecoder::take_audio_samples() {
    return impl_->take_audio_samples();
}

void MediaDecoder::set_stream_cache_bytes(const std::size_t bytes) {
    if (impl_) {
        impl_->set_stream_cache_bytes(bytes);
    }
}

void MediaDecoder::set_display_size(const int width, const int height) {
    if (impl_) {
        impl_->set_display_size(width, height);
    }
}

std::size_t MediaDecoder::stream_cache_bytes() const {
    if (!impl_) {
        return 0U;
    }
    return impl_->stream_cache_bytes();
}

void MediaDecoder::close() { impl_->close(); }

} // namespace videovault::app
