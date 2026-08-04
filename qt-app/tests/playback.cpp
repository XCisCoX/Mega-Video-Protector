// Headless playback test: exercises MediaDecoder (the same engine the player
// window uses) against an imported encrypted video — frame decoding, audio
// samples, and seeking — with no GUI involved.
#include "videovault/app/media_decoder.hpp"
#include "videovault/core/vault.hpp"

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

    // Seeking must work and produce more frames.
    if (!decoder.seek_to(1000)) {
        return fail("Seeking to 1 second must succeed.");
    }
    int post_seek_frames = 0;
    for (int guard = 0; guard < 30; ++guard) {
        videovault::app::DecodedFrame frame;
        if (!decoder.decode_next_video_frame(&frame)) {
            break;
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
    if (!decoder.seek_to(0)) {
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
    return EXIT_SUCCESS;
}
