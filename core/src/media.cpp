#include "internal/media.hpp"

// FFmpeg 7.x public headers carry no extern "C" guards; wrap them so the
// C++ references match the DLLs' undecorated C exports.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace videovault::core::internal {
namespace {

constexpr std::size_t kAvioBufferSize = 64U * 1024U;
constexpr std::int64_t kMaxDecodePackets = 400;

// Debug aid for thumbnail position. Debug builds write to stderr (visible when
// the app is run from a console/terminal) and, on Windows, to the debugger
// (VS Output window, DebugView) via OutputDebugStringA — a GUI binary started
// from Explorer has no visible console, so stderr alone would be lost.
// Compiled out of Release builds.
void debug_log_thumbnail(const char* message) {
#if !defined(NDEBUG)
    std::fprintf(stderr, "[thumbnail] %s\n", message);
#if defined(_WIN32)
    OutputDebugStringA(message);
#endif
#else
    (void)message;
#endif
}

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
    void operator()(AVIOContext* context) const {
        if (context == nullptr) return;
        // avio_context_free() releases the AVIOContext but not the buffer we
        // passed to avio_alloc_context(): that must be freed with av_free(). It
        // may also have been swapped for a larger one by libavformat, which is
        // why the current pointer is freed rather than ours.
        if (context->buffer != nullptr) {
            av_freep(&context->buffer);
        }
        avio_context_free(&context);
    }
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
// `name_hint` is a filename given to FFmpeg purely so its extension-keyed
// demuxers (image2 and friends) can identify a bare image: the vault streams
// through a custom AVIO with no filename, so a PNG/WebP/BMP would otherwise be
// undemuxable (JPEG happened to work only because its `mjpeg` demuxer probes
// content).
Result<FormatHandle> try_open_format(PackageReader& reader, const char* name_hint) {
    // Each attempt consumes the stream; start from the beginning every time.
    auto rewound = reader.seek(0U);
    if (!rewound) {
        return rewound.error();
    }
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
    // avio_alloc_context zeroes `seekable`; without this flag FFmpeg refuses
    // every avio_seek (AVERROR(ENOSYS)), so avformat_seek_file always failed
    // and thumbnails silently fell back to the first frame. The reader's seek
    // callback (seek_packet) is fully functional — advertise it.
    raw_io->seekable = AVIO_SEEKABLE_NORMAL;

    AVFormatContext* raw_format = avformat_alloc_context();
    if (raw_format == nullptr) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg format context"};
    }
    raw_format->pb = handle.io.get();
    raw_format->flags |= AVFMT_FLAG_CUSTOM_IO;

    const int status = avformat_open_input(&raw_format, name_hint, nullptr, nullptr);
    if (status < 0) {
        // avformat_open_input() frees the context itself when it fails and
        // guarantees *ps == nullptr; freeing that pointer a second time (or
        // freeing a stale one) is a double free. It is exactly what crashed
        // with SIGSEGV inside avformat_free_context the first time the image
        // viewer decoded a photo, because every image attempt after the failed
        // container probe takes this path.
        raw_format = nullptr;
        return media_error("open media container", status);
    }
    handle.format.reset(raw_format);
    return handle;
}

