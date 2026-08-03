#include "videovault/core/vault.hpp"

#include "internal/crypto.hpp"
#include "internal/database.hpp"
#include "internal/metadata.hpp"
#include "internal/package.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace videovault::core {
namespace {

constexpr std::array<char, 8> kDatabaseContext{'M', 'V', 'P', 'D', 'B', '0', '0', '1'};
constexpr std::array<char, 8> kVerifierContext{'M', 'V', 'P', 'V', 'E', 'R', '0', '1'};
constexpr std::array<char, 8> kPackageWrappingContext{'M', 'V', 'P', 'R', 'A', 'P', '0', '1'};
constexpr std::uint64_t kDatabaseSubkeyId = 1U;
constexpr std::uint64_t kVerifierSubkeyId = 2U;
constexpr std::uint64_t kPackageWrappingSubkeyId = 3U;

std::string to_hex_lower(const std::span<const unsigned char> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2U);
    for (const unsigned char byte : bytes) {
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

VaultError locked_error() {
    return {VaultErrorCode::InvalidArgument, "vault is locked"};
}

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

Vault::Vault() noexcept
    : mutex_(std::make_unique<std::mutex>()) {}
Vault::~Vault() = default;
Vault::Vault(Vault&&) noexcept = default;
Vault& Vault::operator=(Vault&&) noexcept = default;

Vault::Vault(std::filesystem::path root, std::unique_ptr<Impl> impl) noexcept
    : mutex_(std::make_unique<std::mutex>()),
      root_(std::move(root)),
      impl_(std::move(impl)) {}

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
    std::lock_guard<std::mutex> guard(*mutex_);
    return impl_ != nullptr;
}

void Vault::lock() noexcept {
    std::lock_guard<std::mutex> guard(*mutex_);
    impl_.reset();
}

const std::filesystem::path& Vault::root_path() const noexcept {
    return root_;
}

Argon2Parameters Vault::kdf_parameters() const noexcept {
    std::lock_guard<std::mutex> guard(*mutex_);
    return impl_ != nullptr ? impl_->parameters_ : Argon2Parameters{};
}

Result<std::int64_t> Vault::import_file(const std::filesystem::path& source_path) {
    std::lock_guard<std::mutex> guard(*mutex_);
    if (!impl_) {
        return locked_error();
    }
    auto& impl = *impl_;
    std::error_code error;
    if (!std::filesystem::is_regular_file(source_path, error)) {
        if (error) {
            return filesystem_error(error, "inspect source file");
        }
        return VaultError{VaultErrorCode::InvalidArgument,
            "the source file is not a regular file"};
    }
    const auto source_size = std::filesystem::file_size(source_path, error);
    if (error) {
        return filesystem_error(error, "inspect source file size");
    }

    const auto data_directory = root_ / L"vault-data";
    const auto staging_directory = data_directory / L".tmp";
    std::filesystem::create_directories(staging_directory, error);
    if (error) {
        return filesystem_error(error, "create package staging directory");
    }

    std::array<unsigned char, 8> staging_suffix{};
    internal::random_bytes(staging_suffix);
    // ASCII-only name; constructing the path from a narrow string is safe.
    const auto staging_package = staging_directory
        / ("import_" + to_hex_lower(staging_suffix) + ".vvp");

    auto wrapping_key = internal::derive_subkey(
        impl.master_key_, kPackageWrappingSubkeyId, kPackageWrappingContext);
    if (!wrapping_key) {
        return wrapping_key.error();
    }

    internal::SensitiveBuffer file_key(32U);
    internal::random_bytes(std::span(file_key.data(), file_key.size()));

    std::ifstream source(source_path, std::ios::binary);
    if (!source) {
        return VaultError{VaultErrorCode::PermissionDenied,
            "unable to open the source file"};
    }
    auto written = internal::write_package_file(
        staging_package, file_key, internal::kDefaultPackageChunkSize, source, source_size);
    if (!written) {
        std::error_code ignored;
        std::filesystem::remove(staging_package, ignored);
        return written.error();
    }

    const auto hex_hash = to_hex_lower(written.value().sha256);
    const auto relative = std::filesystem::path(hex_hash.substr(0U, 2U))
        / hex_hash.substr(2U, 2U) / (hex_hash + ".vvp");
    const auto final_package = data_directory / relative;

    auto duplicate = impl.database_.query_video_by_sha256(written.value().sha256);
    if (!duplicate) {
        std::error_code ignored;
        std::filesystem::remove(staging_package, ignored);
        return duplicate.error();
    }
    if (duplicate.value()) {
        std::error_code ignored;
        std::filesystem::remove(staging_package, ignored);
        return duplicate.value()->id;
    }

    std::filesystem::create_directories(final_package.parent_path(), error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(staging_package, ignored);
        return filesystem_error(error, "create package directory");
    }
    if (std::filesystem::exists(final_package, error) && !error) {
        // Orphan from an interrupted import; the database row is gone.
        std::filesystem::remove(final_package, error);
        if (error) {
            std::error_code ignored;
            std::filesystem::remove(staging_package, ignored);
            return filesystem_error(error, "replace orphan package");
        }
    }
    error.clear();
    auto moved = internal::move_file_atomic(staging_package, final_package);
    if (!moved) {
        return moved.error();
    }

    internal::VideoRow row;
    const auto name_u8 = source_path.filename().u8string();
    row.display_name.assign(
        reinterpret_cast<const char*>(name_u8.data()), name_u8.size());
    row.original_size = source_size;
    const auto relative_u8 = relative.generic_u8string();
    row.package_relative_path.assign(
        reinterpret_cast<const char*>(relative_u8.data()), relative_u8.size());
    row.package_size = written.value().package_size;
    row.package_sha256 = written.value().sha256;
    row.format_version = internal::kPackageFormatVersion;
    row.algorithm_id = internal::kPackageAlgorithmXChaCha20Poly1305;
    row.chunk_size = internal::kDefaultPackageChunkSize;
    row.package_id = written.value().package_id;
    row.imported_at = static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    internal::WrappedVideoKey key;
    internal::random_bytes(key.nonce);
    auto wrapped = internal::encrypt_xchacha20_poly1305(
        std::span(file_key.data(), file_key.size()),
        std::span(row.package_id), key.nonce, wrapping_key.value());
    if (!wrapped || wrapped.value().size() != key.wrapped.size()) {
        std::error_code ignored;
        std::filesystem::remove(final_package, ignored);
        if (!wrapped) {
            return wrapped.error();
        }
        return VaultError{VaultErrorCode::CryptoFailure,
            "wrapped file key has an invalid size"};
    }
    std::copy(wrapped.value().begin(), wrapped.value().end(), key.wrapped.begin());

    auto inserted = impl.database_.insert_video(row, key);
    if (!inserted) {
        std::error_code ignored;
        std::filesystem::remove(final_package, ignored);
        return inserted.error();
    }
    return inserted.value();
}

Result<std::vector<VideoInfo>> Vault::list_videos() const {
    std::lock_guard<std::mutex> guard(*mutex_);
    if (!impl_) {
        return locked_error();
    }
    auto& impl = *impl_;
    auto rows = impl.database_.list_videos();
    if (!rows) {
        return rows.error();
    }
    std::vector<VideoInfo> videos;
    videos.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        VideoInfo info;
        info.id = row.id;
        info.display_name = row.display_name;
        info.original_size = row.original_size;
        info.package_size = row.package_size;
        info.package_sha256 = row.package_sha256;
        info.package_relative_path = row.package_relative_path;
        info.imported_at = row.imported_at;
        videos.push_back(std::move(info));
    }
    return videos;
}

