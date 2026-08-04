#include "internal/media.hpp"

// FFmpeg 7.x public headers carry no extern "C" guards; wrap them so the
// C++ references match the DLLs' undecorated C exports.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace videovault::core::internal {
namespace {

constexpr std::size_t kAvioBufferSize = 64U * 1024U;
constexpr std::int64_t kMaxDecodePackets = 400;

VaultError media_error(const char* operation, const int status) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, buffer, sizeof(buffer));
    return {VaultErrorCode::UnsupportedVideoFormat,
        std::string(operation) + ": " + buffer};
}

int read_packet(void* opaque, unsigned char* buffer, const int size) {
    auto* reader = static_cast<PackageReader*>(opaque);
    auto got = reader->read(
        std::span(buffer, static_cast<std::size_t>(size)));
    if (!got) {
        return AVERROR(EIO);
    }
    if (got.value() == 0U) {
        return AVERROR_EOF;
    }
    return static_cast<int>(got.value());
}

std::int64_t seek_packet(void* opaque, const std::int64_t offset, const int whence) {
    auto* reader = static_cast<PackageReader*>(opaque);
    if (whence == AVSEEK_SIZE) {
        return static_cast<std::int64_t>(reader->plaintext_size());
    }
    std::int64_t target = 0;
    switch (whence) {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = static_cast<std::int64_t>(reader->position()) + offset;
        break;
    case SEEK_END:
        target = static_cast<std::int64_t>(reader->plaintext_size()) + offset;
        break;
    default:
        return -1;
    }
    if (target < 0) {
        target = 0;
    }
    auto sought = reader->seek(static_cast<std::uint64_t>(target));
    if (!sought) {
        return -1;
    }
    return target;
}

struct AvIoContextDeleter {
    void operator()(AVIOContext* context) const { avio_context_free(&context); }
};
using AvIoPtr = std::unique_ptr<AVIOContext, AvIoContextDeleter>;

struct AvFormatDeleter {
    void operator()(AVFormatContext* context) const {
        avformat_close_input(&context);
    }
};
using AvFormatPtr = std::unique_ptr<AVFormatContext, AvFormatDeleter>;

struct AvCodecContextDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
using AvCodecPtr = std::unique_ptr<AVCodecContext, AvCodecContextDeleter>;

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

// Owns a format context and the custom AVIO it reads from. Destruction order
// matters: the format context must die before the AVIO it references.
struct FormatHandle {
    AvIoPtr io;
    AvFormatPtr format;
};

// Opens the format context with a custom AVIO layered over the reader. The
// returned handle is null when the input is not a recognizable container.
Result<FormatHandle> open_format(PackageReader& reader) {
    auto* io_buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (io_buffer == nullptr) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg IO buffer"};
    }
    AVIOContext* raw_io = avio_alloc_context(
        io_buffer, static_cast<int>(kAvioBufferSize), 0, &reader,
        read_packet, nullptr, seek_packet);
    if (raw_io == nullptr) {
        av_free(io_buffer);
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg IO context"};
    }
    FormatHandle handle;
    handle.io.reset(raw_io);

    AVFormatContext* raw_format = avformat_alloc_context();
    if (raw_format == nullptr) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg format context"};
    }
    raw_format->pb = handle.io.get();
    raw_format->flags |= AVFMT_FLAG_CUSTOM_IO;

    const int status = avformat_open_input(&raw_format, nullptr, nullptr, nullptr);
    if (status < 0) {
        if (raw_format != nullptr) {
            avformat_free_context(raw_format);
        }
        return media_error("open media container", status);
    }
    handle.format.reset(raw_format);
    return handle;
}

AVStream* first_video_stream(AVFormatContext* format) {
    for (unsigned int index = 0U; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return format->streams[index];
        }
    }
    return nullptr;
}

std::uint64_t duration_milliseconds(AVFormatContext* format, AVStream* stream) {
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        return static_cast<std::uint64_t>(
            format->duration * 1000 / AV_TIME_BASE);
    }
    if (stream != nullptr && stream->duration != AV_NOPTS_VALUE
        && stream->duration > 0 && stream->time_base.den > 0) {
        return static_cast<std::uint64_t>(
            stream->duration * 1000 * stream->time_base.num / stream->time_base.den);
    }
    return 0U;
}

