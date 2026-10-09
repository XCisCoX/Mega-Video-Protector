// JNI bridge between the native Kotlin/Compose app and the C++ VideoVaultCore.
// One vault is open at a time (like the desktop app). Complex results are
// marshalled as JSON strings; thumbnails/frames come back as byte arrays.
// The frame decoder mirrors core/src/media.cpp: FFmpeg reads the decrypted
// stream through Vault::read_video_range — nothing is ever written to disk.

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "videovault/core/vault.hpp"

using namespace videovault::core;

namespace {

std::mutex g_mutex;
std::unique_ptr<Vault> g_vault;

// Plaintext of small videos, kept while the vault is unlocked. A loop used to
// open the package and decrypt it again on every repeat — for a clip under a
// second that cost more than the clip itself. The first read pays for the
// decrypt; later seeks and repeats copy from here.
constexpr std::size_t kPlainFileCap = 12U << 20;
constexpr std::size_t kPlainTotalCap = 48U << 20;
std::unordered_map<std::int64_t, std::uint64_t> g_sizes;
std::unordered_map<std::int64_t, std::vector<unsigned char>> g_plain;
std::size_t g_plain_bytes = 0;

void wipe_plain(std::vector<unsigned char>& bytes) {
    std::fill(bytes.begin(), bytes.end(), 0U);
    bytes.clear();
    bytes.shrink_to_fit();
}

void forget_plain(const std::int64_t id) {
    auto it = g_plain.find(id);
    if (it != g_plain.end()) {
        g_plain_bytes -= it->second.size();
        wipe_plain(it->second);
        g_plain.erase(it);
    }
    g_sizes.erase(id);
}

void clear_plain() {
    for (auto& entry : g_plain) wipe_plain(entry.second);
    g_plain.clear();
    g_sizes.clear();
    g_plain_bytes = 0;
}

std::optional<std::uint64_t> lookup_size(Vault& vault, const std::int64_t id) {
    auto known = g_sizes.find(id);
    if (known != g_sizes.end()) return known->second;
    auto list = vault.list_videos();
    if (!list) return std::nullopt;
    std::optional<std::uint64_t> found;
    for (const auto& video : list.value()) {
        g_sizes[video.id] = video.original_size;
        if (video.id == id) found = video.original_size;
    }
    return found;
}

// Loads the whole plaintext once when it fits in the cap. Returns null when
// the video is missing or too big to keep (those keep streaming per range).
const std::vector<unsigned char>* ensure_plain(Vault& vault, const std::int64_t id) {
    auto it = g_plain.find(id);
    if (it != g_plain.end()) return &it->second;
    auto size = lookup_size(vault, id);
    if (!size || size.value() == 0U || size.value() > kPlainFileCap) return nullptr;
    auto all = vault.read_video_range(id, 0U, static_cast<std::size_t>(size.value()));
    if (!all || all.value().size() != static_cast<std::size_t>(size.value())) return nullptr;
    while (!g_plain.empty() && g_plain_bytes + all.value().size() > kPlainTotalCap) {
        auto drop = g_plain.begin();
        g_plain_bytes -= drop->second.size();
        wipe_plain(drop->second);
        g_plain.erase(drop);
    }
    g_plain_bytes += all.value().size();
    auto placed = g_plain.emplace(id, std::move(all.value()));
    return &placed.first->second;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string ok_json(const std::string& extra = "") {
    return "{\"ok\":true" + (extra.empty() ? std::string() : "," + extra) + "}";
}

std::string err_json(const VaultError& e) {
    return "{\"ok\":false,\"error\":\"" + json_escape(std::string(user_message(e.code))) +
           "\",\"detail\":\"" + json_escape(e.technical_detail) + "\"}";
}

std::string err_msg(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + json_escape(msg) + "\",\"detail\":\"\"}";
}

std::string jstring_to_string(JNIEnv* env, jstring jstr) {
    if (jstr == nullptr) return {};
    // Decode UTF-16 rather than using GetStringUTFChars: that returns modified
    // UTF-8 (CESU-8 for astral characters), which does not round-trip to the
    // UTF-8 paths the core expects for file names.
    const jsize length = env->GetStringLength(jstr);
    const jchar* chars = env->GetStringChars(jstr, nullptr);
    if (chars == nullptr) return {};
    std::string out;
    out.reserve(static_cast<std::size_t>(length) * 3U);
    for (jsize i = 0; i < length; ++i) {
        std::uint32_t code = chars[i];
        if (code >= 0xD800U && code <= 0xDBFFU && i + 1 < length
            && chars[i + 1] >= 0xDC00U && chars[i + 1] <= 0xDFFFU) {
            code = 0x10000U + ((code - 0xD800U) << 10U)
                + (static_cast<std::uint32_t>(chars[i + 1]) - 0xDC00U);
            ++i;
        }
        if (code < 0x80U) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else if (code < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
    }
    env->ReleaseStringChars(jstr, chars);
    return out;
}

jstring to_jstring(JNIEnv* env, const std::string& s) {
    // ART rejects NewStringUTF input that is not valid *modified* UTF-8 (it
    // aborts the process under CheckJNI): a 4-byte UTF-8 sequence such as an
    // emoji in a file name is enough. Decode real UTF-8 and hand over UTF-16.
    bool ascii = true;
    for (unsigned char c : s) {
        if (c >= 0x80U) {
            ascii = false;
            break;
        }
    }
    if (ascii) return env->NewStringUTF(s.c_str());

    std::u16string utf16;
    utf16.reserve(s.size());
    std::size_t i = 0U;
    while (i < s.size()) {
        const unsigned char b0 = static_cast<unsigned char>(s[i]);
        std::uint32_t code = 0U;
        std::size_t width = 1U;
        if (b0 < 0x80U) {
            code = b0;
        } else if ((b0 & 0xE0U) == 0xC0U) {
            code = b0 & 0x1FU;
            width = 2U;
        } else if ((b0 & 0xF0U) == 0xE0U) {
            code = b0 & 0x0FU;
            width = 3U;
        } else if ((b0 & 0xF8U) == 0xF0U) {
            code = b0 & 0x07U;
            width = 4U;
        } else {
            utf16.push_back(static_cast<char16_t>(0xFFFDU));
            ++i;
            continue;
        }
        bool valid = i + width <= s.size();
        if (valid) {
            for (std::size_t k = 1U; k < width; ++k) {
                const unsigned char b = static_cast<unsigned char>(s[i + k]);
                if ((b & 0xC0U) != 0x80U) {
                    valid = false;
                    break;
                }
                code = (code << 6U) | (b & 0x3FU);
            }
        }
        if (!valid) {
            utf16.push_back(static_cast<char16_t>(0xFFFDU));
            ++i;
            continue;
        }
        i += width;
        if (code <= 0xFFFFU) {
            utf16.push_back(static_cast<char16_t>(code));
        } else {
            code -= 0x10000U;
            utf16.push_back(static_cast<char16_t>(0xD800U + (code >> 10U)));
            utf16.push_back(static_cast<char16_t>(0xDC00U + (code & 0x3FFU)));
        }
    }
    return env->NewString(reinterpret_cast<const jchar*>(utf16.data()),
        static_cast<jsize>(utf16.size()));
}

// --- Range reader over Vault::read_video_range (public API) ----------------

struct RangeReader {
    Vault* vault{nullptr};
    std::int64_t video_id{0};
    std::uint64_t pos{0};
    // Plaintext size of the entry, resolved once at construction. FFmpeg's image
    // demuxers size the stream before probing (avio_size / SEEK_END) and refuse
    // to open without it, which is why stills came back as "could not decode"
    // even with the right filename hint.
    std::uint64_t size{0};

    RangeReader(Vault* v, const std::int64_t id, const std::uint64_t start)
        : vault(v), video_id(id), pos(start) {
        if (vault == nullptr) return;
        auto list = vault->list_videos();
        if (!list) return;
        for (const auto& video : list.value()) {
            if (video.id == video_id) {
                size = video.original_size;
                return;
            }
        }
    }

    std::optional<std::size_t> read(std::span<unsigned char> dst) {
        if (dst.empty()) return std::size_t{0};
        if (vault != nullptr) {
            if (const auto* cached = ensure_plain(*vault, video_id)) {
                if (pos >= cached->size()) return std::size_t{0};
                const auto n = std::min(
                    dst.size(), cached->size() - static_cast<std::size_t>(pos));
                std::memcpy(dst.data(), cached->data() + static_cast<std::size_t>(pos), n);
                pos += n;
                return n;
            }
        }
        auto got = vault->read_video_range(video_id, pos, dst.size());
        if (!got) return std::nullopt;
        auto& bytes = got.value();
        if (bytes.empty()) return std::size_t{0};
        std::memcpy(dst.data(), bytes.data(), bytes.size());
        pos += bytes.size();
        return bytes.size();
    }
    std::uint64_t position() const { return pos; }
    bool seek(std::uint64_t target) {
        pos = target;
        return true;
    }
};

int read_packet(void* opaque, unsigned char* buffer, const int size) {
    auto* reader = static_cast<RangeReader*>(opaque);
    auto got = reader->read(std::span(buffer, static_cast<std::size_t>(size)));
    if (!got) return AVERROR(EIO);
    if (got.value() == 0U) return AVERROR_EOF;
    return static_cast<int>(got.value());
}

std::int64_t seek_packet(void* opaque, const std::int64_t offset, const int whence) {
    auto* reader = static_cast<RangeReader*>(opaque);
    // Mirrors the core's reader (core/src/media.cpp): AVSEEK_SIZE and SEEK_END
    // must both work. Reporting "size unknown" made the image demuxers fail to
    // open, so every photo the viewer tried came back undecodable.
    if (whence == AVSEEK_SIZE) {
        return reader->size > 0U ? static_cast<std::int64_t>(reader->size) : -1;
    }
    std::int64_t target = 0;
    switch (whence) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = static_cast<std::int64_t>(reader->position()) + offset; break;
        case SEEK_END:
            if (reader->size == 0U) return -1;
            target = static_cast<std::int64_t>(reader->size) + offset;
            break;
        default: return -1;
    }
    if (target < 0) target = 0;
    reader->seek(static_cast<std::uint64_t>(target));
    return target;
}

// --- frame decode (mirrors core/src/media.cpp) ------------------------------

struct AvIoDeleter {
    void operator()(AVIOContext* c) const {
        if (c == nullptr) return;
        // avio_context_free() does not release the buffer handed to
        // avio_alloc_context(), so free it here or every decode leaks 64 KiB.
        if (c->buffer != nullptr) av_freep(&c->buffer);
        avio_context_free(&c);
    }
};
struct AvFormatDeleter { void operator()(AVFormatContext* c) const { avformat_close_input(&c); } };
struct AvCodecDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct AvFrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct AvPacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct SwsDeleter { void operator()(SwsContext* c) const { sws_freeContext(c); } };

constexpr std::size_t kAvioBufferSize = 64U * 1024U;
constexpr std::int64_t kMaxDecodePackets = 600;

// Opens the input, retrying with image filename hints so the extension-keyed
// demuxers (image2) can identify a bare still: the vault streams through a
// custom AVIO that has no filename, so a PNG/WebP/BMP would otherwise be
// undemuxable and the viewer would refuse a perfectly good picture.
//
// Ownership: avformat_open_input() frees the context and NULLs the pointer when
// it fails, so the unique_ptr is only constructed after a *successful* open.
// Wrapping it first is a double free — it kept the freed pointer and closed it
// again on scope exit, which is the SIGSEGV inside avformat_free_context that
// killed the app the moment an image was opened.
std::unique_ptr<AVFormatContext, AvFormatDeleter> open_format_with_hint(
    AVIOContext* io, RangeReader& reader, const char* name_hint) {
    (void)reader.seek(0);
    AVFormatContext* raw_format = avformat_alloc_context();
    if (raw_format == nullptr) return nullptr;
    raw_format->pb = io;
    raw_format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&raw_format, name_hint, nullptr, nullptr) < 0) {
        return nullptr; // FFmpeg released the context already
    }
    return std::unique_ptr<AVFormatContext, AvFormatDeleter>(raw_format);
}

