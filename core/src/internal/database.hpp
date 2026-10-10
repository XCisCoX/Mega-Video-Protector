#pragma once

#include "internal/crypto.hpp"
#include "internal/metadata.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace videovault::core::internal {

struct VideoRow {
    std::int64_t id{0};
    std::string display_name;
    std::uint64_t original_size{0U};
    std::uint64_t package_size{0U};
    Sha256Digest package_sha256{};
    std::string package_relative_path;
    std::uint32_t format_version{1U};
    std::uint32_t algorithm_id{1U};
    std::uint32_t chunk_size{0U};
    std::array<unsigned char, 16> package_id{};
    std::uint64_t imported_at{0U};
    // 0 means the library root. Stored as NULL in the database.
    std::int64_t folder_id{0};
    // Phase 7: tag names attached to this video (lowercase-sorted).
    std::vector<std::string> tag_names;
};

// A virtual folder. parent_id 0 is the library root. Packages are not moved.
struct FolderRow {
    std::int64_t id{0};
    std::int64_t parent_id{0};
    std::string name;
    std::uint64_t created_at{0U};
};

struct TagRow {
    std::int64_t id{0};
    std::string name;
    std::int64_t video_count{0};
};

struct WrappedVideoKey {
    // 32-byte file key + 16-byte AEAD tag under the wrapping subkey.
    std::array<unsigned char, 48> wrapped{};
    std::array<unsigned char, 24> nonce{};
};

struct ThumbnailRow {
    std::string mime;
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::array<unsigned char, 24> nonce{};
    std::vector<unsigned char> ciphertext;
    std::uint64_t created_at{0U};
};

class Database final {
public:
    Database() noexcept = default;
    ~Database();

    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] static Result<Database> create(
        const std::filesystem::path& path,
        const SensitiveBuffer& database_key,
        const VaultMetadata& metadata);

    [[nodiscard]] static Result<Database> open(
        const std::filesystem::path& path,
        const SensitiveBuffer& database_key);

    [[nodiscard]] Result<bool> verify_vault_metadata(
        const VaultMetadata& metadata,
        const SensitiveBuffer& verifier_key) const;

    // Phase 3: gallery storage. insert_video commits the video row and its
    // wrapped file key in one transaction.
    [[nodiscard]] Result<std::int64_t> insert_video(
        const VideoRow& row,
        const WrappedVideoKey& key);

    [[nodiscard]] Result<std::vector<VideoRow>> list_videos() const;

    [[nodiscard]] Result<VideoRow> query_video(std::int64_t video_id) const;

    [[nodiscard]] Result<std::optional<VideoRow>> query_video_by_sha256(
        const Sha256Digest& package_sha256) const;

    [[nodiscard]] Result<WrappedVideoKey> query_video_key(
        std::int64_t video_id) const;

    // Phase 4: administration.
    [[nodiscard]] Result<bool> delete_video(std::int64_t video_id);

    [[nodiscard]] Result<bool> update_video_key(
        std::int64_t video_id,
        const WrappedVideoKey& key);

    [[nodiscard]] Result<bool> update_vault_metadata_record(
        const VaultMetadata& metadata);

    // Re-encrypts the whole database file with a new SQLCipher key.
    [[nodiscard]] Result<bool> rekey(const SensitiveBuffer& new_database_key);

    // Phase 5: encrypted thumbnails.
    [[nodiscard]] Result<bool> insert_thumbnail(
        std::int64_t video_id,
        const ThumbnailRow& row);

    [[nodiscard]] Result<ThumbnailRow> query_thumbnail(
        std::int64_t video_id) const;

    [[nodiscard]] Result<std::vector<std::pair<std::int64_t, ThumbnailRow>>>
    list_thumbnails() const;

    // Phase 7: tags.
    // Returns the id of the tag, creating it (case-insensitively) if missing.
    [[nodiscard]] Result<std::int64_t> ensure_tag(const std::string& name);

    [[nodiscard]] Result<bool> tag_video(std::int64_t video_id, std::int64_t tag_id);

    [[nodiscard]] Result<bool> untag_video(std::int64_t video_id, std::int64_t tag_id);

    // Renames a tag (case-insensitively unique). Fails with InvalidArgument
    // when the tag is unknown or the destination name collides.
    [[nodiscard]] Result<std::int64_t> rename_tag(
        std::int64_t tag_id, const std::string& new_name);

    // Deletes a tag entirely; video_tags rows cascade.
    [[nodiscard]] Result<bool> delete_tag(std::int64_t tag_id);

    [[nodiscard]] Result<std::vector<TagRow>> tags_for_video(std::int64_t video_id) const;

    [[nodiscard]] Result<std::vector<TagRow>> list_tags() const;

    // Virtual folders. parent_id 0 is the library root.
    [[nodiscard]] Result<std::vector<FolderRow>> list_folders() const;

    [[nodiscard]] Result<FolderRow> query_folder(std::int64_t folder_id) const;

    [[nodiscard]] Result<bool> name_taken(
        std::int64_t parent_id,
        const std::string& name,
        std::int64_t except_folder_id,
        std::int64_t except_video_id) const;

    [[nodiscard]] Result<std::int64_t> insert_folder(
        std::int64_t parent_id,
        const std::string& name);

    [[nodiscard]] Result<bool> rename_folder_row(
        std::int64_t folder_id,
        const std::string& name);

    [[nodiscard]] Result<bool> set_folder_parent(
        std::int64_t folder_id,
        std::int64_t parent_id);

    // Moves every video in the folder and its children up to parent_id
    // (NULL when parent_id is 0), then deletes the folder. Child folders
    // go with it. Package files are not touched.
    [[nodiscard]] Result<bool> remove_folder_row(std::int64_t folder_id);

    [[nodiscard]] Result<bool> rename_video_row(
        std::int64_t video_id,
        const std::string& name);

    [[nodiscard]] Result<bool> set_video_folder(
        std::int64_t video_id,
        std::int64_t folder_id);

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return database_ != nullptr; }

    // Adopts a successfully opened SQLCipher handle. Internal API only.
    explicit Database(sqlite3* database) noexcept : database_(database) {}

private:
    sqlite3* database_{nullptr};
};

} // namespace videovault::core::internal
