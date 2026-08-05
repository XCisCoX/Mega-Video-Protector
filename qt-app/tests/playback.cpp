// Headless playback test: exercises MediaDecoder (the same engine the player
// window uses) against an imported encrypted video — frame decoding, audio
// samples, and seeking — with no GUI involved.
#include "videovault/app/media_decoder.hpp"
#include "videovault/app/player_window.hpp"
#include "videovault/core/vault.hpp"

#include <QApplication>
#include <QLibraryInfo>
#include <QSlider>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVideoProtect_Playback_" + suffix);
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return EXIT_FAILURE;
}

videovault::core::Argon2Parameters testParameters() {
    videovault::core::Argon2Parameters parameters;
    parameters.memory_kib = 8U * 1024U;
    parameters.iterations = 2U;
    parameters.parallelism = 1U;
    return parameters;
}

// Writes a Matroska fixture with an MJPEG video stream (64x64, 10 fps,
// 20 frames) and a PCM s16le audio stream (48 kHz stereo, 0.1 s per frame).
bool writeFixture(const std::filesystem::path& path) {
    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    constexpr int kFps = 10;
    constexpr int kFrames = 20;
    constexpr int kAudioRate = 48000;
    constexpr int kAudioChannels = 2;
    constexpr int kSamplesPerFrame = kAudioRate / kFps; // 4800

    // Video source frames (RGB24 gradient).
    const int frame_size = kWidth * kHeight * 3;
    std::vector<unsigned char> rgb(
        static_cast<std::size_t>(frame_size) * kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const auto offset = static_cast<std::size_t>(
                    frame * frame_size + (y * kWidth + x) * 3);
                rgb[offset + 0] = static_cast<unsigned char>((x * 255 / kWidth + frame * 3) & 0xff);
                rgb[offset + 1] = static_cast<unsigned char>((y * 255 / kHeight) & 0xff);
                rgb[offset + 2] = static_cast<unsigned char>(((x + y) * 255 / (kWidth + kHeight) + frame * 5) & 0xff);
            }
        }
    }

    // 440 Hz tone, s16le stereo.
    std::vector<std::int16_t> tone(
        static_cast<std::size_t>(kSamplesPerFrame) * kAudioChannels * kFrames);
    for (std::size_t i = 0; i < tone.size() / kAudioChannels; ++i) {
        const auto value = static_cast<std::int16_t>(
            8000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / kAudioRate));
        tone[i * kAudioChannels + 0] = value;
        tone[i * kAudioChannels + 1] = value;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* codec = avcodec_alloc_context3(encoder);
    codec->width = kWidth;
    codec->height = kHeight;
    codec->time_base = AVRational{1, kFps};
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->color_range = AVCOL_RANGE_JPEG;
    if (avcodec_open2(codec, encoder, nullptr) < 0) {
        return false;
    }

    SwsContext* scaler = sws_getContext(
        kWidth, kHeight, AV_PIX_FMT_RGB24,
        kWidth, kHeight, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
        nullptr, nullptr, nullptr);
    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = kWidth;
    yuv->height = kHeight;
    av_frame_get_buffer(yuv, 0);
    const uint8_t* src_slices[4] = {rgb.data(), nullptr, nullptr, nullptr};
    const int src_strides[4] = {kWidth * 3, 0, 0, 0};

    AVFormatContext* format = nullptr;
    if (avformat_alloc_output_context2(&format, nullptr, "matroska", nullptr) < 0
        || format == nullptr) {
        return false;
    }

    AVStream* video_stream = avformat_new_stream(format, nullptr);
    video_stream->id = 0;
    video_stream->time_base = AVRational{1, kFps};
    AVCodecParameters* vpar = video_stream->codecpar;
    vpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vpar->codec_id = AV_CODEC_ID_MJPEG;
    vpar->format = AV_PIX_FMT_YUV420P;
    vpar->width = kWidth;
    vpar->height = kHeight;

    AVStream* audio_stream = avformat_new_stream(format, nullptr);
    audio_stream->id = 1;
    audio_stream->time_base = AVRational{1, kAudioRate};
    AVCodecParameters* apar = audio_stream->codecpar;
    apar->codec_type = AVMEDIA_TYPE_AUDIO;
    apar->codec_id = AV_CODEC_ID_PCM_S16LE;
    apar->format = AV_SAMPLE_FMT_S16;
    apar->sample_rate = kAudioRate;
    av_channel_layout_default(&apar->ch_layout, kAudioChannels);
    apar->bits_per_coded_sample = 16;
    apar->block_align = kAudioChannels * 2;
    apar->bit_rate = kAudioRate * kAudioChannels * 16;

    if (avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
        return false;
    }
    if (avformat_write_header(format, nullptr) < 0) {
        return false;
    }

    const AVRational video_tb = video_stream->time_base; // after header
    const AVRational audio_tb = audio_stream->time_base;
    const AVRational source_tb = AVRational{1, kFps};

    AVPacket* packet = av_packet_alloc();
    bool success = true;
    for (int frame = 0; frame < kFrames; ++frame) {
        sws_scale(scaler, src_slices, src_strides, 0, kHeight, yuv->data, yuv->linesize);
        yuv->pts = av_rescale_q(frame, source_tb, video_tb);
        if (avcodec_send_frame(codec, yuv) < 0
            || avcodec_receive_packet(codec, packet) < 0) {
            success = false;
            break;
        }
        packet->stream_index = video_stream->index;
        packet->pts = av_rescale_q(frame, source_tb, video_tb);
        packet->dts = packet->pts;
        if (av_write_frame(format, packet) < 0) {
            success = false;
            break;
        }
        av_packet_unref(packet);

        const auto audio_offset = static_cast<std::size_t>(frame) * kSamplesPerFrame * kAudioChannels;
        packet->data = reinterpret_cast<std::uint8_t*>(
            const_cast<std::int16_t*>(tone.data() + audio_offset));
        packet->size = kSamplesPerFrame * kAudioChannels * 2;
        packet->stream_index = audio_stream->index;
        packet->pts = av_rescale_q(
            static_cast<std::int64_t>(frame) * kSamplesPerFrame,
            AVRational{1, kAudioRate}, audio_tb);
        packet->dts = packet->pts;
        if (av_write_frame(format, packet) < 0) {
            success = false;
            break;
        }
        av_packet_unref(packet);
        src_slices[0] += frame_size;
    }
    av_write_trailer(format);
    avio_close(format->pb);
    avformat_free_context(format);
    avcodec_free_context(&codec);
    sws_freeContext(scaler);
    av_frame_free(&yuv);
    av_packet_free(&packet);
    return success;
}

