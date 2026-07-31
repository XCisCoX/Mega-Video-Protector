#include "videovault/core/vault.hpp"

#include "internal/crypto.hpp"
#include "internal/database.hpp"
#include "internal/metadata.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace videovault::core {
namespace {

constexpr std::array<char, 8> kDatabaseContext{'M', 'V', 'P', 'D', 'B', '0', '0', '1'};
constexpr std::array<char, 8> kVerifierContext{'M', 'V', 'P', 'V', 'E', 'R', '0', '1'};
constexpr std::uint64_t kDatabaseSubkeyId = 1U;
constexpr std::uint64_t kVerifierSubkeyId = 2U;

struct Paths {
    explicit Paths(const std::filesystem::path& root)
        : metadata(root / L"vault.meta"),
          database(root / L"vault.db"),
          ready(root / L"vault.ready"),
          temporary_directory(root / L".tmp"),
          temporary_database(temporary_directory / L"vault.db.create") {}

    std::filesystem::path metadata;
    std::filesystem::path database;
    std::filesystem::path ready;
    std::filesystem::path temporary_directory;
    std::filesystem::path temporary_database;
};

VaultError filesystem_error(const std::error_code& error, const char* operation) {
    VaultErrorCode code = VaultErrorCode::DatabaseFailure;
    if (error == std::errc::permission_denied) {
        code = VaultErrorCode::PermissionDenied;
    } else if (error == std::errc::no_space_on_device) {
        code = VaultErrorCode::DiskFull;
    }
    return {code, std::string(operation) + ": " + error.message()};
}

bool any_vault_file_exists(const Paths& paths) noexcept {
    std::error_code ignored;
    return std::filesystem::exists(paths.metadata, ignored)
        || std::filesystem::exists(paths.database, ignored)
        || std::filesystem::exists(paths.ready, ignored);
}

void remove_creation_artifacts(const Paths& paths) noexcept {
    std::error_code ignored;
    for (const auto& path : {
             paths.temporary_database,
             std::filesystem::path(paths.temporary_database.wstring() + L"-journal"),
             paths.ready,
             std::filesystem::path(paths.ready.wstring() + L".write.tmp"),
             paths.metadata,
             std::filesystem::path(paths.metadata.wstring() + L".write.tmp"),
             paths.database,
             std::filesystem::path(paths.database.wstring() + L"-journal")}) {
        std::filesystem::remove(path, ignored);
        ignored.clear();
    }
}

} // namespace

class Vault::Impl final {
public:
    Impl(
        internal::SensitiveBuffer master_key,
        internal::Database database,
        const Argon2Parameters parameters)
        : master_key_(std::move(master_key)),
          database_(std::move(database)),
          parameters_(parameters) {}

    internal::SensitiveBuffer master_key_;
    internal::Database database_;
    Argon2Parameters parameters_;
};

Vault::Vault() noexcept = default;
Vault::~Vault() = default;
Vault::Vault(Vault&&) noexcept = default;
Vault& Vault::operator=(Vault&&) noexcept = default;

Vault::Vault(std::filesystem::path root, std::unique_ptr<Impl> impl) noexcept
    : root_(std::move(root)), impl_(std::move(impl)) {}

bool Vault::exists(const std::filesystem::path& root) noexcept {
    const Paths paths(root);
    std::error_code ignored;
    return std::filesystem::is_regular_file(paths.metadata, ignored)
        && std::filesystem::is_regular_file(paths.database, ignored)
        && std::filesystem::is_regular_file(paths.ready, ignored);
}

