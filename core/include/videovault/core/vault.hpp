#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace videovault::core {

enum class VaultErrorCode {
    WrongPassword,
    DatabaseCorrupt,
    UnsupportedPackageVersion,
    AuthenticationFailed,
    PackageMissing,
    PackageModified,
    UnsupportedVideoFormat,
    ImportCancelled,
    DiskFull,
    PermissionDenied,
    CryptoFailure,
    DatabaseFailure,
    InvalidArgument,
    VaultAlreadyExists,
    VaultNotFound,
    MetadataCorrupt,
    UnsupportedVaultVersion
};

struct VaultError {
    VaultErrorCode code{VaultErrorCode::DatabaseFailure};
    std::string technical_detail;
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T&& value) : storage_(std::move(value)) {}
    Result(const T& value) requires std::is_copy_constructible_v<T> : storage_(value) {}
    Result(VaultError error) : storage_(std::move(error)) {}

    explicit operator bool() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] bool has_value() const noexcept { return static_cast<bool>(*this); }

    T& value() & { return std::get<T>(storage_); }
    const T& value() const& { return std::get<T>(storage_); }
    T&& value() && { return std::get<T>(std::move(storage_)); }

    VaultError& error() & { return std::get<VaultError>(storage_); }
    const VaultError& error() const& { return std::get<VaultError>(storage_); }

private:
    std::variant<T, VaultError> storage_;
};

struct Argon2Parameters {
    std::uint32_t memory_kib{256U * 1024U};
    std::uint32_t iterations{3U};
    std::uint32_t parallelism{1U};
};

// Gallery entry for an imported video. package_sha256 is the SHA-256 of the
// complete encrypted package file and also encodes its storage path
// (vault-data/<h0h1>/<h2h3>/<64 lowercase hex>.vvp).
struct VideoInfo {
    std::int64_t id{0};
    std::string display_name;
    std::uint64_t original_size{0U};
    std::uint64_t package_size{0U};
    std::array<unsigned char, 32> package_sha256{};
    std::string package_relative_path;
    std::uint64_t imported_at{0U};
    // Phase 7: tag names attached to this video (case-insensitively sorted).
    std::vector<std::string> tags;
};

// Phase 7: a tag with the number of videos it is attached to.
struct TagInfo {
    std::int64_t id{0};
    std::string name;
    std::int64_t video_count{0};
};

// Phase 5: container metadata probed through FFmpeg.
struct MediaInfo {
    std::int64_t video_id{0};
    std::uint64_t duration_ms{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t rotation_degrees{0U};
    std::string codec_name;
};

// A decrypted JPEG thumbnail ready for display.
struct ThumbnailInfo {
    std::int64_t video_id{0};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::string mime;
    std::vector<unsigned char> bytes;
};

class Vault final {
public:
    Vault() noexcept;
    ~Vault();

    Vault(Vault&&) noexcept;
    Vault& operator=(Vault&&) noexcept;

    Vault(const Vault&) = delete;
    Vault& operator=(const Vault&) = delete;

    [[nodiscard]] static bool exists(const std::filesystem::path& root) noexcept;

    [[nodiscard]] static Result<Vault> create(
        const std::filesystem::path& root,
        std::string_view password,
        Argon2Parameters parameters = {});

    [[nodiscard]] static Result<Vault> open(
        const std::filesystem::path& root,
        std::string_view password);

    [[nodiscard]] static Result<bool> validate_password(
        const std::filesystem::path& root,
        std::string_view password);

    [[nodiscard]] bool is_unlocked() const noexcept;
    void lock() noexcept;

    [[nodiscard]] const std::filesystem::path& root_path() const noexcept;
    [[nodiscard]] Argon2Parameters kdf_parameters() const noexcept;

    // Phase 3: import and gallery.
    // Imports an opaque file as an encrypted package and records it in the
    // vault database. Identical content re-imports return the existing id.
    [[nodiscard]] Result<std::int64_t> import_file(
        const std::filesystem::path& source_path);

    [[nodiscard]] Result<std::vector<VideoInfo>> list_videos() const;

    // Resolves the stored package file for a video and verifies it exists.
    [[nodiscard]] Result<std::filesystem::path> package_path(
        std::int64_t video_id) const;

    // Decrypts a video package fully into memory. Suitable for verification
    // and export of reasonably sized files; playback uses a streaming reader
    // in a later phase.
    [[nodiscard]] Result<std::vector<unsigned char>> read_video_bytes(
        std::int64_t video_id) const;

    // Reads up to `size` plaintext bytes starting at `offset` from a video
    // package (fewer at EOF; empty when offset is at or past the end). Every
    // touched chunk is authenticated before its bytes are returned.
    [[nodiscard]] Result<std::vector<unsigned char>> read_video_range(
        std::int64_t video_id,
        std::uint64_t offset,
        std::size_t size) const;

    // Phase 4: vault administration.
    // Removes a video's gallery row (cascading its wrapped key) and its
    // stored package file. Returns InvalidArgument for an unknown id.
    [[nodiscard]] Result<bool> remove_video(std::int64_t video_id);

    // Re-keys the vault with a new password: new salt and Argon2id parameters,
    // SQLCipher rekey, a fresh sealed password verifier, and re-wrapped file
    // keys. The vault stays unlocked under the new credentials on success.
    [[nodiscard]] Result<bool> change_password(
        std::string_view current_password,
        std::string_view new_password,
        Argon2Parameters parameters = {});

    // Returns the stored thumbnail, decrypting it for display.
    [[nodiscard]] Result<ThumbnailInfo> thumbnail(
        std::int64_t video_id) const;

    // Phase 5: media metadata and encrypted thumbnails.
    // Probes an imported video's container through FFmpeg (streaming over the
    // encrypted package; nothing is written to disk).
    [[nodiscard]] Result<MediaInfo> media_info(std::int64_t video_id) const;

    // Decodes a representative frame, scales it to fit `max_dimension`, and
    // stores the JPEG encrypted in the vault database. Returns the plaintext
    // thumbnail for display.
    [[nodiscard]] Result<ThumbnailInfo> generate_thumbnail(
        std::int64_t video_id,
        std::uint32_t max_dimension = 320U) const;

    // Phase 7: tags. Tag names are trimmed, non-empty, at most 64 bytes, and
    // matched case-insensitively (re-adding "family" to "Family" is a no-op
    // that returns the same tag id). Attaching a tag creates it on demand.
    [[nodiscard]] Result<std::int64_t> add_tag(
        std::int64_t video_id,
        std::string_view tag_name);

    [[nodiscard]] Result<bool> remove_tag(
        std::int64_t video_id,
        std::int64_t tag_id);

    [[nodiscard]] Result<std::vector<TagInfo>> tags_for_video(
        std::int64_t video_id) const;

    // All tags with their video counts, sorted case-insensitively.
    [[nodiscard]] Result<std::vector<TagInfo>> list_tags() const;

private:
    class Impl;
    explicit Vault(std::filesystem::path root, std::unique_ptr<Impl> impl) noexcept;

    // Guards impl_ and all operations on it. Held by unique_ptr so the mutex
    // address is stable across Vault moves. Vault methods are safe to call
    // from a worker thread while the owner may lock() concurrently.
    std::unique_ptr<std::mutex> mutex_;

    std::filesystem::path root_;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view user_message(VaultErrorCode code) noexcept;

} // namespace videovault::core