Result<std::filesystem::path> Vault::package_path(const std::int64_t video_id) const {
    std::lock_guard<std::mutex> guard(*mutex_);
    if (!impl_) {
        return locked_error();
    }
    auto& impl = *impl_;
    auto row = impl.database_.query_video(video_id);
    if (!row) {
        return row.error();
    }
    auto package = root_ / L"vault-data" / row.value().package_relative_path;
    std::error_code error;
    if (!std::filesystem::is_regular_file(package, error)) {
        if (error) {
            return filesystem_error(error, "inspect package file");
        }
        return VaultError{VaultErrorCode::PackageMissing,
            "the encrypted video package is missing"};
    }
    return package;
}

Result<std::vector<unsigned char>> Vault::read_video_bytes(
    const std::int64_t video_id) const {
    // The range reader streams with a bounded chunk cache and authenticates
    // everything; a request for the full plaintext size reproduces the old
    // whole-file behavior without any new code path.
    auto row_result = [&]() -> Result<internal::VideoRow> {
        std::lock_guard<std::mutex> guard(*mutex_);
        if (!impl_) {
            return locked_error();
        }
        return impl_->database_.query_video(video_id);
    }();
    if (!row_result) {
        return row_result.error();
    }
    return read_video_range(video_id, 0U, static_cast<std::size_t>(
        std::min<std::uint64_t>(row_result.value().original_size,
            std::numeric_limits<std::size_t>::max())));
}