// First try the input as a container (FFmpeg probes those by content). If that
// fails, retry with image filename hints so the extension-keyed image demuxers
// can take over — this is what makes imported photos (not just videos) work.
Result<FormatHandle> open_format(PackageReader& reader) {
    auto container = try_open_format(reader, nullptr);
    if (container) {
        return container;
    }
    // Hold on to the first failure's message, but do not hand that Result back
    // to the caller at the end: it carries a failed attempt's FormatHandle, and
    // the image attempts below supersede it.
    const VaultError first_error = container.error();
    static const char* const kImageHints[] = {
        "image.png", "image.jpg", "image.webp", "image.gif",
        "image.bmp", "image.tiff", "image.avif", "image.heic",
    };
    for (const char* hint : kImageHints) {
        auto as_image = try_open_format(reader, hint);
        if (as_image) {
            return as_image;
        }
    }
    return first_error;
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

// Seeks to roughly 30% into the stream and decodes the next video frame.
Result<AvFramePtr> decode_representative_frame(
    AVFormatContext* format,
    AVStream* stream) {
    const auto duration = duration_milliseconds(format, stream);
    // Single-frame media (images) have no "30% of the video": the only frame
    // is the first one, and seeking a single-frame demuxer can misbehave
    // (FFmpeg's image2 demuxer reports ~40 ms for one frame), so skip the
    // seek entirely and decode the first frame.
    const bool single_frame = duration <= 1000U;
    const auto target_ms = single_frame ? 0U : (duration * 3U / 10U);
    char debug[256];
    if (single_frame) {
        debug_log_thumbnail(
            "single-frame media (image): using the first frame (no seek)");
    } else {
        std::snprintf(debug, sizeof(debug),
            "duration %llu ms, thumbnail target %.3f s (30%% of the video)",
            static_cast<unsigned long long>(duration),
            static_cast<double>(target_ms) / 1000.0);
        debug_log_thumbnail(debug);
    }
    const auto target_ts = av_rescale_q(
        static_cast<std::int64_t>(target_ms), AV_TIME_BASE_Q, stream->time_base);
    int seek_status = 0;
    if (!single_frame) {
        std::snprintf(debug, sizeof(debug), "avio position before seek: %lld",
            static_cast<long long>(format->pb != nullptr ? format->pb->pos : -1));
        debug_log_thumbnail(debug);
        seek_status = avformat_seek_file(
            format, stream->index, INT64_MIN, target_ts, target_ts, 0);
        if (seek_status < 0) {
            // Some containers do not support seeking; fall back to the start.
            (void)avformat_seek_file(format, stream->index, INT64_MIN, 0, 0, 0);
            std::snprintf(debug, sizeof(debug),
                "avformat_seek_file failed (%d); fell back to the start", seek_status);
            debug_log_thumbnail(debug);
        } else {
            std::snprintf(debug, sizeof(debug),
                "avformat_seek_file ok (status %d)", seek_status);
            debug_log_thumbnail(debug);
        }
        // Drop any packets the demuxer buffered before the seek (e.g. during
        // header parsing); without this, av_read_frame can keep serving the
        // pre-seek stream from its internal buffer.
        avformat_flush(format);
        std::snprintf(debug, sizeof(debug), "avio position after seek: %lld",
            static_cast<long long>(format->pb != nullptr ? format->pb->pos : -1));
        debug_log_thumbnail(debug);
    } else {
        debug_log_thumbnail("single-frame media (image): using the first frame (no seek)");
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

    // Still images (PNG and the other image2 demuxers) are one packet, and
    // some of those demuxers do not mark it as a keyframe. Discarding
    // non-keyframes then drops the only frame, so the gallery never gets a
    // thumbnail. Videos still walk keyframes to reach the 30% mark.
    if (!single_frame) {
        codec->skip_frame = AVDISCARD_NONKEY;
    }

    // Decode walk. A "successful" container seek can still leave the demuxer
    // serving the stream from its start (observed with the custom AVIO:
    // avformat_seek_file returns 0 yet the first packet is at 0.000 s even
    // after avformat_flush). Instead of trusting the seek, decode forward
    // from the current position until the first frame at/after the target
    // appears; the closest frame before the target is kept as a fallback.
    // Bounded by a packet budget scaled to the target, so a no-op seek still
    // finishes quickly. Videos skip non-keyframes (set above); stills decode
    // their only frame.
    AvPacketPtr packet(av_packet_alloc());
    if (!packet) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg packet"};
    }
    const double target_seconds = static_cast<double>(target_ms) / 1000.0;
    // The walk must be able to reach the target even when the container seek
    // no-ops, so the budget scales with the target (≈8× the worst-case packet
    // count) and is NOT capped by kMaxDecodePackets, which bounds other
    // decode uses. When the seek works the walk stops after a few packets.
    const std::int64_t packet_budget =
        4000LL + static_cast<std::int64_t>(target_ms) * 8LL;
    std::int64_t packets_seen = 0;
    bool first_video_packet_logged = false;
    bool have_frame = false;
    bool reached_target = false;
    double frame_seconds = -1.0;
    double chosen_seconds = -1.0;
    AvFramePtr chosen;
    AvFramePtr frame(av_frame_alloc());
    if (!frame) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg frame"};
    }
    while (!reached_target
        && av_read_frame(format, packet.get()) >= 0
        && packets_seen < packet_budget) {
        ++packets_seen;
        if (packet->stream_index != stream->index) {
            av_packet_unref(packet.get());
            continue;
        }
        if (!first_video_packet_logged) {
            first_video_packet_logged = true;
            if (packet->pts != AV_NOPTS_VALUE && stream->time_base.den > 0) {
                std::snprintf(debug, sizeof(debug),
                    "first video packet after seek at %.3f s (packets_seen %lld)",
                    static_cast<double>(packet->pts)
                        * stream->time_base.num / stream->time_base.den,
                    static_cast<long long>(packets_seen));
            } else {
                std::snprintf(debug, sizeof(debug),
                    "first video packet after seek has unknown pts (packets_seen %lld)",
                    static_cast<long long>(packets_seen));
            }
            debug_log_thumbnail(debug);
        }
        status = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        if (status < 0) {
            return media_error("feed packet to video decoder", status);
        }
        while (true) {
            status = avcodec_receive_frame(codec.get(), frame.get());
            if (status == AVERROR(EAGAIN)) {
                break;
            }
            if (status < 0) {
                // Decode error / end of stream: keep whatever we already have.
                reached_target = true;
                break;
            }
            if (frame->pts != AV_NOPTS_VALUE && stream->time_base.den > 0) {
                frame_seconds = static_cast<double>(frame->pts)
                    * stream->time_base.num / stream->time_base.den;
            } else {
                frame_seconds = -1.0;
            }
            if (frame_seconds < 0.0) {
                // No timestamp (common for a single PNG/WebP frame). Keep the
                // first decoded picture; a video walk continues in case a
                // later packet does carry a timestamp.
                if (!have_frame) {
                    chosen = AvFramePtr(av_frame_clone(frame.get()));
                    if (!chosen) {
                        return VaultError{VaultErrorCode::CryptoFailure,
                            "unable to clone FFmpeg frame"};
                    }
                    have_frame = true;
                    if (single_frame) {
                        reached_target = true;
                    }
                }
            } else if (frame_seconds <= target_seconds) {
                // Closest keyframe at/before the target so far.
                if (!have_frame || frame_seconds >= chosen_seconds) {
                    chosen = AvFramePtr(av_frame_clone(frame.get()));
                    if (!chosen) {
                        return VaultError{VaultErrorCode::CryptoFailure,
                            "unable to clone FFmpeg frame"};
                    }
                    chosen_seconds = frame_seconds;
                    have_frame = true;
                }
            } else if (frame_seconds >= 0.0 && frame_seconds >= target_seconds) {
                // Reached the target: this frame is the best answer.
                chosen = AvFramePtr(av_frame_clone(frame.get()));
                if (!chosen) {
                    return VaultError{VaultErrorCode::CryptoFailure,
                        "unable to clone FFmpeg frame"};
                }
                chosen_seconds = frame_seconds;
                have_frame = true;
                reached_target = true;
                break;
            }
        }
    }
    if (!have_frame) {
        return VaultError{VaultErrorCode::UnsupportedVideoFormat,
            "no decodable video frame was found"};
    }
    if (chosen_seconds >= 0.0) {
        std::snprintf(debug, sizeof(debug),
            "decoded frame at %.3f s (target %.3f s, packets_seen %lld)",
            chosen_seconds, target_seconds, static_cast<long long>(packets_seen));
    } else {
        std::snprintf(debug, sizeof(debug),
            "decoded frame at unknown timestamp (target %.3f s, packets_seen %lld)",
            target_seconds, static_cast<long long>(packets_seen));
    }
    debug_log_thumbnail(debug);
    return chosen;
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

// MJPEG's YUV420P encoder rejects odd width or height. PNG frames are often
// odd (icons, cropped screenshots). Round up to the next even size.
std::uint32_t even_jpeg_dimension(const std::uint32_t value) {
    const auto even = value + (value & 1U);
    return even < 2U ? 2U : even;
}

// Gallery surface (#10151d). JPEG has no alpha, so transparent PNG pixels are
// composited onto this color instead of becoming black holes or a white box.
constexpr unsigned char kThumbBackgroundR = 16;
constexpr unsigned char kThumbBackgroundG = 21;
constexpr unsigned char kThumbBackgroundB = 29;

bool format_needs_opaque_flatten(const AVPixelFormat format) {
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
                (pixel[0] * alpha + static_cast<unsigned int>(kThumbBackgroundB) * inverse) / 255U);
            pixel[1] = static_cast<unsigned char>(
                (pixel[1] * alpha + static_cast<unsigned int>(kThumbBackgroundG) * inverse) / 255U);
            pixel[2] = static_cast<unsigned char>(
                (pixel[2] * alpha + static_cast<unsigned int>(kThumbBackgroundR) * inverse) / 255U);
            pixel[3] = 255;
        }
    }
}