std::optional<std::vector<unsigned char>> decode_frame_at(
    Vault& vault, std::int64_t video_id, std::int64_t position_ms, std::uint32_t max_dimension) {
    RangeReader reader{&vault, video_id, 0};

    auto* io_buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (io_buffer == nullptr) return std::nullopt;
    AVIOContext* raw_io = avio_alloc_context(
        io_buffer, static_cast<int>(kAvioBufferSize), 0, &reader,
        read_packet, nullptr, seek_packet);
    if (raw_io == nullptr) {
        av_free(io_buffer);
        return std::nullopt;
    }
    std::unique_ptr<AVIOContext, AvIoDeleter> io(raw_io);
    raw_io->seekable = AVIO_SEEKABLE_NORMAL;

    auto format = open_format_with_hint(io.get(), reader, nullptr);
    if (!format) {
        static const char* const kImageHints[] = {
            "image.png", "image.jpg", "image.webp", "image.gif",
            "image.bmp", "image.tiff",
        };
        for (const char* hint : kImageHints) {
            format = open_format_with_hint(io.get(), reader, hint);
            if (format) break;
        }
    }
    if (!format) return std::nullopt;

    AVFormatContext* raw_format = format.get();
    if (avformat_find_stream_info(raw_format, nullptr) < 0) return std::nullopt;

    int stream_index = -1;
    for (unsigned i = 0; i < raw_format->nb_streams; ++i) {
        if (raw_format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream_index = static_cast<int>(i);
            break;
        }
    }
    if (stream_index < 0) return std::nullopt;

    AVStream* stream = raw_format->streams[stream_index];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) return std::nullopt;
    std::unique_ptr<AVCodecContext, AvCodecDeleter> codec(avcodec_alloc_context3(decoder));
    if (codec == nullptr) return std::nullopt;
    if (avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0) return std::nullopt;
    if (avcodec_open2(codec.get(), decoder, nullptr) < 0) return std::nullopt;

    if (position_ms > 0) {
        std::int64_t target = av_rescale_q(position_ms * 1000LL, AV_TIME_BASE_Q, stream->time_base);
        (void)avformat_seek_file(raw_format, stream_index, INT64_MIN, target, target, 0);
        avcodec_flush_buffers(codec.get());
    }

    std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (!frame || !packet) return std::nullopt;

    bool decoded = false;
    std::int64_t packets = 0;
    while (packets++ < kMaxDecodePackets) {
        int status = av_read_frame(raw_format, packet.get());
        if (status < 0) break;
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet.get());
            continue;
        }
        if (avcodec_send_packet(codec.get(), packet.get()) == 0) {
            while (avcodec_receive_frame(codec.get(), frame.get()) == 0) {
                decoded = true;
                break;
            }
        }
        av_packet_unref(packet.get());
        if (decoded) break;
    }
    if (!decoded) return std::nullopt;

    // Scale to fit max_dimension, output RGBA.
    std::uint32_t w = static_cast<std::uint32_t>(frame->width);
    std::uint32_t h = static_cast<std::uint32_t>(frame->height);
    if (w == 0 || h == 0) return std::nullopt;
    if (max_dimension > 0 && (w > max_dimension || h > max_dimension)) {
        if (w >= h) {
            h = h * max_dimension / w;
            w = max_dimension;
        } else {
            w = w * max_dimension / h;
            h = max_dimension;
        }
    }

    std::unique_ptr<SwsContext, SwsDeleter> sws(sws_getContext(
        frame->width, frame->height,
        static_cast<AVPixelFormat>(frame->format),
        static_cast<int>(w), static_cast<int>(h), AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (sws == nullptr) return std::nullopt;

    std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * h * 4U);
    std::uint8_t* dst_planes[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int dst_linesizes[4] = {static_cast<int>(w * 4U), 0, 0, 0};
    sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, dst_planes, dst_linesizes);

    // Prepend width/height (little-endian) so the caller can rebuild the bitmap.
    std::vector<unsigned char> out;
    out.reserve(rgba.size() + 8);
    for (std::uint32_t v : {w, h}) {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
    }
    out.insert(out.end(), rgba.begin(), rgba.end());
    return out;
}