Result<std::vector<unsigned char>> Vault::read_video_range(
    const std::int64_t video_id,
    const std::uint64_t offset,
    const std::size_t size) const {
    std::lock_guard<std::mutex> guard(*mutex_);
    if (!impl_) {
        return locked_error();
    }
    auto& impl = *impl_;
    auto row = impl.database_.query_video(video_id);
    if (!row) {
        return row.error();
    }
    auto key_record = impl.database_.query_video_key(video_id);
    if (!key_record) {
        return key_record.error();
    }

    auto wrapping_key = internal::derive_subkey(
        impl.master_key_, kPackageWrappingSubkeyId, kPackageWrappingContext);
    if (!wrapping_key) {
        return wrapping_key.error();
    }

    auto unwrapped = internal::decrypt_xchacha20_poly1305(
        key_record.value().wrapped, std::span(row.value().package_id),
        key_record.value().nonce, wrapping_key.value(), VaultErrorCode::PackageModified);
    if (!unwrapped) {
        return unwrapped.error();
    }
    if (unwrapped.value().size() != 32U) {
        return VaultError{VaultErrorCode::DatabaseCorrupt,
            "the unwrapped file key has an invalid size"};
    }

    internal::SensitiveBuffer file_key(32U);
    std::copy(unwrapped.value().begin(), unwrapped.value().end(), file_key.data());
    // Best-effort wipe of the transient plaintext key copy.
    std::fill(unwrapped.value().begin(), unwrapped.value().end(), 0U);

    auto package = root_ / L"vault-data" / row.value().package_relative_path;
    auto reader = internal::PackageReader::open(package, std::move(file_key));
    if (!reader) {
        return reader.error();
    }
    auto sought = reader.value().seek(offset);
    if (!sought) {
        return sought.error();
    }
    std::vector<unsigned char> output(size);
    auto got = reader.value().read(output);
    if (!got) {
        return got.error();
    }
    output.resize(got.value());
    return output;
}

Result<bool> Vault::remove_video(const std::int64_t video_id) {
    std::lock_guard<std::mutex> guard(*mutex_);
    if (!impl_) {
        return locked_error();
    }
    auto& impl = *impl_;
    auto row = impl.database_.query_video(video_id);
    if (!row) {
        return row.error();
    }
    // Delete the row (cascading the wrapped key) first, then remove the
    // package file. A file-removal failure leaves an orphaned package that
    // a later import can replace; a committed row never points at a removed
    // package.
    auto removed = impl.database_.delete_video(video_id);
    if (!removed) {
        return removed.error();
    }
    const auto package = root_ / L"vault-data" / row.value().package_relative_path;
    std::error_code error;
    std::filesystem::remove(package, error);
    if (error) {
        return filesystem_error(error, "remove package file");
    }
    return true;
}

