#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

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

private:
    class Impl;
    explicit Vault(std::filesystem::path root, std::unique_ptr<Impl> impl) noexcept;

    std::filesystem::path root_;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view user_message(VaultErrorCode code) noexcept;

} // namespace videovault::core