Result<Vault> Vault::create(
    const std::filesystem::path& root,
    const std::string_view password,
    const Argon2Parameters parameters) {
    if (root.empty()) {
        return VaultError{VaultErrorCode::InvalidArgument, "vault storage location must not be empty"};
    }
    const auto validation = internal::validate_argon2_parameters(parameters, password);
    if (!validation.technical_detail.empty()) {
        return validation;
    }
    auto initialized = internal::initialize_crypto();
    if (!initialized) {
        return initialized.error();
    }

    const Paths paths(root);
    if (any_vault_file_exists(paths)) {
        return VaultError{VaultErrorCode::VaultAlreadyExists,
            "vault metadata, database, or ready marker already exists"};
    }

    std::error_code error;
    const bool root_preexisted = std::filesystem::exists(root, error);
    if (error) {
        return filesystem_error(error, "inspect vault location");
    }
    if (root_preexisted && !std::filesystem::is_directory(root, error)) {
        return VaultError{VaultErrorCode::InvalidArgument, "vault location is not a directory"};
    }
    std::filesystem::create_directories(root / L"vault-data", error);
    if (error) {
        return filesystem_error(error, "create vault data directory");
    }
    std::filesystem::create_directories(paths.temporary_directory, error);
    if (error) {
        return filesystem_error(error, "create vault temporary directory");
    }

    auto metadata = internal::create_metadata_header(parameters);
    if (!metadata) {
        remove_creation_artifacts(paths);
        return metadata.error();
    }
    auto master_key = internal::derive_master_key(
        password, metadata.value().salt, parameters);
    if (!master_key) {
        remove_creation_artifacts(paths);
        return master_key.error();
    }
    auto database_key = internal::derive_subkey(
        master_key.value(), kDatabaseSubkeyId, kDatabaseContext);
    if (!database_key) {
        remove_creation_artifacts(paths);
        return database_key.error();
    }
    auto verifier_key = internal::derive_subkey(
        master_key.value(), kVerifierSubkeyId, kVerifierContext);
    if (!verifier_key) {
        remove_creation_artifacts(paths);
        return verifier_key.error();
    }
    auto sealed = internal::seal_password_verifier(metadata.value(), verifier_key.value());
    if (!sealed) {
        remove_creation_artifacts(paths);
        return sealed.error();
    }

    std::filesystem::remove(paths.temporary_database, error);
    error.clear();
    auto created_database = internal::Database::create(
        paths.temporary_database, database_key.value(), metadata.value());
    if (!created_database) {
        remove_creation_artifacts(paths);
        return created_database.error();
    }
    created_database.value().close();

    auto flushed = internal::flush_existing_file(paths.temporary_database);
    if (!flushed) {
        remove_creation_artifacts(paths);
        return flushed.error();
    }
    auto moved = internal::move_file_atomic(paths.temporary_database, paths.database);
    if (!moved) {
        remove_creation_artifacts(paths);
        return moved.error();
    }
    auto metadata_written = internal::write_atomic_file(
        paths.metadata, metadata.value().encoded);
    if (!metadata_written) {
        remove_creation_artifacts(paths);
        return metadata_written.error();
    }
    auto ready_written = internal::write_ready_marker(paths.ready, metadata.value());
    if (!ready_written) {
        remove_creation_artifacts(paths);
        return ready_written.error();
    }

    auto database = internal::Database::open(paths.database, database_key.value());
    if (!database) {
        remove_creation_artifacts(paths);
        return database.error();
    }
    auto verified = database.value().verify_vault_metadata(
        metadata.value(), verifier_key.value());
    if (!verified) {
        remove_creation_artifacts(paths);
        return verified.error();
    }

    auto impl = std::make_unique<Impl>(
        std::move(master_key.value()), std::move(database.value()), parameters);
    return Vault(root, std::move(impl));
}

