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
    std::string package_relative_path;
    std::uint64_t package_size{0U};
    Sha256Digest package_sha256{};
    std::uint32_t format_version{1U};
    std::uint32_t algorithm_id{1U};
    std::uint32_t chunk_size{0U};
    std::array<unsigned char, 16> package_id{};
    std::uint64_t imported_at{0U};
};

struct WrappedVideoKey {
    // 32-byte file key + 16-byte AEAD tag under the wrapping subkey.
    std::array<unsigned char, 48> wrapped{};
    std::array<unsigned char, 24> nonce{};
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

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return database_ != nullptr; }

    // Adopts a successfully opened SQLCipher handle. Internal API only.
    explicit Database(sqlite3* database) noexcept : database_(database) {}

private:
    sqlite3* database_{nullptr};
};

} // namespace videovault::core::internal