void append_u32(std::vector<unsigned char>& out, const std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

// One JPEG of a decoded frame. The encoder is reused; each picture is its own
// packet, so a repeat can draw frames without opening the codec again.
bool append_jpeg(
    AVCodecContext* encoder,
    SwsContext* scaler,
    AVFrame* yuv,
    const AVFrame* source,
    std::vector<unsigned char>& out) {
    sws_scale(scaler, source->data, source->linesize, 0, source->height, yuv->data, yuv->linesize);
    if (avcodec_send_frame(encoder, yuv) < 0) return false;
    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (!packet) return false;
    if (avcodec_receive_packet(encoder, packet.get()) < 0) return false;
    append_u32(out, static_cast<std::uint32_t>(packet->size));
    out.insert(out.end(), packet->data, packet->data + packet->size);
    return true;
}

// Clips of about a second or less. ExoPlayer repeats them by seeking back to
// the start, which flushes the decoder and (without the plaintext cache) decrypts
// the package again — longer than the clip. This walks the file once and returns
// the pictures plus PCM so the phone can loop them in memory.
// Empty means "not a short clip" (or it could not be decoded).
std::optional<std::vector<unsigned char>> decode_short_loop(
    Vault& vault, const std::int64_t video_id, const std::uint32_t max_dimension) {
    RangeReader reader{&vault, video_id, 0};
    auto* io_buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (io_buffer == nullptr) return std::nullopt;
    AVIOContext* raw_io = avio_alloc_context(
        io_buffer, static_cast<int>(kAvioBufferSize), 0, &reader,
        read_packet, nullptr, seek_packet);
    if (raw_io == nullptr) {
        av_free(io_buffer);
        return std::nullopt;
    }
    std::unique_ptr<AVIOContext, AvIoDeleter> io(raw_io);
    raw_io->seekable = AVIO_SEEKABLE_NORMAL;
    auto format = open_format_with_hint(io.get(), reader, nullptr);
    if (!format) return std::nullopt;
    AVFormatContext* raw_format = format.get();
    if (avformat_find_stream_info(raw_format, nullptr) < 0) return std::nullopt;

    int video_index = -1;
    int audio_index = -1;
    for (unsigned i = 0; i < raw_format->nb_streams; ++i) {
        const auto type = raw_format->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && video_index < 0) video_index = static_cast<int>(i);
        if (type == AVMEDIA_TYPE_AUDIO && audio_index < 0) audio_index = static_cast<int>(i);
    }
    if (video_index < 0) return std::nullopt;

    auto duration_us = raw_format->duration;
    AVStream* video_stream = raw_format->streams[video_index];
    if ((duration_us <= 0 || duration_us == AV_NOPTS_VALUE)
        && video_stream->duration > 0 && video_stream->duration != AV_NOPTS_VALUE) {
        duration_us = av_rescale_q(video_stream->duration, video_stream->time_base, AV_TIME_BASE_Q);
    }
    // Longer clips stay on ExoPlayer. Only the ones that would restart the
    // decoder more often than they play are unpacked here.
    constexpr std::int64_t kShortLimitUs = 1500LL * 1000LL;
    if (duration_us > 0 && duration_us != AV_NOPTS_VALUE && duration_us > kShortLimitUs) {
        return std::nullopt;
    }

    const AVCodec* video_decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (video_decoder == nullptr) return std::nullopt;
    std::unique_ptr<AVCodecContext, AvCodecDeleter> video_codec(avcodec_alloc_context3(video_decoder));
    if (!video_codec) return std::nullopt;
    if (avcodec_parameters_to_context(video_codec.get(), video_stream->codecpar) < 0) return std::nullopt;
    if (avcodec_open2(video_codec.get(), video_decoder, nullptr) < 0) return std::nullopt;

    std::unique_ptr<AVCodecContext, AvCodecDeleter> audio_codec;
    std::unique_ptr<SwrContext, void(*)(SwrContext*)> swr(nullptr, [](SwrContext* c) { swr_free(&c); });
    int sample_rate = 0;
    int channels = 0;
    if (audio_index >= 0) {
        AVStream* audio_stream = raw_format->streams[audio_index];
        const AVCodec* audio_decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (audio_decoder != nullptr) {
            audio_codec.reset(avcodec_alloc_context3(audio_decoder));
            if (audio_codec
                && avcodec_parameters_to_context(audio_codec.get(), audio_stream->codecpar) >= 0
                && avcodec_open2(audio_codec.get(), audio_decoder, nullptr) >= 0) {
                sample_rate = 44100;
                channels = 2;
                AVChannelLayout out_layout;
                av_channel_layout_default(&out_layout, channels);
                SwrContext* raw_swr = nullptr;
                if (swr_alloc_set_opts2(
                        &raw_swr, &out_layout, AV_SAMPLE_FMT_S16, sample_rate,
                        &audio_codec->ch_layout, audio_codec->sample_fmt, audio_codec->sample_rate,
                        0, nullptr) < 0
                    || raw_swr == nullptr || swr_init(raw_swr) < 0) {
                    swr_free(&raw_swr);
                    audio_codec.reset();
                    sample_rate = 0;
                    channels = 0;
                } else {
                    swr.reset(raw_swr);
                }
                av_channel_layout_uninit(&out_layout);
            }
        }
    }

    std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
    if (!frame || !packet) return std::nullopt;

    std::vector<std::pair<std::int64_t, std::vector<unsigned char>>> pictures;
    std::vector<unsigned char> pcm;
    constexpr int kMaxFrames = 48;
    bool truncated = false;
    std::int64_t origin = AV_NOPTS_VALUE;

    std::unique_ptr<AVCodecContext, AvCodecDeleter> jpeg;
    std::unique_ptr<SwsContext, SwsDeleter> scaler;
    std::unique_ptr<AVFrame, AvFrameDeleter> yuv;
    int jpeg_w = 0;
    int jpeg_h = 0;

    auto take_video = [&](AVFrame* picture) -> bool {
        std::int64_t stamp = picture->best_effort_timestamp;
        if (stamp == AV_NOPTS_VALUE) stamp = picture->pts;
        std::int64_t us = 0;
        if (stamp != AV_NOPTS_VALUE) {
            if (origin == AV_NOPTS_VALUE) origin = stamp;
            us = av_rescale_q(stamp - origin, video_stream->time_base, AV_TIME_BASE_Q);
            if (us < 0) us = 0;
        }
        if (us > kShortLimitUs || static_cast<int>(pictures.size()) >= kMaxFrames) {
            truncated = true;
            return false;
        }
        int w = picture->width;
        int h = picture->height;
        if (w < 2 || h < 2) return true;
        if (max_dimension > 0 && (static_cast<std::uint32_t>(w) > max_dimension
                || static_cast<std::uint32_t>(h) > max_dimension)) {
            if (w >= h) {
                h = std::max(2, h * static_cast<int>(max_dimension) / w);
                w = static_cast<int>(max_dimension);
            } else {
                w = std::max(2, w * static_cast<int>(max_dimension) / h);
                h = static_cast<int>(max_dimension);
            }
        }
        w &= ~1;
        h &= ~1;
        if (!jpeg || w != jpeg_w || h != jpeg_h) {
            const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
            if (encoder == nullptr) return false;
            jpeg.reset(avcodec_alloc_context3(encoder));
            if (!jpeg) return false;
            jpeg->width = w;
            jpeg->height = h;
            jpeg->time_base = AVRational{1, 25};
            jpeg->pix_fmt = AV_PIX_FMT_YUV420P;
            jpeg->color_range = AVCOL_RANGE_JPEG;
            if (avcodec_open2(jpeg.get(), encoder, nullptr) < 0) return false;
            scaler.reset(sws_getContext(
                picture->width, picture->height, static_cast<AVPixelFormat>(picture->format),
                w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!scaler) return false;
            yuv.reset(av_frame_alloc());
            if (!yuv) return false;
            yuv->format = AV_PIX_FMT_YUV420P;
            yuv->width = w;
            yuv->height = h;
            if (av_frame_get_buffer(yuv.get(), 0) < 0) return false;
            jpeg_w = w;
            jpeg_h = h;
        }
        if (av_frame_make_writable(yuv.get()) < 0) return false;
        std::vector<unsigned char> body;
        if (!append_jpeg(jpeg.get(), scaler.get(), yuv.get(), picture, body)) return false;
        pictures.emplace_back(us, std::move(body));
        return true;
    };

    auto take_audio = [&](AVFrame* audio) {
        if (!swr || sample_rate <= 0) return;
        constexpr std::size_t kMaxPcm = 44100U * 2U * 2U * 2U;
        if (pcm.size() >= kMaxPcm) return;
        const int out_samples = swr_get_out_samples(swr.get(), audio->nb_samples);
        if (out_samples <= 0) return;
        std::vector<unsigned char> chunk(static_cast<std::size_t>(out_samples) * static_cast<std::size_t>(channels) * 2U);
        uint8_t* dest[1] = {chunk.data()};
        const int got = swr_convert(swr.get(), dest, out_samples, audio->extended_data, audio->nb_samples);
        if (got <= 0) return;
        const auto bytes = static_cast<std::size_t>(got) * static_cast<std::size_t>(channels) * 2U;
        const auto room = kMaxPcm - pcm.size();
        pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(std::min(bytes, room)));
    };

    std::int64_t packets = 0;
    while (!truncated && packets++ < kMaxDecodePackets) {
        const int status = av_read_frame(raw_format, packet.get());
        if (status < 0) break;
        const int index = packet->stream_index;
        if (index == video_index) {
            if (avcodec_send_packet(video_codec.get(), packet.get()) == 0) {
                while (avcodec_receive_frame(video_codec.get(), frame.get()) == 0) {
                    if (!take_video(frame.get())) break;
                }
            }
        } else if (audio_codec && index == audio_index) {
            if (avcodec_send_packet(audio_codec.get(), packet.get()) == 0) {
                while (avcodec_receive_frame(audio_codec.get(), frame.get()) == 0) {
                    take_audio(frame.get());
                }
            }
        }
        av_packet_unref(packet.get());
    }
    if (!truncated) {
        avcodec_send_packet(video_codec.get(), nullptr);
        while (avcodec_receive_frame(video_codec.get(), frame.get()) == 0) {
            if (!take_video(frame.get())) break;
        }
        if (audio_codec) {
            avcodec_send_packet(audio_codec.get(), nullptr);
            while (avcodec_receive_frame(audio_codec.get(), frame.get()) == 0) {
                take_audio(frame.get());
            }
        }
    }
    if (pictures.empty() || truncated) return std::nullopt;

    std::vector<unsigned char> out;
    out.reserve(64U * 1024U);
    append_u32(out, 0x504F4F4CU);
    append_u32(out, static_cast<std::uint32_t>(pictures.size()));
    for (const auto& picture : pictures) {
        append_u32(out, static_cast<std::uint32_t>(std::max<std::int64_t>(picture.first, 0)));
        out.insert(out.end(), picture.second.begin(), picture.second.end());
    }
    if (pcm.empty()) {
        sample_rate = 0;
        channels = 0;
    }
    append_u32(out, static_cast<std::uint32_t>(sample_rate));
    append_u32(out, static_cast<std::uint32_t>(channels));
    append_u32(out, static_cast<std::uint32_t>(pcm.size()));
    out.insert(out.end(), pcm.begin(), pcm.end());
    return out;
}

