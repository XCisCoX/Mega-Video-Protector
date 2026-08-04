#include "videovault/app/media_decoder.hpp"

#include "videovault/core/vault.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QDebug>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace videovault::app {
namespace {

constexpr std::size_t kAvioBufferSize = 64U * 1024U;
constexpr std::int64_t kMaxAudioSamples = 1U << 20; // ~8.7 min of s16le stereo 48k

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

// Random-access plaintext source over the encrypted package. Each call goes
// through Vault::read_video_range, which authenticates every touched chunk.
class VaultSource {
public:
    VaultSource(
        std::shared_ptr<videovault::core::Vault> vault,
        const std::int64_t video_id)
        : vault_(std::move(vault)), video_id_(video_id) {}

    int read(unsigned char* buffer, const int size) {
        if (position_ >= plaintext_size_) {
            return AVERROR_EOF;
        }
        auto got = vault_->read_video_range(
            video_id_, position_, static_cast<std::size_t>(size));
        if (!got) {
            qWarning() << "vault read failed:" << got.error().technical_detail.c_str();
            return AVERROR(EIO);
        }
        if (got.value().empty()) {
            return AVERROR_EOF;
        }
        std::memcpy(buffer, got.value().data(), got.value().size());
        position_ += got.value().size();
        return static_cast<int>(got.value().size());
    }

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

        // The package plaintext size for the AVIO size callback.
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

        auto* io_buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
        if (io_buffer == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("Out of memory opening the video.");
            }
            return false;
        }
        handle_ = std::make_unique<FormatHandle>(
            FormatHandle{VaultSource(vault, video_id), nullptr, nullptr});
        handle_->source.set_plaintext_size(package_size);

        AVIOContext* raw_io = avio_alloc_context(
            io_buffer, static_cast<int>(kAvioBufferSize), 0, &handle_->source,
            source_read_packet, nullptr, source_seek);
        if (raw_io == nullptr) {
            av_free(io_buffer);
            if (error != nullptr) {
                *error = QStringLiteral("Unable to allocate the FFmpeg IO context.");
            }
            return false;
        }
        handle_->io.reset(raw_io);

        AVFormatContext* raw_format = avformat_alloc_context();
        if (raw_format == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("Unable to allocate the FFmpeg format context.");
            }
            return false;
        }
        raw_format->pb = handle_->io.get();
        raw_format->flags |= AVFMT_FLAG_CUSTOM_IO;

        int status = avformat_open_input(&raw_format, nullptr, nullptr, nullptr);
        if (status < 0) {
            if (raw_format != nullptr) {
                avformat_free_context(raw_format);
            }
            handle_.reset();
            if (error != nullptr) {
                *error = av_error_message("open media container", status);
            }
            return false;
        }
        handle_->format.reset(raw_format);

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
                scaler_.reset(sws_getContext(
                    video_codec_->width, video_codec_->height,
                    video_codec_->pix_fmt,
                    video_codec_->width, video_codec_->height,
                    AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr));
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
    }

    bool is_open() const { return handle_ != nullptr && handle_->format != nullptr; }

    bool seek_to(const std::int64_t ms) {
        if (!is_open()) {
            return false;
        }
        const auto target_ts = av_rescale_q(
            ms, AVRational{1, 1000}, handle_->format->streams[video_stream_]->time_base);
        const int status = avformat_seek_file(
            handle_->format.get(), video_stream_, INT64_MIN, target_ts, target_ts, 0);
        if (status < 0) {
            return false;
        }
        if (video_codec_) {
            avcodec_flush_buffers(video_codec_.get());
        }
        if (audio_codec_) {
            avcodec_flush_buffers(audio_codec_.get());
        }
        audio_samples_.clear();
        return true;
    }

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
    bool has_audio() const { return audio_stream_ != -1 && audio_codec_ && swr_; }
    int audio_sample_rate() const { return audio_sample_rate_; }
    int audio_channels() const { return audio_channels_; }

private:
    static std::int64_t frame_pts_ms_from_ts(
        const std::int64_t ts, const AVRational& time_base) {
        if (ts == AV_NOPTS_VALUE) {
            return 0;
        }
        return av_rescale_q(ts, time_base, AVRational{1, 1000});
    }

    QImage convert_frame(const AVFrame* source) {
        if (!scaler_) {
            return QImage();
        }
        AvFramePtr rgb(av_frame_alloc());
        rgb->format = AV_PIX_FMT_BGRA;
        rgb->width = video_width_;
        rgb->height = video_height_;
        if (av_frame_get_buffer(rgb.get(), 0) < 0) {
            return QImage();
        }
        sws_scale(scaler_.get(), source->data, source->linesize, 0, source->height,
            rgb->data, rgb->linesize);
        QImage image(
            rgb->data[0], video_width_, video_height_,
            rgb->linesize[0], QImage::Format_RGB32);
        return image.copy();
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
bool MediaDecoder::has_audio() const { return impl_->has_audio(); }
int MediaDecoder::audio_sample_rate() const { return impl_->audio_sample_rate(); }
int MediaDecoder::audio_channels() const { return impl_->audio_channels(); }
bool MediaDecoder::seek_to(const std::int64_t ms) { return impl_->seek_to(ms); }
bool MediaDecoder::decode_next_video_frame(DecodedFrame* frame) {
    return impl_->decode_next_video_frame(frame);
}
std::vector<std::int16_t> MediaDecoder::take_audio_samples() {
    return impl_->take_audio_samples();
}
void MediaDecoder::close() { impl_->close(); }

} // namespace videovault::app