std::uint32_t stream_rotation(AVStream* stream) {
    std::size_t size = 0U;
    auto* data = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &size);
    if (data == nullptr || size < 9U * sizeof(int32_t)) {
        return 0U;
    }
    const double rotation =
        av_display_rotation_get(reinterpret_cast<const int32_t*>(data));
    if (rotation == AV_NOPTS_VALUE) {
        return 0U;
    }
    const auto rounded = static_cast<int>(rotation + (rotation >= 0 ? 0.5 : -0.5));
    return static_cast<std::uint32_t>(((rounded % 360) + 360) % 360);
}

Result<AVStream*> find_video_stream(AVFormatContext* format) {
    const int status = avformat_find_stream_info(format, nullptr);
    if (status < 0) {
        return media_error("read stream information", status);
    }
    AVStream* stream = first_video_stream(format);
    if (stream == nullptr) {
        return VaultError{VaultErrorCode::UnsupportedVideoFormat,
            "the media container has no video stream"};
    }
    return stream;
}

// Seeks to roughly 10% into the stream (capped at 10 seconds) and decodes the
// next video frame.
Result<AvFramePtr> decode_representative_frame(
    AVFormatContext* format,
    AVStream* stream) {
    const auto duration = duration_milliseconds(format, stream);
    const auto target_ms = std::min<std::uint64_t>(duration / 10U, 10000U);
    const auto target_ts = av_rescale_q(
        static_cast<std::int64_t>(target_ms), AV_TIME_BASE_Q, stream->time_base);
    const int seek_status = avformat_seek_file(
        format, stream->index, INT64_MIN, target_ts, target_ts, 0);
    if (seek_status < 0) {
        // Some containers do not support seeking; fall back to the start.
        (void)avformat_seek_file(format, stream->index, INT64_MIN, 0, 0, 0);
    }

    const AVCodec* decoder =
        avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        return VaultError{VaultErrorCode::UnsupportedVideoFormat,
            "no decoder is available for the video codec"};
    }
    AvCodecPtr codec(avcodec_alloc_context3(decoder));
    if (!codec) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg decoder context"};
    }
    int status = avcodec_parameters_to_context(codec.get(), stream->codecpar);
    if (status < 0) {
        return media_error("apply codec parameters", status);
    }
    status = avcodec_open2(codec.get(), decoder, nullptr);
    if (status < 0) {
        return media_error("open video decoder", status);
    }

    std::int64_t packets_seen = 0;
    AvPacketPtr packet(av_packet_alloc());
    if (!packet) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg packet"};
    }
    while (av_read_frame(format, packet.get()) >= 0 && packets_seen < kMaxDecodePackets) {
        ++packets_seen;
        if (packet->stream_index != stream->index) {
            av_packet_unref(packet.get());
            continue;
        }
        status = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        if (status < 0) {
            return media_error("feed packet to video decoder", status);
        }
        AvFramePtr frame(av_frame_alloc());
        if (!frame) {
            return VaultError{VaultErrorCode::CryptoFailure,
                "unable to allocate FFmpeg frame"};
        }
        status = avcodec_receive_frame(codec.get(), frame.get());
        if (status == AVERROR(EAGAIN)) {
            continue;
        }
        if (status < 0) {
            return media_error("decode video frame", status);
        }
        return frame;
    }
    return VaultError{VaultErrorCode::UnsupportedVideoFormat,
        "no decodable video frame was found"};
}

Result<std::pair<std::uint32_t, std::uint32_t>> scaled_dimensions(
    const std::uint32_t source_width,
    const std::uint32_t source_height,
    const std::uint32_t max_dimension) {
    if (source_width == 0U || source_height == 0U) {
        return VaultError{VaultErrorCode::UnsupportedVideoFormat,
            "the video frame has an invalid size"};
    }
    if (max_dimension == 0U
        || (source_width <= max_dimension && source_height <= max_dimension)) {
        return std::pair{source_width, source_height};
    }
    const double scale = static_cast<double>(max_dimension)
        / static_cast<double>(std::max(source_width, source_height));
    const auto width = std::max<std::uint32_t>(
        1U, static_cast<std::uint32_t>(source_width * scale));
    const auto height = std::max<std::uint32_t>(
        1U, static_cast<std::uint32_t>(source_height * scale));
    return std::pair{width, height};
}