std::string videos_json() {
    if (!g_vault) return "[]";
    auto list = g_vault->list_videos();
    if (!list) return "[]";
    std::string out = "[";
    bool first = true;
    for (const auto& v : list.value()) {
        // Container metadata is deliberately NOT probed here. probe_media opens
        // the package and decrypts container bytes, so listing a vault with N
        // videos cost N FFmpeg probes and took seconds to appear on the phone.
        // Rows carry only what the database knows; the gallery fills duration /
        // resolution / codec per *visible* row via nativeMediaInfo, exactly as
        // the desktop fills its Details columns asynchronously.
        std::string tags;
        auto t = g_vault->tags_for_video(v.id);
        if (t) {
            bool tf = true;
            for (const auto& tag : t.value()) {
                if (!tf) tags += ",";
                tags += "\"" + json_escape(tag.name) + "\"";
                tf = false;
            }
        }
        if (!first) out += ",";
        first = false;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "{\"id\":%lld,\"name\":\"%s\",\"size\":%llu,"
            "\"importedAt\":%llu,\"tags\":[%s]}",
            static_cast<long long>(v.id), json_escape(v.display_name).c_str(),
            static_cast<unsigned long long>(v.original_size),
            static_cast<unsigned long long>(v.imported_at),
            tags.c_str());
        out += buf;
    }
    out += "]";
    return out;
}

