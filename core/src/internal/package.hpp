#pragma once

#include "internal/crypto.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <istream>
#include <limits>
#include <span>
#include <vector>

namespace videovault::core::internal {

// Independently authenticated fixed-size chunk package format ("vvp").
//
// File layout:
//   [68-byte canonical header][16-byte header tag][chunk 0 ciphertext]...
//
// The canonical header is authenticated by an AEAD tag computed over the
// header bytes themselves. Each chunk is independently AEAD-encrypted; its
// associated data binds the header digest, package id, chunk index, plaintext
// length, and the final-chunk flag. Nonces are the 16-byte random package
// prefix plus a 64-bit index (the header uses the reserved all-ones index).

inline constexpr std::array<unsigned char, 8> kPackageMagic{
    'M', 'V', 'P', 'V', 'V', 'P', '0', '1'};
inline constexpr std::uint32_t kPackageFormatVersion = 1U;
inline constexpr std::uint32_t kPackageAlgorithmXChaCha20Poly1305 = 1U;
inline constexpr std::size_t kPackageHeaderSize = 68U;
inline constexpr std::size_t kPackageTagSize = 16U;
inline constexpr std::uint32_t kDefaultPackageChunkSize = 64U * 1024U;
inline constexpr std::uint32_t kMinimumPackageChunkSize = 1U;
inline constexpr std::uint32_t kMaximumPackageChunkSize = 16U * 1024U * 1024U;
inline constexpr std::size_t kPackageHeaderAndTagSize =
    kPackageHeaderSize + kPackageTagSize;

struct PackageHeader {
    std::uint32_t format_version{kPackageFormatVersion};
    std::uint32_t algorithm_id{kPackageAlgorithmXChaCha20Poly1305};
    std::uint32_t header_size{static_cast<std::uint32_t>(kPackageHeaderSize)};
    std::uint32_t chunk_size{kDefaultPackageChunkSize};
    std::uint64_t plaintext_size{0U};
    std::uint32_t chunk_count{0U};
    std::array<unsigned char, 16> package_id{};
    std::array<unsigned char, 16> nonce_prefix{};
};

struct PackageWriteResult {
    Sha256Digest sha256{};
    std::uint64_t package_size{0U};
    std::array<unsigned char, 16> package_id{};
};

[[nodiscard]] std::array<unsigned char, kPackageHeaderSize>
serialize_package_header(const PackageHeader& header);

[[nodiscard]] Result<PackageHeader> parse_package_header(
    std::span<const unsigned char> bytes);

[[nodiscard]] Sha256Digest package_header_digest(const PackageHeader& header);

// Streams the source into an encrypted package at `destination`. The complete
// ciphertext is hashed incrementally with SHA-256. `plaintext_size` must match
// the bytes actually available in `source`. `progress`, when given, receives a
// monotonic 0.0..1.0 fraction per completed chunk.
[[nodiscard]] Result<PackageWriteResult> write_package_file(
    const std::filesystem::path& destination,
    const SensitiveBuffer& file_key,
    std::uint32_t chunk_size,
    std::istream& source,
    std::uint64_t plaintext_size,
    const std::function<void(double)>& progress = {});

// Reads and authenticates the whole package, returning the plaintext.
[[nodiscard]] Result<std::vector<unsigned char>> read_package_content(
    const std::filesystem::path& path,
    const SensitiveBuffer& file_key);

// Streaming, seekable, authenticated reader over a package file. Chunks are
// decrypted on demand with a bounded LRU cache and every chunk tag is
// verified before its plaintext is exposed.
class PackageReader final {
public:
    PackageReader() noexcept = default;

    [[nodiscard]] static Result<PackageReader> open(
        const std::filesystem::path& path,
        SensitiveBuffer file_key);

    [[nodiscard]] bool is_open() const noexcept { return input_.is_open(); }
    [[nodiscard]] std::uint64_t plaintext_size() const noexcept {
        return plaintext_size_;
    }
    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }

    // Seeks to a plaintext offset; offsets past the end clamp to EOF.
    [[nodiscard]] Result<bool> seek(std::uint64_t offset);

    // Reads up to destination.size() bytes at the current position and
    // returns the number actually read (0 at EOF).
    [[nodiscard]] Result<std::size_t> read(
        std::span<unsigned char> destination);

private:
    static constexpr std::size_t kMaxCachedChunks = 4U;

    struct CachedChunk {
        // An impossible index so empty default slots never satisfy a hit.
        std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};
        std::vector<unsigned char> plaintext;
    };

    [[nodiscard]] Result<std::vector<unsigned char>> chunk(
        std::uint32_t index);
    void note_access(std::size_t slot) noexcept;

    std::ifstream input_;
    PackageHeader header_{};
    Sha256Digest digest_{};
    SensitiveBuffer file_key_;
    std::uint64_t plaintext_size_{0U};
    std::uint64_t position_{0U};
    std::array<CachedChunk, kMaxCachedChunks> cache_{};
    std::array<std::uint32_t, kMaxCachedChunks> last_used_{};
    std::uint32_t clock_{0U};
};

} // namespace videovault::core::internal
