#include "videovault/core/vault.hpp"

// FFmpeg 7.x public headers carry no extern "C" guards; wrap them so the
// C++ references match the DLLs' undecorated C exports.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVideoProtect_Phase5_media_" + suffix);
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
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

// ctest starts in the build tree (out/build/<preset>/tests), not the repo.
// Walk parents until the repo-relative file is found.
std::filesystem::path repoPath(const std::filesystem::path& relative) {
    std::error_code error;
    auto dir = std::filesystem::current_path(error);
    if (error) {
        return {};
    }
    for (int i = 0; i < 8; ++i) {
        const auto candidate = dir / relative;
        if (std::filesystem::exists(candidate, error)) {
            return candidate;
        }
        if (!dir.has_parent_path()) {
            break;
        }
        const auto parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

videovault::core::Argon2Parameters testParameters() {
    videovault::core::Argon2Parameters parameters;
    parameters.memory_kib = 8U * 1024U;
    parameters.iterations = 2U;
    parameters.parallelism = 1U;
    return parameters;
}

// Writes a tiny Matroska container holding MJPEG frames: 64x64, 10 fps,
// 20 frames of a gradient pattern. MJPEG is used because the AVI/MKV muxers
// reject rawvideo; mjpeg is a real codec with proper duration signaling.
bool writeMjpegMkv(
    const std::filesystem::path& path,
    const int frame_count) {
    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    constexpr int kFps = 10;
    const int frame_size = kWidth * kHeight * 3;

    std::vector<unsigned char> rgb(
        static_cast<std::size_t>(frame_size) * frame_count);
    for (int frame = 0; frame < frame_count; ++frame) {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const auto offset = static_cast<std::size_t>(
                    frame * frame_size + (y * kWidth + x) * 3);
                rgb[offset + 0] = static_cast<unsigned char>(
                    (x * 255 / kWidth + frame * 3) & 0xff);
                rgb[offset + 1] = static_cast<unsigned char>(
                    (y * 255 / kHeight) & 0xff);
                rgb[offset + 2] = static_cast<unsigned char>(
                    ((x + y) * 255 / (kWidth + kHeight) + frame * 5) & 0xff);
            }
        }
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (encoder == nullptr) {
        return false;
    }
    AVCodecContext* codec = avcodec_alloc_context3(encoder);
    if (codec == nullptr) {
        return false;
    }
    codec->width = kWidth;
    codec->height = kHeight;
    codec->time_base = AVRational{1, kFps};
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->color_range = AVCOL_RANGE_JPEG;
    if (avcodec_open2(codec, encoder, nullptr) < 0) {
        avcodec_free_context(&codec);
        return false;
    }

    SwsContext* scaler = sws_getContext(
        kWidth, kHeight, AV_PIX_FMT_RGB24,
        kWidth, kHeight, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
        nullptr, nullptr, nullptr);
    if (scaler == nullptr) {
        avcodec_free_context(&codec);
        return false;
    }

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
        avcodec_free_context(&codec);
        sws_freeContext(scaler);
        return false;
    }
    AVStream* stream = avformat_new_stream(format, nullptr);
    if (stream == nullptr) {
        avformat_free_context(format);
        avcodec_free_context(&codec);
        sws_freeContext(scaler);
        return false;
    }
    stream->id = format->nb_streams - 1;
    stream->time_base = AVRational{1, kFps};
    AVCodecParameters* parameters = stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_MJPEG;
    parameters->format = AV_PIX_FMT_YUV420P;
    parameters->width = kWidth;
    parameters->height = kHeight;
    if (avio_open(&format->pb, path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
        avformat_free_context(format);
        avcodec_free_context(&codec);
        sws_freeContext(scaler);
        return false;
    }
    if (avformat_write_header(format, nullptr) < 0) {
        avio_close(format->pb);
        avformat_free_context(format);
        avcodec_free_context(&codec);
        sws_freeContext(scaler);
        return false;
    }

    const AVRational source_tb = AVRational{1, kFps};
    const AVRational stream_tb = stream->time_base; // muxer may override
    AVPacket* packet = av_packet_alloc();
    bool success = true;
    for (int frame = 0; frame < frame_count; ++frame) {
        sws_scale(scaler, src_slices, src_strides, 0, kHeight,
            yuv->data, yuv->linesize);
        yuv->pts = av_rescale_q(frame, source_tb, stream_tb);
        if (avcodec_send_frame(codec, yuv) < 0
            || avcodec_receive_packet(codec, packet) < 0) {
            success = false;
            break;
        }
        packet->stream_index = stream->index;
        packet->pts = av_rescale_q(frame, source_tb, stream_tb);
        packet->dts = packet->pts;
        if (av_write_frame(format, packet) < 0) {
            success = false;
            av_packet_unref(packet);
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

bool isJpeg(const std::vector<unsigned char>& bytes) {
    return bytes.size() >= 3U
        && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff;
}

} // namespace

int main() {
    TemporaryDirectory temp;
    using namespace videovault::core;

    const auto fixture = temp.path() / L"fixture.mkv";
    if (!writeMjpegMkv(fixture, 20)) {
        return fail("Generating the MJPEG fixture must succeed.");
    }

    const auto root = temp.path() / L"vault";
    auto created = Vault::create(root, "media test password", testParameters());
    if (!created) {
        return fail("Creating a vault for media tests must succeed.");
    }
    // The optional progress callback must fire with a monotonic 0.0..1.0
    // fraction as the file is encrypted.
    {
        double last = -1.0;
        bool saw_final = false;
        bool monotonic = true;
        const auto progressed = created.value().import_file(
            fixture, [&](const double fraction) {
                if (fraction < last - 1e-9 || fraction < 0.0 || fraction > 1.0) {
                    monotonic = false;
                }
                last = fraction;
                if (fraction >= 1.0) {
                    saw_final = true;
                }
            });
        if (!progressed) {
            return fail("Re-import with a progress callback must succeed.");
        }
        if (!monotonic || !saw_final) {
            return fail("The import progress callback must be monotonic and reach 1.0.");
        }
    }
    const auto imported = created.value().import_file(fixture);
    if (!imported) {
        return fail("Importing the media fixture must succeed.");
    }
    const int64_t video_id = imported.value();

    // Import must auto-generate a thumbnail (best effort for media content).
    {
        const auto auto_thumb = created.value().thumbnail(video_id);
        if (!auto_thumb || auto_thumb.value().bytes.empty()
            || auto_thumb.value().mime != "image/jpeg"
            || !isJpeg(auto_thumb.value().bytes)) {
            return fail("Import must auto-generate a readable thumbnail.");
        }
    }

    // Restore decrypts a video back to plaintext, byte-identical to the
    // original source file, using its original file name.
    {
        const auto restore_dir = temp.path() / L"restored";
        std::error_code ignored;
        std::filesystem::create_directories(restore_dir, ignored);
        const auto restored = created.value().restore_video(video_id, restore_dir);
        if (!restored) {
            std::cerr << "restore_video failed: "
                      << restored.error().technical_detail << '\n';
            return EXIT_FAILURE;
        }
        if (restored.value().filename() != fixture.filename()) {
            return fail("The restored file must keep the original file name.");
        }
        std::ifstream in(restored.value(), std::ios::binary);
        std::vector<unsigned char> restored_bytes(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ifstream original(fixture, std::ios::binary);
        std::vector<unsigned char> original_bytes(
            (std::istreambuf_iterator<char>(original)), std::istreambuf_iterator<char>());
        if (restored_bytes != original_bytes) {
            return fail("The restored file must be byte-identical to the source.");
        }
        // Restoring to a non-existent directory must fail cleanly.
        const auto bad_dir = temp.path() / L"does-not-exist";
        if (created.value().restore_video(video_id, bad_dir)) {
            return fail("Restoring into a missing directory must fail.");
        }
        if (created.value().restore_video(999999, restore_dir)) {
            return fail("Restoring an unknown video must fail.");
        }
    }

    // Container probing.
    {
        const auto info = created.value().media_info(video_id);
        if (!info) {
            std::cerr << "media_info failed: "
                      << info.error().technical_detail << '\n';
            return EXIT_FAILURE;
        }
        if (info.value().width != 64U || info.value().height != 64U) {
            return fail("The probed resolution must match the fixture.");
        }
        if (info.value().codec_name != "mjpeg") {
            return fail("The probed codec must be mjpeg.");
        }
        // 20 frames at 10 fps = 2000 ms, with a tolerance for container jitter.
        if (info.value().duration_ms < 1500U || info.value().duration_ms > 2500U) {
            std::cerr << "duration_ms probed: " << info.value().duration_ms
                      << ", codec: " << info.value().codec_name << '\n';
            return fail("The probed duration must match the fixture.");
        }
    }

    // Thumbnail generation and retrieval.
    {
        const auto at_native = created.value().generate_thumbnail(video_id, 64U);
        if (!at_native) {
            std::cerr << "generate_thumbnail failed: "
                      << at_native.error().technical_detail << '\n';
            return EXIT_FAILURE;
        }
        if (at_native.value().width != 64U || at_native.value().height != 64U
            || at_native.value().mime != "image/jpeg"
            || !isJpeg(at_native.value().bytes)) {
            return fail("The native-size thumbnail must be a 64x64 JPEG.");
        }

        const auto scaled = created.value().generate_thumbnail(video_id, 32U);
        if (!scaled || scaled.value().width != 32U || scaled.value().height != 32U
            || !isJpeg(scaled.value().bytes)) {
            return fail("A 32px thumbnail must scale the frame down to 32x32.");
        }

        const auto stored = created.value().thumbnail(video_id);
        if (!stored || stored.value().width != 32U || stored.value().height != 32U
            || stored.value().mime != "image/jpeg"
            || stored.value().bytes != scaled.value().bytes) {
            return fail("The stored thumbnail must match the last generated one.");
        }
    }

    // Thumbnails persist across lock and reopen.
    {
        created.value().lock();
        auto reopened = Vault::open(root, "media test password");
        if (!reopened) {
            return fail("The vault must reopen after thumbnail generation.");
        }
        const auto stored = reopened.value().thumbnail(video_id);
        if (!stored || stored.value().bytes.empty()) {
            return fail("The stored thumbnail must survive a reopen.");
        }
        const auto info = reopened.value().media_info(video_id);
        if (!info || info.value().width != 64U) {
            return fail("Media probing must work after a reopen.");
        }
        reopened.value().lock();
    }

    // Removing a video must cascade its thumbnail away.
    {
        auto reopened = Vault::open(root, "media test password");
        if (!reopened) {
            return fail("The vault must reopen for the removal check.");
        }
        const auto removed = reopened.value().remove_video(video_id);
        if (!removed || !removed.value()) {
            return fail("Removing the media video must succeed.");
        }
        const auto gone = reopened.value().thumbnail(video_id);
        if (gone || gone.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Removal must cascade the stored thumbnail away.");
        }
        reopened.value().lock();
    }

    // Non-video content must be rejected as an unsupported format.
    {
        auto reopened = Vault::open(root, "media test password");
        if (!reopened) {
            return fail("The vault must reopen for the non-video check.");
        }
        const auto junk_path = temp.path() / L"junk.bin";
        std::ofstream junk(junk_path, std::ios::binary);
        junk.write("this is definitely not a media container", 40);
        junk.close();
        const auto junk_id = reopened.value().import_file(junk_path);
        if (!junk_id) {
            return fail("Importing the junk file must succeed.");
        }
        const auto probe = reopened.value().media_info(junk_id.value());
        if (probe || probe.error().code != VaultErrorCode::UnsupportedVideoFormat) {
            return fail("Probing non-media content must return UnsupportedVideoFormat.");
        }
        const auto thumb = reopened.value().generate_thumbnail(junk_id.value(), 32U);
        if (thumb || thumb.error().code != VaultErrorCode::UnsupportedVideoFormat) {
            return fail("Thumbnailing non-media content must return UnsupportedVideoFormat.");
        }
        reopened.value().lock();
    }

    // Unknown id and locked vault must fail cleanly.
    {
        const auto unknown = created.value().media_info(99999);
        if (unknown || unknown.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Media probing an unknown id must return InvalidArgument.");
        }
        const auto locked = created.value().media_info(video_id);
        if (locked || locked.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Media probing on a locked vault must fail cleanly.");
        }
    }

    // Thumbnails must survive a password change (they are re-encrypted under
    // the new thumbnail subkey).
    {
        auto reopened = Vault::open(root, "media test password");
        if (!reopened) {
            return fail("The vault must reopen for the password-change check.");
        }
        const auto fresh_id = reopened.value().import_file(fixture);
        if (!fresh_id) {
            return fail("Re-importing the fixture for the password-change check must succeed.");
        }
        const auto before = reopened.value().thumbnail(fresh_id.value());
        if (!before || before.value().bytes.empty()) {
            return fail("The re-imported fixture must carry a thumbnail.");
        }
        const auto changed = reopened.value().change_password(
            "media test password", "changed password", testParameters());
        if (!changed || !changed.value()) {
            return fail("Changing the vault password must succeed.");
        }
        reopened.value().lock();

        auto under_new = Vault::open(root, "changed password");
        if (!under_new) {
            return fail("The vault must open under the changed password.");
        }
        const auto after = under_new.value().thumbnail(fresh_id.value());
        if (!after || after.value().bytes.empty()
            || after.value().bytes != before.value().bytes) {
            return fail("Thumbnails must survive a password change byte-for-byte.");
        }
        const auto info = under_new.value().media_info(fresh_id.value());
        if (!info || info.value().width != 64U) {
            return fail("Media probing must work after a password change.");
        }
        under_new.value().lock();
    }

    // A 1x1 semi-transparent PNG: odd dimensions plus alpha. Windows FFmpeg
    // must be built with zlib or the PNG decoder is missing and this import
    // produces no thumbnail. The JPEG encoder also rejects odd YUV420 sizes,
    // so the stored thumbnail is padded to an even size.
    {
        static const unsigned char kTinyPng[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
            0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
            0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x4F, 0x25, 0xC4, 0xD6, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44,
            0x41, 0x54, 0x78, 0x9C, 0x63, 0xFC, 0xCF, 0xC0, 0x50, 0x0F, 0x00, 0x04,
            0x85, 0x01, 0x80, 0x5B, 0xF7, 0x9E, 0x31, 0x00, 0x00, 0x00, 0x00, 0x49,
            0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
        };
        const auto png_path = temp.path() / L"tiny.png";
        {
            std::ofstream out(png_path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(kTinyPng), sizeof(kTinyPng));
            if (!out) {
                return fail("Writing the PNG fixture must succeed.");
            }
        }
        const auto png_root = temp.path() / L"png-vault";
        auto png_vault = Vault::create(png_root, "png password", testParameters());
        if (!png_vault) {
            return fail("Creating a vault for the PNG thumbnail check must succeed.");
        }
        const auto png_id = png_vault.value().import_file(png_path);
        if (!png_id) {
            return fail("Importing a PNG must succeed.");
        }
        const auto thumb = png_vault.value().thumbnail(png_id.value());
        if (!thumb || thumb.value().bytes.empty()
            || thumb.value().mime != "image/jpeg"
            || !isJpeg(thumb.value().bytes)
            || thumb.value().width < 2U || thumb.value().height < 2U
            || (thumb.value().width & 1U) != 0U
            || (thumb.value().height & 1U) != 0U) {
            std::cerr << "PNG thumbnail missing or not an even-sized JPEG.\n";
            return fail("Importing a PNG must auto-generate an even-sized JPEG thumbnail.");
        }
        const auto info = png_vault.value().media_info(png_id.value());
        if (!info || info.value().codec_name != "png"
            || info.value().width != 1U || info.value().height != 1U) {
            return fail("Probing a PNG must report the png codec and its real size.");
        }
    }

    // An H.264 clip whose sync-sample table does not flag the IDR. The
    // keyframe-only thumbnail walk used to drop every packet and import
    // stored no JPEG. The fixture is tests/fixtures/h264-unmarked-key.mp4.
    {
        const auto sample = repoPath(
            std::filesystem::path("tests") / "fixtures" / "h264-unmarked-key.mp4");
        if (sample.empty()) {
            return fail("The unmarked-key H.264 fixture must be present.");
        }
        const auto short_root = temp.path() / L"short-h264-vault";
        auto vault = Vault::create(short_root, "short h264 password", testParameters());
        if (!vault) {
            return fail("Creating a vault for the unmarked-key H.264 sample must succeed.");
        }
        const auto id = vault.value().import_file(sample);
        if (!id) {
            std::cerr << "unmarked-key import failed: " << id.error().technical_detail << "\n";
            return fail("Importing the unmarked-key H.264 sample must succeed.");
        }
        const auto generated = vault.value().generate_thumbnail(id.value(), 320U);
        if (!generated) {
            std::cerr << "unmarked-key generate_thumbnail failed: "
                << generated.error().technical_detail << "\n";
        }
        const auto thumb = vault.value().thumbnail(id.value());
        if (!thumb || thumb.value().bytes.empty() || !isJpeg(thumb.value().bytes)) {
            std::cerr << "unmarked-key thumbnail missing: "
                << (thumb ? "empty jpeg" : thumb.error().technical_detail) << "\n";
            return fail("Importing an H.264 clip with no flagged keyframe must auto-generate a thumbnail.");
        }
        std::cout << "unmarked-key thumbnail " << thumb.value().width << "x"
            << thumb.value().height << " (" << thumb.value().bytes.size() << " bytes)\n";
    }

    std::cout << "Media probing and thumbnail checks succeeded.\n";
    return EXIT_SUCCESS;
}