std::string media_info_json(std::int64_t id) {
    if (!g_vault) return "{}";
    auto info = g_vault->media_info(id);
    if (!info) return "{}";
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"durationMs\":%llu,\"width\":%u,\"height\":%u,\"rotation\":%u,\"codec\":\"%s\"}",
        static_cast<unsigned long long>(info.value().duration_ms),
        info.value().width, info.value().height, info.value().rotation_degrees,
        json_escape(info.value().codec_name).c_str());
    return buf;
}

std::string tags_json() {
    if (!g_vault) return "[]";
    auto list = g_vault->list_tags();
    if (!list) return "[]";
    std::string out = "[";
    bool first = true;
    for (const auto& tag : list.value()) {
        if (!first) out += ",";
        first = false;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{\"id\":%lld,\"name\":\"%s\",\"count\":%lld}",
            static_cast<long long>(tag.id), json_escape(tag.name).c_str(),
            static_cast<long long>(tag.video_count));
        out += buf;
    }
    out += "]";
    return out;
}

}  // namespace

// --- JNI exports -------------------------------------------------------------

#define JNI_METHOD(name) \
    JNIEXPORT jstring JNICALL Java_org_megavideoprotect_app_CoreBridge_##name

extern "C" {

JNIEXPORT jboolean JNICALL Java_org_megavideoprotect_app_CoreBridge_nativeVaultExists(
    JNIEnv* env, jobject, jstring root) {
    const std::string r = jstring_to_string(env, root);
    return Vault::exists(r) ? JNI_TRUE : JNI_FALSE;
}

JNI_METHOD(nativeCreateVault)(JNIEnv* env, jobject, jstring root, jstring password,
                              jint mem_kib, jint iterations, jint parallelism) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string r = jstring_to_string(env, root);
    const std::string p = jstring_to_string(env, password);
    Argon2Parameters params{
        static_cast<std::uint32_t>(mem_kib),
        static_cast<std::uint32_t>(iterations),
        static_cast<std::uint32_t>(parallelism)};
    auto result = Vault::create(r, p, params);
    if (!result) return to_jstring(env, err_json(result.error()));
    g_vault = std::make_unique<Vault>(std::move(result.value()));
    return to_jstring(env, ok_json());
}

