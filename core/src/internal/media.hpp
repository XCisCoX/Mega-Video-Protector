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
};

// Probes container metadata through FFmpeg, reading the plaintext stream from
// `reader`. Reader authentication failures surface as PackageModified.
[[nodiscard]] Result<MediaProbeResult> probe_media(PackageReader& reader);

// Decodes a representative frame, scales it to fit `max_dimension` (keeping
// the aspect ratio; no upscaling), and encodes it as JPEG. Returns the JPEG
// bytes and the scaled dimensions.
[[nodiscard]] Result<std::vector<unsigned char>> extract_thumbnail_jpeg(
    PackageReader& reader,
    std::uint32_t max_dimension,
    std::uint32_t& out_width,
    std::uint32_t& out_height);

} // namespace videovault::core::internal