// MP4 fixture with an INTERFRAME codec (MPEG-4) — a keyframe every kGop
// frames — plus AAC audio: the shape of real user videos. The Matroska/MJPEG
// fixture above is all-keyframes, which makes seeking trivially correct;
// this one exercises the sparse-keyframe seek path (the reported bug).
bool writeMp4Fixture(const std::filesystem::path& path) {
    constexpr int kWidth = 160;
    constexpr int kHeight = 120;
    constexpr int kFps = 15;
    constexpr int kFrames = 45; // 3.0 s
    constexpr int kGop = 15;    // keyframe every 1.0 s
    constexpr int kAudioRate = 48000;
    constexpr int kAudioChannels = 2;
    constexpr int kAacSamples = 1024;

    const int frame_size = kWidth * kHeight * 3;
    std::vector<unsigned char> rgb(
        static_cast<std::size_t>(frame_size) * kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const auto offset = static_cast<std::size_t>(
                    frame * frame_size + (y * kWidth + x) * 3);
                rgb[offset + 0] = static_cast<unsigned char>((x * 255 / kWidth + frame * 7) & 0xff);
                rgb[offset + 1] = static_cast<unsigned char>((y * 255 / kHeight) & 0xff);
                rgb[offset + 2] = static_cast<unsigned char>(((x + y) * 255 / (kWidth + kHeight) + frame * 11) & 0xff);
            }
        }
    }

    const AVCodec* venc = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (venc == nullptr) {
        return false;
    }
    AVCodecContext* vcodec = avcodec_alloc_context3(venc);
    vcodec->width = kWidth;
    vcodec->height = kHeight;
    vcodec->time_base = AVRational{1, kFps};
    vcodec->pix_fmt = AV_PIX_FMT_YUV420P;
    vcodec->gop_size = kGop;
    vcodec->max_b_frames = 0;
    if (avcodec_open2(vcodec, venc, nullptr) < 0) {
        avcodec_free_context(&vcodec);
        return false;
    }

    const AVCodec* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (aenc == nullptr) {
        avcodec_free_context(&vcodec);
        return false;
    }
    AVCodecContext* acodec = avcodec_alloc_context3(aenc);
    acodec->sample_rate = kAudioRate;
    acodec->bit_rate = 128000;
    av_channel_layout_default(&acodec->ch_layout, kAudioChannels);
    acodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (avcodec_open2(acodec, aenc, nullptr) < 0) {
        avcodec_free_context(&vcodec);
        avcodec_free_context(&acodec);
        return false;
    }

    SwsContext* scaler = sws_getContext(
        kWidth, kHeight, AV_PIX_FMT_RGB24,
        kWidth, kHeight, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
        nullptr, nullptr, nullptr);
    AVFrame* yuv = av_frame_alloc();
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = kWidth;
    yuv->height = kHeight;
    av_frame_get_buffer(yuv, 0);
    const uint8_t* src_slices[4] = {rgb.data(), nullptr, nullptr, nullptr};
    const int src_strides[4] = {kWidth * 3, 0, 0, 0};

    AVFrame* aframe = av_frame_alloc();
    aframe->format = AV_SAMPLE_FMT_FLTP;
    aframe->sample_rate = kAudioRate;
    av_channel_layout_copy(&aframe->ch_layout, &acodec->ch_layout);
    aframe->nb_samples = kAacSamples;
    av_frame_get_buffer(aframe, 0);

    AVFormatContext* format = nullptr;
    if (avformat_alloc_output_context2(&format, nullptr, "mp4", nullptr) < 0
        || format == nullptr) {
        return false;
    }
    AVStream* video_stream = avformat_new_stream(format, nullptr);
    video_stream->time_base = AVRational{1, kFps};
    if (avcodec_parameters_from_context(video_stream->codecpar, vcodec) < 0) {
        return false;
    }
    AVStream* audio_stream = avformat_new_stream(format, nullptr);
    audio_stream->time_base = AVRational{1, kAudioRate};
    if (avcodec_parameters_from_context(audio_stream->codecpar, acodec) < 0) {
        return false;
    }

    if (avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
        return false;
    }
    if (avformat_write_header(format, nullptr) < 0) {
        return false;
    }

    const AVRational video_tb = video_stream->time_base; // after header
    const AVRational audio_tb = audio_stream->time_base;
    const AVRational source_tb = AVRational{1, kFps};

    AVPacket* packet = av_packet_alloc();
    bool success = true;

    auto encode_audio_chunk = [&](const std::int64_t start_sample) {
        for (int ch = 0; ch < kAudioChannels; ++ch) {
            auto* samples = reinterpret_cast<float*>(aframe->data[ch]);
            for (int i = 0; i < kAacSamples; ++i) {
                const auto t = static_cast<double>(start_sample + i);
                samples[i] = static_cast<float>(
                    0.6 * std::sin(2.0 * 3.141592653589793 * 440.0 * t / kAudioRate));
            }
        }
        aframe->pts = start_sample;
        if (avcodec_send_frame(acodec, aframe) < 0) {
            return false;
        }
        while (avcodec_receive_packet(acodec, packet) == 0) {
            packet->stream_index = audio_stream->index;
            packet->pts = av_rescale_q(packet->pts, AVRational{1, kAudioRate}, audio_tb);
            packet->dts = packet->pts;
            if (av_write_frame(format, packet) < 0) {
                return false;
            }
            av_packet_unref(packet);
        }
        return true;
    };

    for (int frame = 0; frame < kFrames; ++frame) {
        sws_scale(scaler, src_slices, src_strides, 0, kHeight, yuv->data, yuv->linesize);
        yuv->pts = av_rescale_q(frame, source_tb, video_tb);
        if (avcodec_send_frame(vcodec, yuv) < 0
            || avcodec_receive_packet(vcodec, packet) < 0) {
            success = false;
            break;
        }
        packet->stream_index = video_stream->index;
        packet->pts = av_rescale_q(frame, source_tb, video_tb);
        packet->dts = packet->pts;
        if (av_write_frame(format, packet) < 0) {
            success = false;
            break;
        }
        av_packet_unref(packet);

        const auto start_sample = static_cast<std::int64_t>(frame) * kAacSamples;
        if (!encode_audio_chunk(start_sample)) {
            success = false;
            break;
        }
        src_slices[0] += frame_size;
    }

    // Flush both encoders.
    avcodec_send_frame(vcodec, nullptr);
    while (avcodec_receive_packet(vcodec, packet) == 0) {
        packet->stream_index = video_stream->index;
        packet->pts = av_rescale_q(packet->pts, source_tb, video_tb);
        packet->dts = packet->pts;
        if (av_write_frame(format, packet) < 0) {
            success = false;
        }
        av_packet_unref(packet);
    }
    avcodec_send_frame(acodec, nullptr);
    while (avcodec_receive_packet(acodec, packet) == 0) {
        packet->stream_index = audio_stream->index;
        packet->pts = av_rescale_q(packet->pts, AVRational{1, kAudioRate}, audio_tb);
        packet->dts = packet->pts;
        if (av_write_frame(format, packet) < 0) {
            success = false;
        }
        av_packet_unref(packet);
    }

    av_write_trailer(format);
    avio_close(format->pb);
    avformat_free_context(format);
    avcodec_free_context(&vcodec);
    avcodec_free_context(&acodec);
    sws_freeContext(scaler);
    av_frame_free(&yuv);
    av_frame_free(&aframe);
    av_packet_free(&packet);
    return success;
}