JNI_METHOD(nativeOpenVault)(JNIEnv* env, jobject, jstring root, jstring password) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string r = jstring_to_string(env, root);
    const std::string p = jstring_to_string(env, password);
    auto result = Vault::open(r, p);
    if (!result) return to_jstring(env, err_json(result.error()));
    g_vault = std::make_unique<Vault>(std::move(result.value()));
    return to_jstring(env, ok_json());
}

JNIEXPORT jboolean JNICALL Java_org_megavideoprotect_app_CoreBridge_nativeIsUnlocked(
    JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return (g_vault && g_vault->is_unlocked()) ? JNI_TRUE : JNI_FALSE;
}

JNI_METHOD(nativeLock)(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_plain();
    if (g_vault) g_vault->lock();
    return to_jstring(env, ok_json());
}

JNI_METHOD(nativeListVideos)(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return to_jstring(env, videos_json());
}

JNI_METHOD(nativeImportFile)(JNIEnv* env, jobject, jstring path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return to_jstring(env, err_msg("Vault is not open"));
    const std::string p = jstring_to_string(env, path);
    auto result = g_vault->import_file(p);
    if (!result) return to_jstring(env, err_json(result.error()));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"id\":%lld", static_cast<long long>(result.value()));
    return to_jstring(env, ok_json(buf));
}

