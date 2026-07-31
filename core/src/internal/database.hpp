#pragma once

#include "internal/metadata.hpp"

#include <filesystem>

struct sqlite3;

namespace videovault::core::internal {

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

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept { return database_ != nullptr; }

    // Adopts a successfully opened SQLCipher handle. Internal API only.
    explicit Database(sqlite3* database) noexcept : database_(database) {}

private:
    sqlite3* database_{nullptr};
};

} // namespace videovault::core::internal