// Strict post-seek verification: the first decoded frame must be at/near the
// requested target (not a silent restart at 0, not the stale pre-seek
// position), pts must advance forward, and decoding must reach the target.
int checkSeek(videovault::app::MediaDecoder& decoder, const char* label,
              const std::int64_t target_ms, const std::int64_t max_gap_ms) {
    if (decoder.seek_to(target_ms) < 0) {
        std::fprintf(stderr, "%s: seek_to(%lld) returned false\n",
            label, static_cast<long long>(target_ms));
        return 1;
    }
    std::int64_t first_pts = -1;
    std::int64_t prev_pts = -1;
    std::int64_t last_pts = -1;
    bool reached_target = false;
    int frames = 0;
    for (int guard = 0; guard < 300; ++guard) {
        videovault::app::DecodedFrame frame;
        if (!decoder.decode_next_video_frame(&frame)) {
            break;
        }
        if (first_pts < 0) {
            first_pts = frame.pts_ms;
        }
        if (prev_pts >= 0 && frame.pts_ms < prev_pts) {
            std::fprintf(stderr, "%s: pts went backwards (%lld -> %lld)\n",
                label, static_cast<long long>(prev_pts), static_cast<long long>(frame.pts_ms));
            return 1;
        }
        prev_pts = frame.pts_ms;
        last_pts = frame.pts_ms;
        ++frames;
        (void)decoder.take_audio_samples();
        if (frame.pts_ms >= target_ms) {
            reached_target = true;
            break;
        }
    }
    if (frames < 1) {
        std::fprintf(stderr, "%s: no frames decoded after the seek\n", label);
        return 1;
    }
    if (first_pts < target_ms - max_gap_ms) {
        std::fprintf(stderr, "%s: first post-seek frame %lld is too far before target %lld (silent restart at 0?)\n",
            label, static_cast<long long>(first_pts), static_cast<long long>(target_ms));
        return 1;
    }
    if (first_pts > target_ms + 250) {
        std::fprintf(stderr, "%s: first post-seek frame %lld is after target %lld (stale pre-seek position?)\n",
            label, static_cast<long long>(first_pts), static_cast<long long>(target_ms));
        return 1;
    }
    if (!reached_target) {
        std::fprintf(stderr, "%s: decode did not advance past the target %lld (last %lld)\n",
            label, static_cast<long long>(target_ms), static_cast<long long>(last_pts));
        return 1;
    }
    std::printf("  %s: ok (first %lld ms, reached %lld ms)\n",
        label, static_cast<long long>(first_pts), static_cast<long long>(last_pts));
    return 0;
}

} // namespace