JNI_METHOD(nativeRemoveVideo)(JNIEnv* env, jobject, jlong id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return to_jstring(env, err_msg("Vault is not open"));
    auto result = g_vault->remove_video(id);
    if (!result) return to_jstring(env, err_json(result.error()));
    forget_plain(static_cast<std::int64_t>(id));
    return to_jstring(env, ok_json());
}

JNIEXPORT jbyteArray JNICALL Java_org_megavideoprotect_app_CoreBridge_nativeThumbnail(
    JNIEnv* env, jobject, jlong id, jint max_dimension) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return nullptr;
    auto thumb = g_vault->thumbnail(id);
    if (!thumb) {
        auto generated = g_vault->generate_thumbnail(id, static_cast<std::uint32_t>(max_dimension));
        if (!generated) return nullptr;
        thumb = std::move(generated);
    }
    auto& bytes = thumb.value().bytes;
    jbyteArray out = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (out != nullptr) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes.size()),
            reinterpret_cast<const jbyte*>(bytes.data()));
    }
    return out;
}

JNI_METHOD(nativeMediaInfo)(JNIEnv* env, jobject, jlong id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return to_jstring(env, media_info_json(id));
}

JNI_METHOD(nativeRestoreVideo)(JNIEnv* env, jobject, jlong id, jstring dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return to_jstring(env, err_msg("Vault is not open"));
    const std::string d = jstring_to_string(env, dir);
    auto result = g_vault->restore_video(id, d);
    if (!result) return to_jstring(env, err_json(result.error()));
    return to_jstring(env, ok_json("\"path\":\"" + json_escape(result.value().string()) + "\""));
}

JNI_METHOD(nativeChangePassword)(JNIEnv* env, jobject, jstring current, jstring new_password,
                                 jint mem_kib, jint iterations, jint parallelism) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return to_jstring(env, err_msg("Vault is not open"));
    const std::string cur = jstring_to_string(env, current);
    const std::string nw = jstring_to_string(env, new_password);
    Argon2Parameters params{
        static_cast<std::uint32_t>(mem_kib),
        static_cast<std::uint32_t>(iterations),
        static_cast<std::uint32_t>(parallelism)};
    auto result = g_vault->change_password(cur, nw, params);
    if (!result) return to_jstring(env, err_json(result.error()));
    return to_jstring(env, ok_json());
}

