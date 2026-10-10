#pragma once

#include "internal/package.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace videovault::core::internal {

struct MediaProbeResult {
    std::uint64_t duration_ms{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t rotation_degrees{0U};
    std::string codec_name;
    std::string title;
    std::string artist;
};

// Probes container metadata through FFmpeg, reading the plaintext stream from
// `reader`. Reader authentication failures surface as PackageModified.
[[nodiscard]] Result<MediaProbeResult> probe_media(PackageReader& reader);

// Header-only probe for a song. Does not scan the file to estimate duration.
[[nodiscard]] Result<MediaProbeResult> probe_audio_media(
    PackageReader& reader,
    const char* name_hint);

// Decodes a representative frame, scales it to fit `max_dimension` (keeping
// the aspect ratio; no upscaling), and encodes it as JPEG. Returns the JPEG
// bytes and the scaled dimensions.
[[nodiscard]] Result<std::vector<unsigned char>> extract_thumbnail_jpeg(
    PackageReader& reader,
    std::uint32_t max_dimension,
    std::uint32_t& out_width,
    std::uint32_t& out_height);

// Album art for a song. Reads the embedded cover when the container has one,
// otherwise paints the music-note JPEG. Never walks the file for a video frame.
// art_kind: 1 embedded cover, 0 opened and there is no cover (note image),
// -1 could not read the file (note image, do not treat it as final).
[[nodiscard]] Result<std::vector<unsigned char>> extract_audio_thumbnail_jpeg(
    PackageReader& reader,
    std::uint32_t max_dimension,
    std::uint32_t& out_width,
    std::uint32_t& out_height,
    const char* name_hint,
    int& art_kind);

} // namespace videovault::core::internal