int main() {
    TemporaryDirectory temp;
    using namespace videovault::core;

    const auto fixture = temp.path() / L"fixture.mkv";
    if (!writeFixture(fixture)) {
        return fail("Generating the AV fixture must succeed.");
    }

    const auto root = temp.path() / L"vault";
    auto created = Vault::create(root, "playback test password", testParameters());
    if (!created) {
        return fail("Creating a vault for the playback test must succeed.");
    }
    const auto imported = created.value().import_file(fixture);
    if (!imported) {
        return fail("Importing the fixture must succeed.");
    }

    // Realistic interframe fixture (MPEG-4 in MP4 + AAC, sparse keyframes) —
    // imported BEFORE the vault object is moved into the first decoder.
    const auto mp4_fixture = temp.path() / L"fixture.mp4";
    if (!writeMp4Fixture(mp4_fixture)) {
        return fail("Generating the MP4 interframe fixture must succeed.");
    }
    const auto imported2 = created.value().import_file(mp4_fixture);
    if (!imported2) {
        return fail("Importing the MP4 fixture must succeed.");
    }

    videovault::app::MediaDecoder decoder;
    QString open_error;
    if (!decoder.open(
            std::make_shared<Vault>(std::move(created.value())),
            imported.value(), &open_error)) {
        std::fprintf(stderr, "MediaDecoder open failed: %s\n", qPrintable(open_error));
        return EXIT_FAILURE;
    }
    if (!decoder.is_open()) {
        return fail("The decoder must report open.");
    }
    if (decoder.duration_ms() < 1500 || decoder.duration_ms() > 2500) {
        return fail("The decoded duration must match the fixture.");
    }
    if (decoder.video_width() != 64 || decoder.video_height() != 64) {
        return fail("The decoded resolution must match the fixture.");
    }
    if (!decoder.has_audio()
        || decoder.audio_sample_rate() != 48000
        || decoder.audio_channels() != 2) {
        return fail("The fixture's audio stream must be detected as 48 kHz stereo.");
    }

    // The streaming cache budget is settable and applies to the decoder.
    decoder.set_stream_cache_bytes(8U << 20);
    if (decoder.stream_cache_bytes() != (8U << 20)) {
        return fail("The streaming cache budget must be settable.");
    }

    // Decode a bounded number of frames; the fixture has 20.
    int video_frames = 0;
    bool first_frame_valid = false;
    for (int guard = 0; guard < 60; ++guard) {
        videovault::app::DecodedFrame frame;
        if (!decoder.decode_next_video_frame(&frame)) {
            break;
        }
        if (video_frames == 0) {
            first_frame_valid = !frame.image.isNull()
                && frame.image.width() == 64
                && frame.image.height() == 64;
        }
        ++video_frames;
        (void)decoder.take_audio_samples();
    }
    if (video_frames < 10) {
        return fail("Fewer than 10 video frames were decoded.");
    }
    if (!first_frame_valid) {
        return fail("The first decoded frame must be a valid 64x64 image.");
    }

    // Seeking must work and produce more frames at/after the target — this
    // guards against a seek silently restarting from the beginning.
    if (decoder.seek_to(1000) < 0) {
        return fail("Seeking to 1 second must succeed.");
    }
    int post_seek_frames = 0;
    bool post_seek_at_target = false;
    for (int guard = 0; guard < 30; ++guard) {
        videovault::app::DecodedFrame frame;
        if (!decoder.decode_next_video_frame(&frame)) {
            break;
        }
        if (post_seek_frames == 0 && frame.pts_ms < 950) {
            return fail("The first post-seek frame must be at or near the seek target.");
        }
        ++post_seek_frames;
    }
    if (post_seek_frames < 1) {
        return fail("Decoding after a seek must produce frames.");
    }

    // The audio path must have produced PCM samples (the fixture has sound).
    auto samples = decoder.take_audio_samples();
    if (samples.empty()) {
        return fail("Decoding the fixture must produce audio samples.");
    }

    // Display-size decoding: sws must output exactly the requested size.
    if (decoder.seek_to(0) < 0) {
        return fail("Seeking back to the start must succeed.");
    }
    decoder.set_display_size(32, 32);
    videovault::app::DecodedFrame small;
    if (!decoder.decode_next_video_frame(&small)
        || small.image.isNull()
        || small.image.width() != 32
        || small.image.height() != 32) {
        return fail("Display-size decoding must output the requested 32x32 size.");
    }

    std::printf("Playback decode checks succeeded (%d frames, %zu audio samples).\n",
        video_frames, samples.size());

    // --- Strict post-seek verification on the interframe MP4 fixture. This
    // is the shape of real user videos; the MJPEG fixture above is
    // all-keyframes and cannot reproduce sparse-keyframe seek bugs.
    auto opened = Vault::open(root, "playback test password");
    if (!opened) {
        return fail("Reopening the vault for the MP4 fixture must succeed.");
    }
    videovault::app::MediaDecoder decoder2;
    QString open_error2;
    if (!decoder2.open(
            std::make_shared<Vault>(std::move(opened.value())),
            imported2.value(), &open_error2)) {
        std::fprintf(stderr, "MP4 decoder open failed: %s\n", qPrintable(open_error2));
        return EXIT_FAILURE;
    }
    if (decoder2.duration_ms() < 2500 || decoder2.duration_ms() > 3500) {
        return fail("The MP4 duration must match its 3-second fixture.");
    }
    if (decoder2.video_width() != 160 || decoder2.video_height() != 120) {
        return fail("The MP4 resolution must match the fixture.");
    }
    if (!decoder2.has_audio() || decoder2.audio_sample_rate() != 48000) {
        return fail("The MP4 fixture's AAC stream must be detected (48 kHz).");
    }

    std::printf("Strict post-seek verification (interframe MP4):\n");
    // Keyframes at 0 / 1000 / 2000 ms; max backward gap = 1000 ms (+200 slack).
    if (checkSeek(decoder2, "seek to 1500 ms", 1500, 1200) != 0) {
        return EXIT_FAILURE;
    }
    if (checkSeek(decoder2, "seek to 2500 ms", 2500, 1200) != 0) {
        return EXIT_FAILURE;
    }
    if (checkSeek(decoder2, "seek to 500 ms", 500, 1200) != 0) {
        return EXIT_FAILURE;
    }

    std::printf("MP4 interframe seek checks succeeded.\n");

    // --- Headless PlayerWindow seek test. Runs the REAL player on the
    // offscreen platform: worker thread, tick timer, clock re-anchor, and the
    // slider-release -> doSeek path (simulated by setting the slider value and
    // emitting sliderReleased, which the connected handler routes to doSeek).
    qputenv("QT_PLUGIN_PATH", QLibraryInfo::location(QLibraryInfo::PluginsPath).toUtf8());
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    int player_argc = 1;
    char player_name[] = "megavideoprotect-headless-player-test";
    char* player_argv[1] = {player_name};
    QApplication player_app(player_argc, player_argv);

    auto reopened = Vault::open(root, "playback test password");
    if (!reopened) {
        return fail("Reopening the vault for the headless player must succeed.");
    }
    videovault::app::PlayerWindow player(
        std::make_shared<Vault>(std::move(reopened.value())),
        imported2.value(), QStringLiteral("headless seek test"));
    auto* slider = player.findChild<QSlider*>();
    if (slider == nullptr) {
        return fail("The player must expose its position slider.");
    }

    const auto spin = [&player_app](const std::chrono::milliseconds duration) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            player_app.processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    // Let playback start from the beginning.
    spin(std::chrono::milliseconds(1500));
    const int pos_before = slider->value();
    if (pos_before < 200) {
        std::fprintf(stderr, "Playback must start advancing (slider %d ms).\n", pos_before);
        return EXIT_FAILURE;
    }

    // Simulate dragging the thumb to the middle and releasing it.
    slider->setValue(1500);
    emit slider->sliderReleased();
    spin(std::chrono::milliseconds(800));
    const int pos_after = slider->value();
    if (pos_after < 1100 || pos_after > 2900) {
        std::fprintf(stderr, "Seek to 1500 ms failed: slider at %d ms.\n", pos_after);
        return EXIT_FAILURE;
    }
    spin(std::chrono::milliseconds(800));
    const int pos_later = slider->value();
    if (pos_later <= pos_after) {
        std::fprintf(stderr, "Playback did not advance past the seek point (%d -> %d ms).\n",
            pos_after, pos_later);
        return EXIT_FAILURE;
    }

    // Simulate seeking backwards.
    slider->setValue(400);
    emit slider->sliderReleased();
    spin(std::chrono::milliseconds(800));
    const int pos_back = slider->value();
    if (pos_back < 0 || pos_back > 900) {
        std::fprintf(stderr, "Seek back to 400 ms failed: slider at %d ms.\n", pos_back);
        return EXIT_FAILURE;
    }

    std::printf("Headless player seek checks succeeded (%d -> %d -> %d -> %d ms).\n",
        pos_before, pos_after, pos_later, pos_back);
    return EXIT_SUCCESS;
}