Result<Vault> Vault::open(
    const std::filesystem::path& root,
    const std::string_view password) {
    if (root.empty()) {
        return VaultError{VaultErrorCode::InvalidArgument, "vault storage location must not be empty"};
    }
    auto initialized = internal::initialize_crypto();
    if (!initialized) {
        return initialized.error();
    }

    const Paths paths(root);
    if (!exists(root)) {
        if (any_vault_file_exists(paths)) {
            return VaultError{VaultErrorCode::MetadataCorrupt,
                "vault is incomplete: metadata, database, and ready marker are required"};
        }
        return VaultError{VaultErrorCode::VaultNotFound, "no vault exists at the selected location"};
    }

    auto metadata = internal::read_metadata_file(paths.metadata);
    if (!metadata) {
        return metadata.error();
    }
    auto marker = internal::verify_ready_marker(paths.ready, metadata.value());
    if (!marker) {
        return marker.error();
    }
    auto master_key = internal::derive_master_key(
        password, metadata.value().salt, metadata.value().parameters);
    if (!master_key) {
        if (master_key.error().code == VaultErrorCode::InvalidArgument && password.empty()) {
            return VaultError{VaultErrorCode::WrongPassword, "password is empty"};
        }
        return master_key.error();
    }
    auto verifier_key = internal::derive_subkey(
        master_key.value(), kVerifierSubkeyId, kVerifierContext);
    if (!verifier_key) {
        return verifier_key.error();
    }
    auto database_key = internal::derive_subkey(
        master_key.value(), kDatabaseSubkeyId, kDatabaseContext);
    if (!database_key) {
        return database_key.error();
    }

    auto password_verified = internal::verify_password_verifier(
        metadata.value(), verifier_key.value(), VaultErrorCode::WrongPassword);
    if (!password_verified) {
        // A valid SQLCipher key proves that the password-derived master key is
        // correct. In that case a failed external verifier is authenticated
        // metadata damage, not an incorrect password. The dedicated verifier
        // remains the primary password check; database opening is only used to
        // distinguish these two typed error conditions.
        auto database_probe = internal::Database::open(paths.database, database_key.value());
        if (database_probe) {
            return VaultError{VaultErrorCode::AuthenticationFailed,
                "password verifier authentication failed while the SQLCipher key remained valid"};
        }
        return password_verified.error();
    }

    auto database = internal::Database::open(paths.database, database_key.value());
    if (!database) {
        return database.error();
    }
    auto database_verified = database.value().verify_vault_metadata(
        metadata.value(), verifier_key.value());
    if (!database_verified) {
        return database_verified.error();
    }

    auto impl = std::make_unique<Impl>(
        std::move(master_key.value()), std::move(database.value()), metadata.value().parameters);
    return Vault(root, std::move(impl));
}

Result<bool> Vault::validate_password(
    const std::filesystem::path& root,
    const std::string_view password) {
    auto opened = open(root, password);
    if (!opened) {
        if (opened.error().code == VaultErrorCode::WrongPassword) {
            return false;
        }
        return opened.error();
    }
    opened.value().lock();
    return true;
}

bool Vault::is_unlocked() const noexcept {
    return impl_ != nullptr;
}

void Vault::lock() noexcept {
    impl_.reset();
}

const std::filesystem::path& Vault::root_path() const noexcept {
    return root_;
}

Argon2Parameters Vault::kdf_parameters() const noexcept {
    return impl_ != nullptr ? impl_->parameters_ : Argon2Parameters{};
}

std::string_view user_message(const VaultErrorCode code) noexcept {
    switch (code) {
    case VaultErrorCode::WrongPassword: return "The password is incorrect.";
    case VaultErrorCode::DatabaseCorrupt: return "The encrypted vault database is damaged or unreadable.";
    case VaultErrorCode::AuthenticationFailed: return "Vault authentication failed; stored data may have been modified.";
    case VaultErrorCode::PermissionDenied: return "Access to the selected location was denied.";
    case VaultErrorCode::DiskFull: return "There is not enough free disk space.";
    case VaultErrorCode::CryptoFailure: return "The cryptographic operation failed.";
    case VaultErrorCode::DatabaseFailure: return "The encrypted database operation failed.";
    case VaultErrorCode::InvalidArgument: return "The supplied vault settings are invalid.";
    case VaultErrorCode::VaultAlreadyExists: return "A vault already exists at this location.";
    case VaultErrorCode::VaultNotFound: return "No vault was found at this location.";
    case VaultErrorCode::MetadataCorrupt: return "The vault metadata is missing, damaged, or inconsistent.";
    case VaultErrorCode::UnsupportedVaultVersion: return "This vault was created by an unsupported version.";
    case VaultErrorCode::UnsupportedPackageVersion: return "The video package version is not supported.";
    case VaultErrorCode::PackageMissing: return "An encrypted video package is missing.";
    case VaultErrorCode::PackageModified: return "An encrypted video package has been modified.";
    case VaultErrorCode::UnsupportedVideoFormat: return "The video format is not supported.";
    case VaultErrorCode::ImportCancelled: return "The import was cancelled.";
    }
    return "An unexpected vault error occurred.";
}

} // namespace videovault::core