Result<AvFramePtr> flatten_to_opaque_bgra(const AVFrame* source) {
    SwsPtr scaler(sws_getContext(
        source->width, source->height, static_cast<AVPixelFormat>(source->format),
        source->width, source->height, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg scaler"};
    }
    AvFramePtr flattened(av_frame_alloc());
    if (!flattened) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg frame"};
    }
    flattened->format = AV_PIX_FMT_BGRA;
    flattened->width = source->width;
    flattened->height = source->height;
    flattened->color_range = AVCOL_RANGE_JPEG;
    const int status = av_frame_get_buffer(flattened.get(), 0);
    if (status < 0) {
        return media_error("allocate flattened frame", status);
    }
    if (sws_scale(scaler.get(), source->data, source->linesize, 0, source->height,
            flattened->data, flattened->linesize) < 0) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to flatten the frame"};
    }
    composite_bgra(
        flattened->data[0], flattened->linesize[0], source->width, source->height);
    return flattened;
}

void apply_jpeg_color_range(SwsContext* scaler, const AVFrame* source) {
    const auto format = static_cast<AVPixelFormat>(source->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    // RGB, gray, and palette frames (PNG) are full range. Only planar YUV is
    // limited-range unless the frame says otherwise. Treating a PNG as MPEG
    // limited range and then tagging the JPEG as full range makes it dark.
    const bool yuv = desc != nullptr
        && (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) == 0
        && desc->nb_components >= 3;
    const bool full_range = !yuv
        || source->color_range == AVCOL_RANGE_JPEG
        || format == AV_PIX_FMT_YUVJ420P
        || format == AV_PIX_FMT_YUVJ422P
        || format == AV_PIX_FMT_YUVJ444P
        || format == AV_PIX_FMT_YUVJ440P;
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
    sws_setColorspaceDetails(
        scaler, coefficients, full_range ? 1 : 0, coefficients, 1,
        0, 1 << 16, 1 << 16);
}

Result<std::vector<unsigned char>> encode_jpeg(
    const AVFrame* source,
    std::uint32_t width,
    std::uint32_t height) {
    width = even_jpeg_dimension(width);
    height = even_jpeg_dimension(height);
    const AVFrame* pixels = source;
    AvFramePtr flattened;
    if (format_needs_opaque_flatten(static_cast<AVPixelFormat>(source->format))) {
        auto flat = flatten_to_opaque_bgra(source);
        if (!flat) {
            return flat.error();
        }
        flattened = std::move(flat.value());
        pixels = flattened.get();
    }
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
        pixels->width, pixels->height,
        static_cast<AVPixelFormat>(pixels->format),
        static_cast<int>(width), static_cast<int>(height),
        AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg scaler"};
    }
    // PNG/RGB is full range. Leaving swscale at MPEG limited range and then
    // tagging the JPEG as full range makes the thumbnail look dark and dull.
    apply_jpeg_color_range(scaler.get(), pixels);

    AvFramePtr scaled(av_frame_alloc());
    if (!scaled) {
        return VaultError{VaultErrorCode::CryptoFailure,
            "unable to allocate FFmpeg frame"};
    }
    scaled->format = AV_PIX_FMT_YUV420P;
    scaled->width = static_cast<int>(width);
    scaled->height = static_cast<int>(height);
    scaled->color_range = AVCOL_RANGE_JPEG;
    status = av_frame_get_buffer(scaled.get(), 0);
    if (status < 0) {
        return media_error("allocate scaled frame", status);
    }
    status = sws_scale(scaler.get(),
        pixels->data, pixels->linesize, 0, pixels->height,
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
    const auto jpeg_width = even_jpeg_dimension(dimensions.value().first);
    const auto jpeg_height = even_jpeg_dimension(dimensions.value().second);
    auto jpeg = encode_jpeg(frame.value().get(), jpeg_width, jpeg_height);
    if (!jpeg) {
        return jpeg.error();
    }
    out_width = jpeg_width;
    out_height = jpeg_height;
    return jpeg;
}

} // namespace videovault::core::internal