Result<std::vector<unsigned char>> encode_jpeg(
    const AVFrame* source,
    const std::uint32_t width,
    const std::uint32_t height) {
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (encoder == nullptr) {
        return VaultError{VaultErrorCode::UnsupportedVideoFormat,
            "no MJPEG encoder is available"};
    }
    AvCodecPtr codec(avcodec_alloc_context3(encoder));
    if (!codec) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg encoder context"};
    }
    codec->width = static_cast<int>(width);
    codec->height = static_cast<int>(height);
    codec->time_base = AVRational{1, 25};
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->color_range = AVCOL_RANGE_JPEG;
    int status = avcodec_open2(codec.get(), encoder, nullptr);
    if (status < 0) {
        return media_error("open JPEG encoder", status);
    }

    SwsPtr scaler(sws_getContext(
        source->width, source->height,
        static_cast<AVPixelFormat>(source->format),
        static_cast<int>(width), static_cast<int>(height),
        AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg scaler"};
    }

    AvFramePtr scaled(av_frame_alloc());
    if (!scaled) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg frame"};
    }
    scaled->format = AV_PIX_FMT_YUV420P;
    scaled->width = static_cast<int>(width);
    scaled->height = static_cast<int>(height);
    status = av_frame_get_buffer(scaled.get(), 0);
    if (status < 0) {
        return media_error("allocate scaled frame", status);
    }
    status = sws_scale(scaler.get(),
        source->data, source->linesize, 0, source->height,
        scaled->data, scaled->linesize);
    if (status < 0) {
        return media_error("scale video frame", status);
    }

    status = avcodec_send_frame(codec.get(), scaled.get());
    if (status < 0) {
        return media_error("send frame to JPEG encoder", status);
    }
    AvPacketPtr packet(av_packet_alloc());
    if (!packet) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg packet"};
    }
    status = avcodec_receive_packet(codec.get(), packet.get());
    if (status < 0) {
        return media_error("receive JPEG packet", status);
    }
    std::vector<unsigned char> jpeg(
        packet->data, packet->data + packet->size);
    av_packet_unref(packet.get());
    if (jpeg.empty()) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "JPEG encoding produced no data"};
    }
    return jpeg;
}

} // namespace

Result<MediaProbeResult> probe_media(PackageReader& reader) {
    auto handle = open_format(reader);
    if (!handle) {
        return handle.error();
    }
    AVFormatContext* format = handle.value().format.get();
    auto stream = find_video_stream(format);
    if (!stream) {
        return stream.error();
    }
    AVStream* video = stream.value();
    MediaProbeResult result;
    result.duration_ms = duration_milliseconds(format, video);
    result.width = static_cast<std::uint32_t>(video->codecpar->width);
    result.height = static_cast<std::uint32_t>(video->codecpar->height);
    result.rotation_degrees = stream_rotation(video);
    result.codec_name = avcodec_get_name(video->codecpar->codec_id);
    return result;
}

Result<std::vector<unsigned char>> extract_thumbnail_jpeg(
    PackageReader& reader,
    const std::uint32_t max_dimension,
    std::uint32_t& out_width,
    std::uint32_t& out_height) {
    auto handle = open_format(reader);
    if (!handle) {
        return handle.error();
    }
    AVFormatContext* format = handle.value().format.get();
    auto stream = find_video_stream(format);
    if (!stream) {
        return stream.error();
    }
    auto frame = decode_representative_frame(format, stream.value());
    if (!frame) {
        return frame.error();
    }
    auto dimensions = scaled_dimensions(
        static_cast<std::uint32_t>(frame.value()->width),
        static_cast<std::uint32_t>(frame.value()->height),
        max_dimension);
    if (!dimensions) {
        return dimensions.error();
    }
    auto jpeg = encode_jpeg(
        frame.value().get(), dimensions.value().first, dimensions.value().second);
    if (!jpeg) {
        return jpeg.error();
    }
    out_width = dimensions.value().first;
    out_height = dimensions.value().second;
    return jpeg;
}

} // namespace videovault::core::internal