JNI_METHOD(nativeListTags)(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return to_jstring(env, tags_json());
}

JNI_METHOD(nativeAddTag)(JNIEnv* env, jobject, jlong video_id, jstring name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return to_jstring(env, err_msg("Vault is not open"));
    const std::string n = jstring_to_string(env, name);
    auto result = g_vault->add_tag(video_id, n);
    if (!result) return to_jstring(env, err_json(result.error()));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"id\":%lld", static_cast<long long>(result.value()));
    return to_jstring(env, ok_json(buf));
}

JNI_METHOD(nativeRemoveTag)(JNIEnv* env, jobject, jlong video_id, jlong tag_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return to_jstring(env, err_msg("Vault is not open"));
    auto result = g_vault->remove_tag(video_id, tag_id);
    if (!result) return to_jstring(env, err_json(result.error()));
    return to_jstring(env, ok_json());
}

JNI_METHOD(nativeDeleteTag)(JNIEnv* env, jobject, jlong tag_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return to_jstring(env, err_msg("Vault is not open"));
    auto result = g_vault->delete_tag(tag_id);
    if (!result) return to_jstring(env, err_json(result.error()));
    return to_jstring(env, ok_json());
}

JNIEXPORT jbyteArray JNICALL Java_org_megavideoprotect_app_CoreBridge_nativeDecodeFrame(
    JNIEnv* env, jobject, jlong id, jlong position_ms, jint max_dimension) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return nullptr;
    auto rgba = decode_frame_at(*g_vault, id, position_ms,
        static_cast<std::uint32_t>(max_dimension));
    if (!rgba) return nullptr;
    jbyteArray out = env->NewByteArray(static_cast<jsize>(rgba->size()));
    if (out != nullptr) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(rgba->size()),
            reinterpret_cast<const jbyte*>(rgba->data()));
    }
    return out;
}

JNI_METHOD(nativeCreateTag)(JNIEnv* env, jobject, jstring name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return to_jstring(env, err_msg("Vault is not open"));
    const std::string n = jstring_to_string(env, name);
    // create_tag (not add_tag(-1, ...)): a tag with no video attached.
    auto result = g_vault->create_tag(n);
    if (!result) return to_jstring(env, err_json(result.error()));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"id\":%lld", static_cast<long long>(result.value()));
    return to_jstring(env, ok_json(buf));
}

JNIEXPORT jlong JNICALL Java_org_megavideoprotect_app_CoreBridge_nativeVideoSize(
    JNIEnv*, jobject, jlong id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault) return -1;
    auto size = lookup_size(*g_vault, static_cast<std::int64_t>(id));
    if (!size) return -1;
    return static_cast<jlong>(size.value());
}

// Streaming read for the player (Media3 DataSource): returns up to `size`
// plaintext bytes at `offset`, authenticated per chunk by the core. An empty
// array means end of stream; null means the read failed.
JNIEXPORT jbyteArray JNICALL Java_org_megavideoprotect_app_CoreBridge_nativeReadRange(
    JNIEnv* env, jobject, jlong id, jlong offset, jint size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault || size <= 0 || offset < 0) return nullptr;
    const auto video = static_cast<std::int64_t>(id);
    const auto at = static_cast<std::uint64_t>(offset);
    if (const auto* cached = ensure_plain(*g_vault, video)) {
        if (at >= cached->size()) {
            return env->NewByteArray(0);
        }
        const auto n = std::min(
            static_cast<std::size_t>(size), cached->size() - static_cast<std::size_t>(at));
        jbyteArray out = env->NewByteArray(static_cast<jsize>(n));
        if (out != nullptr && n > 0U) {
            env->SetByteArrayRegion(out, 0, static_cast<jsize>(n),
                reinterpret_cast<const jbyte*>(cached->data() + static_cast<std::size_t>(at)));
        }
        return out;
    }
    auto bytes = g_vault->read_video_range(video, at, static_cast<std::size_t>(size));
    if (!bytes) return nullptr;
    auto& data = bytes.value();
    jbyteArray out = env->NewByteArray(static_cast<jsize>(data.size()));
    if (out != nullptr && !data.empty()) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(data.size()),
            reinterpret_cast<const jbyte*>(data.data()));
    }
    return out;
}

JNIEXPORT jbyteArray JNICALL Java_org_megavideoprotect_app_CoreBridge_nativeLoopClip(
    JNIEnv* env, jobject, jlong id, jint max_dimension) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_vault || max_dimension <= 0) return nullptr;
    auto bytes = decode_short_loop(*g_vault, static_cast<std::int64_t>(id),
        static_cast<std::uint32_t>(max_dimension));
    if (!bytes || bytes->empty()) return nullptr;
    jbyteArray out = env->NewByteArray(static_cast<jsize>(bytes->size()));
    if (out != nullptr) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes->size()),
            reinterpret_cast<const jbyte*>(bytes->data()));
    }
    return out;
}

JNI_METHOD(nativeClose)(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_plain();
    g_vault.reset();
    return to_jstring(env, ok_json());
}

}  // extern "C"