Result<bool> Vault::change_password(
    const std::string_view current_password,
    const std::string_view new_password,
    const Argon2Parameters parameters) {
    std::lock_guard<std::mutex> guard(*mutex_);
    if (!impl_) {
        return locked_error();
    }
    if (current_password.empty() || new_password.empty()) {
        return VaultError{VaultErrorCode::InvalidArgument, "passwords must not be empty"};
    }
    const auto validation = internal::validate_argon2_parameters(parameters, new_password);
    if (!validation.technical_detail.empty()) {
        return validation;
    }
    auto& impl = *impl_;
    const Paths paths(root_);

    // Verify the caller knows the current password by re-deriving from the
    // on-disk metadata and comparing against the live master key.
    auto metadata = internal::read_metadata_file(paths.metadata);
    if (!metadata) {
        return metadata.error();
    }
    auto current_master = internal::derive_master_key(
        current_password, metadata.value().salt, metadata.value().parameters);
    if (!current_master) {
        return current_master.error();
    }
    if (!internal::constant_time_equal(
            std::span(impl.master_key_.data(), impl.master_key_.size()),
            std::span(current_master.value().data(), current_master.value().size()))) {
        return VaultError{VaultErrorCode::WrongPassword, "the current password is incorrect"};
    }

    // New key hierarchy.
    std::array<unsigned char, 16> new_salt{};
    internal::random_bytes(new_salt);
    auto new_master = internal::derive_master_key(new_password, new_salt, parameters);
    if (!new_master) {
        return new_master.error();
    }
    auto new_database_key = internal::derive_subkey(
        new_master.value(), kDatabaseSubkeyId, kDatabaseContext);
    if (!new_database_key) {
        return new_database_key.error();
    }
    auto new_verifier_key = internal::derive_subkey(
        new_master.value(), kVerifierSubkeyId, kVerifierContext);
    if (!new_verifier_key) {
        return new_verifier_key.error();
    }
    auto new_wrapping_key = internal::derive_subkey(
        new_master.value(), kPackageWrappingSubkeyId, kPackageWrappingContext);
    if (!new_wrapping_key) {
        return new_wrapping_key.error();
    }
    auto old_wrapping_key = internal::derive_subkey(
        impl.master_key_, kPackageWrappingSubkeyId, kPackageWrappingContext);
    if (!old_wrapping_key) {
        return old_wrapping_key.error();
    }

    // New metadata keeps the vault id (the ready marker stays valid) and seals
    // a fresh verifier under the new verifier subkey.
    auto new_metadata = internal::create_metadata_header(
        parameters, new_salt, metadata.value().vault_id);
    if (!new_metadata) {
        return new_metadata.error();
    }
    auto sealed = internal::seal_password_verifier(new_metadata.value(), new_verifier_key.value());
    if (!sealed) {
        return sealed.error();
    }

    // Re-wrap every file key under the new wrapping subkey.
    auto rows = impl.database_.list_videos();
    if (!rows) {
        return rows.error();
    }
    struct Rewrapped {
        std::int64_t id;
        internal::WrappedVideoKey key;
    };
    std::vector<Rewrapped> rewrapped;
    rewrapped.reserve(rows.value().size());
    for (const auto& row : rows.value()) {
        auto key_record = impl.database_.query_video_key(row.id);
        if (!key_record) {
            return key_record.error();
        }
        auto file_key = internal::decrypt_xchacha20_poly1305(
            key_record.value().wrapped, std::span(row.package_id),
            key_record.value().nonce, old_wrapping_key.value(),
            VaultErrorCode::CryptoFailure);
        if (!file_key) {
            return file_key.error();
        }
        internal::WrappedVideoKey rekeyed;
        internal::random_bytes(rekeyed.nonce);
        auto wrapped = internal::encrypt_xchacha20_poly1305(
            file_key.value(), std::span(row.package_id), rekeyed.nonce,
            new_wrapping_key.value());
        if (!wrapped || wrapped.value().size() != rekeyed.wrapped.size()) {
            if (!wrapped) {
                return wrapped.error();
            }
            return VaultError{VaultErrorCode::CryptoFailure,
                "re-wrapped file key has an invalid size"};
        }
        std::copy(wrapped.value().begin(), wrapped.value().end(), rekeyed.wrapped.begin());
        rewrapped.push_back({row.id, rekeyed});
    }

    // 1) Publish the new external metadata file first.
    auto metadata_written = internal::write_atomic_file(
        paths.metadata, new_metadata.value().encoded);
    if (!metadata_written) {
        return metadata_written.error();
    }
    // 2) Update the database metadata record and all wrapped keys.
    auto record_updated = impl.database_.update_vault_metadata_record(new_metadata.value());
    if (!record_updated) {
        return record_updated.error();
    }
    for (const auto& entry : rewrapped) {
        auto key_updated = impl.database_.update_video_key(entry.id, entry.key);
        if (!key_updated) {
            return key_updated.error();
        }
    }
    // 3) Re-encrypt the database file under the new SQLCipher key.
    auto rekeyed = impl.database_.rekey(new_database_key.value());
    if (!rekeyed) {
        return rekeyed.error();
    }

    // 4) The session continues under the new credentials.
    impl.master_key_ = std::move(new_master.value());
    impl.parameters_ = parameters;
    return true;
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
