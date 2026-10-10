#include "internal/database.hpp"

#include <sqlcipher/sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace videovault::core::internal {
namespace {

class Statement final {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        status_ = sqlite3_prepare_v3(database, sql, -1, SQLITE_PREPARE_PERSISTENT, &statement_, nullptr);
    }
    ~Statement() {
        if (statement_ != nullptr) {
            (void)sqlite3_finalize(statement_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] int status() const noexcept { return status_; }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    [[nodiscard]] sqlite3* database() const noexcept { return database_; }

private:
    sqlite3* database_{nullptr};
    sqlite3_stmt* statement_{nullptr};
    int status_{SQLITE_ERROR};
};

std::string utf8_path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

VaultError database_error(
    sqlite3* database,
    const VaultErrorCode code,
    const std::string& operation,
    const int status) {
    std::string detail = operation + " failed (SQLite status " + std::to_string(status) + ")";
    if (database != nullptr) {
        const char* message = sqlite3_errmsg(database);
        if (message != nullptr) {
            detail += ": ";
            detail += message;
        }
    }
    return {code, std::move(detail)};
}

Result<bool> run(sqlite3* database, const char* sql) {
    Statement statement(database, sql);
    if (statement.status() != SQLITE_OK) {
        return database_error(database, VaultErrorCode::DatabaseFailure, "prepare statement", statement.status());
    }
    int status = SQLITE_OK;
    do {
        status = sqlite3_step(statement.get());
    } while (status == SQLITE_ROW);
    if (status != SQLITE_DONE) {
        return database_error(database, VaultErrorCode::DatabaseFailure, "execute statement", status);
    }
    return true;
}

Result<std::string> query_text(sqlite3* database, const char* sql) {
    Statement statement(database, sql);
    if (statement.status() != SQLITE_OK) {
        return database_error(database, VaultErrorCode::DatabaseFailure, "prepare text query", statement.status());
    }
    const int status = sqlite3_step(statement.get());
    if (status != SQLITE_ROW) {
        return database_error(database, VaultErrorCode::DatabaseFailure, "execute text query", status);
    }
    const auto* text = sqlite3_column_text(statement.get(), 0);
    const int bytes = sqlite3_column_bytes(statement.get(), 0);
    if (text == nullptr || bytes <= 0) {
        return VaultError{VaultErrorCode::DatabaseFailure, "SQLCipher version query returned no value"};
    }
    return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

Result<Database> open_keyed_database(
    const std::filesystem::path& path,
    const SensitiveBuffer& database_key,
    const int flags,
    const VaultErrorCode access_failure) {
    sqlite3* raw = nullptr;
    const auto path_string = utf8_path(path);
    int status = sqlite3_open_v2(path_string.c_str(), &raw, flags | SQLITE_OPEN_NOMUTEX, nullptr);
    if (status != SQLITE_OK) {
        VaultError error = database_error(raw, access_failure, "open SQLCipher database", status);
        if (raw != nullptr) {
            (void)sqlite3_close_v2(raw);
        }
        return error;
    }

    Database database(raw);
    (void)sqlite3_extended_result_codes(raw, 1);
    (void)sqlite3_busy_timeout(raw, 5000);
    status = sqlite3_key(raw, database_key.data(), static_cast<int>(database_key.size()));
    if (status != SQLITE_OK) {
        return database_error(raw, access_failure, "apply SQLCipher key", status);
    }

    constexpr const char* configuration[] = {
        "PRAGMA cipher_page_size = 4096;",
        "PRAGMA cipher_hmac_algorithm = HMAC_SHA512;",
        "PRAGMA cipher_kdf_algorithm = PBKDF2_HMAC_SHA512;",
        "PRAGMA kdf_iter = 256000;",
        "PRAGMA cipher_memory_security = ON;",
        "PRAGMA foreign_keys = ON;",
        "PRAGMA secure_delete = ON;"
    };
    for (const char* pragma : configuration) {
        auto configured = run(raw, pragma);
        if (!configured) {
            return database_error(raw, access_failure, "configure SQLCipher connection", sqlite3_extended_errcode(raw));
        }
    }

    auto cipher_version = query_text(raw, "PRAGMA cipher_version;");
    if (!cipher_version) {
        return VaultError{VaultErrorCode::DatabaseFailure, "linked SQLite library does not expose SQLCipher"};
    }
    return database;
}

Result<bool> bind_blob(
    const Statement& statement,
    const int index,
    const std::span<const unsigned char> bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database blob exceeds SQLite binding limit"};
    }
    const int status = sqlite3_bind_blob(
        statement.get(), index, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
    if (status != SQLITE_OK) {
        return database_error(statement.database(), VaultErrorCode::DatabaseFailure, "bind blob", status);
    }
    return true;
}

Result<bool> create_schema(sqlite3* database, const VaultMetadata& metadata) {
    auto transaction = run(database, "BEGIN IMMEDIATE;");
    if (!transaction) {
        return transaction.error();
    }

    const auto rollback = [database] { (void)run(database, "ROLLBACK;"); };
    constexpr const char* statements[] = {
        "CREATE TABLE schema_migrations ("
        "version INTEGER PRIMARY KEY NOT NULL, applied_at INTEGER NOT NULL);",
        "CREATE TABLE vault_metadata ("
        "singleton INTEGER PRIMARY KEY NOT NULL CHECK(singleton = 1),"
        "format_version INTEGER NOT NULL,"
        "external_metadata BLOB NOT NULL,"
        "verifier_nonce BLOB NOT NULL,"
        "verifier_ciphertext BLOB NOT NULL,"
        "created_at INTEGER NOT NULL);"
    };
    for (const char* sql : statements) {
        auto result = run(database, sql);
        if (!result) {
            rollback();
            return result.error();
        }
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    {
        Statement insert(database,
            "INSERT INTO schema_migrations(version, applied_at) VALUES(?1, ?2);");
        if (insert.status() != SQLITE_OK
            || sqlite3_bind_int(insert.get(), 1, 1) != SQLITE_OK
            || sqlite3_bind_int64(insert.get(), 2, now) != SQLITE_OK
            || sqlite3_step(insert.get()) != SQLITE_DONE) {
            const auto error = database_error(database, VaultErrorCode::DatabaseFailure,
                "insert schema migration", sqlite3_extended_errcode(database));
            rollback();
            return error;
        }
    }
    {
        Statement insert(database,
            "INSERT INTO vault_metadata("
            "singleton, format_version, external_metadata, verifier_nonce, verifier_ciphertext, created_at) "
            "VALUES(1, ?1, ?2, ?3, ?4, ?5);");
        if (insert.status() != SQLITE_OK
            || sqlite3_bind_int(insert.get(), 1, 1) != SQLITE_OK) {
            const auto error = database_error(database, VaultErrorCode::DatabaseFailure,
                "prepare vault metadata insert", sqlite3_extended_errcode(database));
            rollback();
            return error;
        }
        const auto all = std::span<const unsigned char>(metadata.encoded);
        auto bound_all = bind_blob(insert, 2, all);
        auto bound_nonce = bind_blob(insert, 3, all.subspan(kVaultHeaderSize, kVerifierNonceSize));
        auto bound_ciphertext = bind_blob(insert, 4,
            all.subspan(kVaultHeaderSize + kVerifierNonceSize, kVerifierCiphertextSize));
        if (!bound_all || !bound_nonce || !bound_ciphertext
            || sqlite3_bind_int64(insert.get(), 5, now) != SQLITE_OK
            || sqlite3_step(insert.get()) != SQLITE_DONE) {
            const auto error = database_error(database, VaultErrorCode::DatabaseFailure,
                "insert vault metadata", sqlite3_extended_errcode(database));
            rollback();
            return error;
        }
    }

    auto committed = run(database, "COMMIT;");
    if (!committed) {
        rollback();
        return committed.error();
    }
    return true;
}

std::vector<unsigned char> column_blob(sqlite3_stmt* statement, const int column) {
    const auto* data = static_cast<const unsigned char*>(sqlite3_column_blob(statement, column));
    const int size = sqlite3_column_bytes(statement, column);
    if (data == nullptr || size <= 0) {
        return {};
    }
    return {data, data + size};
}

Result<bool> migrate_to_version_2(sqlite3* database) {
    auto transaction = run(database, "BEGIN IMMEDIATE;");
    if (!transaction) {
        return transaction.error();
    }
    const auto rollback = [database] { (void)run(database, "ROLLBACK;"); };

    Statement current(database, "SELECT max(version) FROM schema_migrations;");
    if (current.status() != SQLITE_OK || sqlite3_step(current.get()) != SQLITE_ROW) {
        const auto error = database_error(database, VaultErrorCode::DatabaseCorrupt,
            "read current schema version", sqlite3_extended_errcode(database));
        rollback();
        return error;
    }
    const int version = sqlite3_column_int(current.get(), 0);
    if (version >= 2) {
        auto committed = run(database, "COMMIT;");
        if (!committed) {
            rollback();
            return committed.error();
        }
        return true;
    }

    constexpr const char* statements[] = {
        "CREATE TABLE videos ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "display_name TEXT NOT NULL,"
        "original_size INTEGER NOT NULL,"
        "package_relative_path TEXT NOT NULL UNIQUE,"
        "package_size INTEGER NOT NULL,"
        "package_sha256 BLOB NOT NULL,"
        "format_version INTEGER NOT NULL,"
        "algorithm_id INTEGER NOT NULL,"
        "chunk_size INTEGER NOT NULL,"
        "package_id BLOB NOT NULL,"
        "imported_at INTEGER NOT NULL);",
        "CREATE TABLE video_keys ("
        "video_id INTEGER PRIMARY KEY NOT NULL REFERENCES videos(id) ON DELETE CASCADE,"
        "wrapped_key BLOB NOT NULL,"
        "wrap_nonce BLOB NOT NULL);"
    };
    for (const char* sql : statements) {
        auto result = run(database, sql);
        if (!result) {
            rollback();
            return result.error();
        }
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Statement insert(database,
        "INSERT INTO schema_migrations(version, applied_at) VALUES(2, ?1);");
    if (insert.status() != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 1, now) != SQLITE_OK
        || sqlite3_step(insert.get()) != SQLITE_DONE) {
        const auto error = database_error(database, VaultErrorCode::DatabaseFailure,
            "insert schema migration 2", sqlite3_extended_errcode(database));
        rollback();
        return error;
    }

    auto committed = run(database, "COMMIT;");
    if (!committed) {
        rollback();
        return committed.error();
    }
    return true;
}

Result<bool> migrate_to_version_3(sqlite3* database) {
    auto transaction = run(database, "BEGIN IMMEDIATE;");
    if (!transaction) {
        return transaction.error();
    }
    const auto rollback = [database] { (void)run(database, "ROLLBACK;"); };

    Statement current(database, "SELECT max(version) FROM schema_migrations;");
    if (current.status() != SQLITE_OK || sqlite3_step(current.get()) != SQLITE_ROW) {
        const auto error = database_error(database, VaultErrorCode::DatabaseCorrupt,
            "read current schema version", sqlite3_extended_errcode(database));
        rollback();
        return error;
    }
    const int version = sqlite3_column_int(current.get(), 0);
    if (version >= 3) {
        auto committed = run(database, "COMMIT;");
        if (!committed) {
            rollback();
            return committed.error();
        }
        return true;
    }

    auto created = run(database,
        "CREATE TABLE thumbnails ("
        "video_id INTEGER PRIMARY KEY NOT NULL REFERENCES videos(id) ON DELETE CASCADE,"
        "mime TEXT NOT NULL,"
        "width INTEGER NOT NULL,"
        "height INTEGER NOT NULL,"
        "nonce BLOB NOT NULL,"
        "ciphertext BLOB NOT NULL,"
        "created_at INTEGER NOT NULL);");
    if (!created) {
        rollback();
        return created.error();
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Statement insert(database,
        "INSERT INTO schema_migrations(version, applied_at) VALUES(3, ?1);");
    if (insert.status() != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 1, now) != SQLITE_OK
        || sqlite3_step(insert.get()) != SQLITE_DONE) {
        const auto error = database_error(database, VaultErrorCode::DatabaseFailure,
            "insert schema migration 3", sqlite3_extended_errcode(database));
        rollback();
        return error;
    }

    auto committed = run(database, "COMMIT;");
    if (!committed) {
        rollback();
        return committed.error();
    }
    return true;
}

Result<bool> migrate_to_version_4(sqlite3* database) {
    auto transaction = run(database, "BEGIN IMMEDIATE;");
    if (!transaction) {
        return transaction.error();
    }
    const auto rollback = [database] { (void)run(database, "ROLLBACK;"); };

    Statement current(database, "SELECT max(version) FROM schema_migrations;");
    if (current.status() != SQLITE_OK || sqlite3_step(current.get()) != SQLITE_ROW) {
        const auto error = database_error(database, VaultErrorCode::DatabaseCorrupt,
            "read current schema version", sqlite3_extended_errcode(database));
        rollback();
        return error;
    }
    const int version = sqlite3_column_int(current.get(), 0);
    if (version >= 4) {
        auto committed = run(database, "COMMIT;");
        if (!committed) {
            rollback();
            return committed.error();
        }
        return true;
    }

    auto created_tags = run(database,
        "CREATE TABLE tags ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
        "created_at INTEGER NOT NULL);");
    if (!created_tags) {
        rollback();
        return created_tags.error();
    }
    auto created_video_tags = run(database,
        "CREATE TABLE video_tags ("
        "video_id INTEGER NOT NULL REFERENCES videos(id) ON DELETE CASCADE,"
        "tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,"
        "created_at INTEGER NOT NULL,"
        "PRIMARY KEY (video_id, tag_id));");
    if (!created_video_tags) {
        rollback();
        return created_video_tags.error();
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Statement insert(database,
        "INSERT INTO schema_migrations(version, applied_at) VALUES(4, ?1);");
    if (insert.status() != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 1, now) != SQLITE_OK
        || sqlite3_step(insert.get()) != SQLITE_DONE) {
        const auto error = database_error(database, VaultErrorCode::DatabaseFailure,
            "insert schema migration 4", sqlite3_extended_errcode(database));
        rollback();
        return error;
    }

    auto committed = run(database, "COMMIT;");
    if (!committed) {
        rollback();
        return committed.error();
    }
    return true;
}

Result<bool> migrate_to_version_5(sqlite3* database) {
    auto transaction = run(database, "BEGIN IMMEDIATE;");
    if (!transaction) {
        return transaction.error();
    }
    const auto rollback = [database] { (void)run(database, "ROLLBACK;"); };

    Statement current(database, "SELECT max(version) FROM schema_migrations;");
    if (current.status() != SQLITE_OK || sqlite3_step(current.get()) != SQLITE_ROW) {
        const auto error = database_error(database, VaultErrorCode::DatabaseCorrupt,
            "read current schema version", sqlite3_extended_errcode(database));
        rollback();
        return error;
    }
    const int version = sqlite3_column_int(current.get(), 0);
    if (version >= 5) {
        auto committed = run(database, "COMMIT;");
        if (!committed) {
            rollback();
            return committed.error();
        }
        return true;
    }

    // parent_id NULL is the library root. The unique index uses IFNULL so two
    // folders at the root cannot share a name (NULL would not collide).
    auto created = run(database,
        "CREATE TABLE folders ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "parent_id INTEGER REFERENCES folders(id) ON DELETE CASCADE,"
        "name TEXT NOT NULL,"
        "created_at INTEGER NOT NULL);");
    if (!created) {
        rollback();
        return created.error();
    }
    auto indexed = run(database,
        "CREATE UNIQUE INDEX folders_name_in_parent"
        " ON folders(IFNULL(parent_id, 0), name COLLATE NOCASE);");
    if (!indexed) {
        rollback();
        return indexed.error();
    }
    auto added = run(database,
        "ALTER TABLE videos ADD COLUMN folder_id INTEGER"
        " REFERENCES folders(id) ON DELETE SET NULL;");
    if (!added) {
        rollback();
        return added.error();
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Statement insert(database,
        "INSERT INTO schema_migrations(version, applied_at) VALUES(5, ?1);");
    if (insert.status() != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 1, now) != SQLITE_OK
        || sqlite3_step(insert.get()) != SQLITE_DONE) {
        const auto error = database_error(database, VaultErrorCode::DatabaseFailure,
            "insert schema migration 5", sqlite3_extended_errcode(database));
        rollback();
        return error;
    }

    auto committed = run(database, "COMMIT;");
    if (!committed) {
        rollback();
        return committed.error();
    }
    return true;
}

VideoRow row_from_statement(sqlite3_stmt* statement) {
    VideoRow row;
    row.id = sqlite3_column_int64(statement, 0);
    if (const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        name != nullptr) {
        row.display_name.assign(name, static_cast<std::size_t>(sqlite3_column_bytes(statement, 1)));
    }
    row.original_size = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 2));
    if (const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
        path != nullptr) {
        row.package_relative_path.assign(path,
            static_cast<std::size_t>(sqlite3_column_bytes(statement, 3)));
    }
    row.package_size = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 4));
    const auto sha = column_blob(statement, 5);
    if (sha.size() == row.package_sha256.size()) {
        std::copy(sha.begin(), sha.end(), row.package_sha256.begin());
    }
    row.format_version = static_cast<std::uint32_t>(sqlite3_column_int(statement, 6));
    row.algorithm_id = static_cast<std::uint32_t>(sqlite3_column_int(statement, 7));
    row.chunk_size = static_cast<std::uint32_t>(sqlite3_column_int(statement, 8));
    const auto id_bytes = column_blob(statement, 9);
    if (id_bytes.size() == row.package_id.size()) {
        std::copy(id_bytes.begin(), id_bytes.end(), row.package_id.begin());
    }
    row.imported_at = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 10));
    row.folder_id = sqlite3_column_type(statement, 11) == SQLITE_NULL
        ? 0
        : sqlite3_column_int64(statement, 11);
    return row;
}

constexpr const char* kVideoColumns =
    "id, display_name, original_size, package_relative_path, package_size,"
    " package_sha256, format_version, algorithm_id, chunk_size, package_id,"
    " imported_at, folder_id";

} // namespace

Database::~Database() {
    close();
}

Database::Database(Database&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        close();
        database_ = std::exchange(other.database_, nullptr);
    }
    return *this;
}

void Database::close() noexcept {
    if (database_ != nullptr) {
        (void)sqlite3_db_release_memory(database_);
        (void)sqlite3_close_v2(database_);
        database_ = nullptr;
    }
}

Result<Database> Database::create(
    const std::filesystem::path& path,
    const SensitiveBuffer& database_key,
    const VaultMetadata& metadata) {
    auto database = open_keyed_database(
        path, database_key, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        VaultErrorCode::DatabaseFailure);
    if (!database) {
        return database.error();
    }
    auto schema = create_schema(database.value().database_, metadata);
    if (!schema) {
        return schema.error();
    }
    auto migrated_v2 = migrate_to_version_2(database.value().database_);
    if (!migrated_v2) {
        return migrated_v2.error();
    }
    auto migrated_v3 = migrate_to_version_3(database.value().database_);
    if (!migrated_v3) {
        return migrated_v3.error();
    }
    auto migrated_v4 = migrate_to_version_4(database.value().database_);
    if (!migrated_v4) {
        return migrated_v4.error();
    }
    auto migrated_v5 = migrate_to_version_5(database.value().database_);
    if (!migrated_v5) {
        return migrated_v5.error();
    }
    return std::move(database.value());
}

Result<Database> Database::open(
    const std::filesystem::path& path,
    const SensitiveBuffer& database_key) {
    auto database = open_keyed_database(
        path, database_key, SQLITE_OPEN_READWRITE, VaultErrorCode::DatabaseCorrupt);
    if (!database) {
        return database.error();
    }
    auto accessible = run(database.value().database_, "SELECT count(*) FROM sqlite_schema;");
    if (!accessible) {
        return database_error(database.value().database_, VaultErrorCode::DatabaseCorrupt,
            "validate encrypted database", sqlite3_extended_errcode(database.value().database_));
    }
    auto migrated_v2 = migrate_to_version_2(database.value().database_);
    if (!migrated_v2) {
        return migrated_v2.error();
    }
    auto migrated_v3 = migrate_to_version_3(database.value().database_);
    if (!migrated_v3) {
        return migrated_v3.error();
    }
    auto migrated_v4 = migrate_to_version_4(database.value().database_);
    if (!migrated_v4) {
        return migrated_v4.error();
    }
    auto migrated_v5 = migrate_to_version_5(database.value().database_);
    if (!migrated_v5) {
        return migrated_v5.error();
    }
    return std::move(database.value());
}

Result<bool> Database::verify_vault_metadata(
    const VaultMetadata& metadata,
    const SensitiveBuffer& verifier_key) const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }

    Statement query(database_,
        "SELECT external_metadata, verifier_nonce, verifier_ciphertext "
        "FROM vault_metadata WHERE singleton = 1;");
    if (query.status() != SQLITE_OK || sqlite3_step(query.get()) != SQLITE_ROW) {
        return database_error(database_, VaultErrorCode::DatabaseCorrupt,
            "read vault metadata record", sqlite3_extended_errcode(database_));
    }
    const auto external = column_blob(query.get(), 0);
    const auto nonce = column_blob(query.get(), 1);
    const auto ciphertext = column_blob(query.get(), 2);
    if (!constant_time_equal(external, metadata.encoded)
        || nonce.size() != kVerifierNonceSize
        || ciphertext.size() != kVerifierCiphertextSize) {
        return VaultError{VaultErrorCode::MetadataCorrupt,
            "encrypted database metadata does not match the external metadata"};
    }

    VaultMetadata database_record = metadata;
    std::copy(nonce.begin(), nonce.end(), database_record.encoded.begin() + kVaultHeaderSize);
    std::copy(ciphertext.begin(), ciphertext.end(),
        database_record.encoded.begin() + kVaultHeaderSize + kVerifierNonceSize);
    return verify_password_verifier(
        database_record, verifier_key, VaultErrorCode::AuthenticationFailed);
}

Result<std::int64_t> Database::insert_video(
    const VideoRow& row,
    const WrappedVideoKey& key) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    auto transaction = run(database_, "BEGIN IMMEDIATE;");
    if (!transaction) {
        return transaction.error();
    }
    const auto rollback = [this] { (void)run(database_, "ROLLBACK;"); };

    Statement insert(database_,
        "INSERT INTO videos(display_name, original_size, package_relative_path,"
        " package_size, package_sha256, format_version, algorithm_id, chunk_size,"
        " package_id, imported_at, folder_id)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);");
    if (insert.status() != SQLITE_OK
        || sqlite3_bind_text(insert.get(), 1, row.display_name.c_str(),
               static_cast<int>(row.display_name.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 2,
               static_cast<sqlite3_int64>(row.original_size)) != SQLITE_OK
        || sqlite3_bind_text(insert.get(), 3, row.package_relative_path.c_str(),
               static_cast<int>(row.package_relative_path.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 4,
               static_cast<sqlite3_int64>(row.package_size)) != SQLITE_OK) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video insert", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }
    const auto bound_sha = bind_blob(insert, 5, row.package_sha256);
    if (!bound_sha
        || sqlite3_bind_int(insert.get(), 6, static_cast<int>(row.format_version)) != SQLITE_OK
        || sqlite3_bind_int(insert.get(), 7, static_cast<int>(row.algorithm_id)) != SQLITE_OK
        || sqlite3_bind_int(insert.get(), 8, static_cast<int>(row.chunk_size)) != SQLITE_OK) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "bind video insert", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }
    const auto bound_id = bind_blob(insert, 9, row.package_id);
    if (!bound_id
        || sqlite3_bind_int64(insert.get(), 10,
               static_cast<sqlite3_int64>(row.imported_at)) != SQLITE_OK
        || (row.folder_id > 0
            ? sqlite3_bind_int64(insert.get(), 11, row.folder_id)
            : sqlite3_bind_null(insert.get(), 11)) != SQLITE_OK
        || sqlite3_step(insert.get()) != SQLITE_DONE) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute video insert", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }
    const auto video_id = sqlite3_last_insert_rowid(database_);

    Statement key_insert(database_,
        "INSERT INTO video_keys(video_id, wrapped_key, wrap_nonce) VALUES(?1, ?2, ?3);");
    if (key_insert.status() != SQLITE_OK
        || sqlite3_bind_int64(key_insert.get(), 1, video_id) != SQLITE_OK) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video key insert", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }
    const auto bound_key = bind_blob(key_insert, 2, key.wrapped);
    const auto bound_nonce = bind_blob(key_insert, 3, key.nonce);
    if (!bound_key || !bound_nonce || sqlite3_step(key_insert.get()) != SQLITE_DONE) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute video key insert", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }

    auto committed = run(database_, "COMMIT;");
    if (!committed) {
        rollback();
        return committed.error();
    }
    return video_id;
}

Result<std::vector<VideoRow>> Database::list_videos() const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    const std::string sql =
        std::string("SELECT ") + kVideoColumns + " FROM videos ORDER BY imported_at, id;";
    Statement query(database_, sql.c_str());
    if (query.status() != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video listing", sqlite3_extended_errcode(database_));
    }
    std::vector<VideoRow> rows;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(query.get())) == SQLITE_ROW) {
        rows.push_back(row_from_statement(query.get()));
    }
    if (status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute video listing", sqlite3_extended_errcode(database_));
    }
    for (auto& row : rows) {
        auto tags = tags_for_video(row.id);
        if (!tags) {
            return tags.error();
        }
        row.tag_names.reserve(tags.value().size());
        for (const auto& tag : tags.value()) {
            row.tag_names.push_back(tag.name);
        }
    }
    return rows;
}

Result<VideoRow> Database::query_video(const std::int64_t video_id) const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    const std::string sql =
        std::string("SELECT ") + kVideoColumns + " FROM videos WHERE id = ?1;";
    Statement query(database_, sql.c_str());
    if (query.status() != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 1, video_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video query", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        return VaultError{VaultErrorCode::InvalidArgument, "no video with the given id"};
    }
    return row_from_statement(query.get());
}

Result<std::optional<VideoRow>> Database::query_video_by_sha256(
    const Sha256Digest& package_sha256) const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    const std::string sql = std::string("SELECT ") + kVideoColumns
        + " FROM videos WHERE package_sha256 = ?1 LIMIT 1;";
    Statement query(database_, sql.c_str());
    if (query.status() != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare sha256 query", sqlite3_extended_errcode(database_));
    }
    const auto bound = bind_blob(query, 1, package_sha256);
    if (!bound) {
        return bound.error();
    }
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        return std::optional<VideoRow>{};
    }
    return std::optional<VideoRow>{row_from_statement(query.get())};
}

Result<WrappedVideoKey> Database::query_video_key(const std::int64_t video_id) const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement query(database_,
        "SELECT wrapped_key, wrap_nonce FROM video_keys WHERE video_id = ?1;");
    if (query.status() != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 1, video_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video key query", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "no wrapped key exists for the given video"};
    }
    const auto wrapped = column_blob(query.get(), 0);
    const auto nonce = column_blob(query.get(), 1);
    WrappedVideoKey key;
    if (wrapped.size() != key.wrapped.size() || nonce.size() != key.nonce.size()) {
        return VaultError{VaultErrorCode::DatabaseCorrupt,
            "wrapped video key record has an invalid size"};
    }
    std::copy(wrapped.begin(), wrapped.end(), key.wrapped.begin());
    std::copy(nonce.begin(), nonce.end(), key.nonce.begin());
    return key;
}

Result<bool> Database::delete_video(const std::int64_t video_id) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    auto transaction = run(database_, "BEGIN IMMEDIATE;");
    if (!transaction) {
        return transaction.error();
    }
    const auto rollback = [this] { (void)run(database_, "ROLLBACK;"); };
    Statement remove(database_, "DELETE FROM videos WHERE id = ?1;");
    if (remove.status() != SQLITE_OK
        || sqlite3_bind_int64(remove.get(), 1, video_id) != SQLITE_OK) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video delete", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }
    if (sqlite3_step(remove.get()) != SQLITE_DONE) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute video delete", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }
    if (sqlite3_changes(database_) == 0) {
        rollback();
        return VaultError{VaultErrorCode::InvalidArgument, "no video with the given id"};
    }
    auto committed = run(database_, "COMMIT;");
    if (!committed) {
        rollback();
        return committed.error();
    }
    return true;
}

Result<bool> Database::update_video_key(
    const std::int64_t video_id,
    const WrappedVideoKey& key) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement update(database_,
        "UPDATE video_keys SET wrapped_key = ?2, wrap_nonce = ?3 WHERE video_id = ?1;");
    if (update.status() != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 1, video_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video key update", sqlite3_extended_errcode(database_));
    }
    const auto bound_key = bind_blob(update, 2, key.wrapped);
    const auto bound_nonce = bind_blob(update, 3, key.nonce);
    if (!bound_key || !bound_nonce || sqlite3_step(update.get()) != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute video key update", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_changes(database_) == 0) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "no wrapped key exists for the given video"};
    }
    return true;
}

Result<bool> Database::update_vault_metadata_record(const VaultMetadata& metadata) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement update(database_,
        "UPDATE vault_metadata SET external_metadata = ?1, verifier_nonce = ?2,"
        " verifier_ciphertext = ?3 WHERE singleton = 1;");
    if (update.status() != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare vault metadata update", sqlite3_extended_errcode(database_));
    }
    const auto all = std::span<const unsigned char>(metadata.encoded);
    const auto bound_all = bind_blob(update, 1, all);
    const auto bound_nonce = bind_blob(update, 2, all.subspan(kVaultHeaderSize, kVerifierNonceSize));
    const auto bound_ciphertext = bind_blob(update, 3,
        all.subspan(kVaultHeaderSize + kVerifierNonceSize, kVerifierCiphertextSize));
    if (!bound_all || !bound_nonce || !bound_ciphertext
        || sqlite3_step(update.get()) != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute vault metadata update", sqlite3_extended_errcode(database_));
    }
    return true;
}

Result<bool> Database::rekey(const SensitiveBuffer& new_database_key) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    if (new_database_key.size() != 32U) {
        return VaultError{VaultErrorCode::CryptoFailure, "invalid SQLCipher key length"};
    }
    const int status = sqlite3_rekey(
        database_, new_database_key.data(), static_cast<int>(new_database_key.size()));
    if (status != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "rekey SQLCipher database", sqlite3_extended_errcode(database_));
    }
    return true;
}

Result<bool> Database::insert_thumbnail(
    const std::int64_t video_id,
    const ThumbnailRow& row) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement insert(database_,
        "INSERT OR REPLACE INTO thumbnails("
        "video_id, mime, width, height, nonce, ciphertext, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);");
    if (insert.status() != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 1, video_id) != SQLITE_OK
        || sqlite3_bind_text(insert.get(), 2, row.mime.c_str(),
               static_cast<int>(row.mime.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int(insert.get(), 3, static_cast<int>(row.width)) != SQLITE_OK
        || sqlite3_bind_int(insert.get(), 4, static_cast<int>(row.height)) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare thumbnail insert", sqlite3_extended_errcode(database_));
    }
    const auto bound_nonce = bind_blob(insert, 5, row.nonce);
    const auto bound_ciphertext = bind_blob(insert, 6, row.ciphertext);
    if (!bound_nonce || !bound_ciphertext
        || sqlite3_bind_int64(insert.get(), 7,
               static_cast<sqlite3_int64>(row.created_at)) != SQLITE_OK
        || sqlite3_step(insert.get()) != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute thumbnail insert", sqlite3_extended_errcode(database_));
    }
    return true;
}

Result<ThumbnailRow> Database::query_thumbnail(const std::int64_t video_id) const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement query(database_,
        "SELECT mime, width, height, nonce, ciphertext, created_at"
        " FROM thumbnails WHERE video_id = ?1;");
    if (query.status() != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 1, video_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare thumbnail query", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "no thumbnail exists for the given video"};
    }
    ThumbnailRow row;
    if (const auto* mime = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 0));
        mime != nullptr) {
        row.mime.assign(mime, static_cast<std::size_t>(sqlite3_column_bytes(query.get(), 0)));
    }
    row.width = static_cast<std::uint32_t>(sqlite3_column_int(query.get(), 1));
    row.height = static_cast<std::uint32_t>(sqlite3_column_int(query.get(), 2));
    const auto nonce = column_blob(query.get(), 3);
    if (nonce.size() == row.nonce.size()) {
        std::copy(nonce.begin(), nonce.end(), row.nonce.begin());
    }
    row.ciphertext = column_blob(query.get(), 4);
    row.created_at = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 5));
    return row;
}

Result<std::vector<std::pair<std::int64_t, ThumbnailRow>>>
Database::list_thumbnails() const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement query(database_,
        "SELECT video_id, mime, width, height, nonce, ciphertext, created_at"
        " FROM thumbnails ORDER BY video_id;");
    if (query.status() != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare thumbnail listing", sqlite3_extended_errcode(database_));
    }
    std::vector<std::pair<std::int64_t, ThumbnailRow>> result;
    while (true) {
        const int step = sqlite3_step(query.get());
        if (step == SQLITE_DONE) {
            break;
        }
        if (step != SQLITE_ROW) {
            return database_error(database_, VaultErrorCode::DatabaseFailure,
                "read thumbnail listing", sqlite3_extended_errcode(database_));
        }
        std::pair<std::int64_t, ThumbnailRow> entry;
        entry.first = sqlite3_column_int64(query.get(), 0);
        ThumbnailRow& row = entry.second;
        if (const auto* mime = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1));
            mime != nullptr) {
            row.mime.assign(mime, static_cast<std::size_t>(sqlite3_column_bytes(query.get(), 1)));
        }
        row.width = static_cast<std::uint32_t>(sqlite3_column_int(query.get(), 2));
        row.height = static_cast<std::uint32_t>(sqlite3_column_int(query.get(), 3));
        const auto nonce = column_blob(query.get(), 4);
        if (nonce.size() == row.nonce.size()) {
            std::copy(nonce.begin(), nonce.end(), row.nonce.begin());
        }
        row.ciphertext = column_blob(query.get(), 5);
        row.created_at = static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 6));
        result.push_back(std::move(entry));
    }
    return result;
}

Result<std::int64_t> Database::ensure_tag(const std::string& name) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Statement insert(database_,
        "INSERT OR IGNORE INTO tags(name, created_at) VALUES(?1, ?2);");
    if (insert.status() != SQLITE_OK
        || sqlite3_bind_text(insert.get(), 1, name.c_str(),
               static_cast<int>(name.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 2, now) != SQLITE_OK
        || sqlite3_step(insert.get()) != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "insert tag", sqlite3_extended_errcode(database_));
    }
    Statement query(database_,
        "SELECT id FROM tags WHERE name = ?1 COLLATE NOCASE;");
    if (query.status() != SQLITE_OK
        || sqlite3_bind_text(query.get(), 1, name.c_str(),
               static_cast<int>(name.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "resolve tag id", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        return VaultError{VaultErrorCode::DatabaseFailure, "tag was not created"};
    }
    return sqlite3_column_int64(query.get(), 0);
}

Result<bool> Database::tag_video(
    const std::int64_t video_id,
    const std::int64_t tag_id) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Statement insert(database_,
        "INSERT OR IGNORE INTO video_tags(video_id, tag_id, created_at)"
        " VALUES(?1, ?2, ?3);");
    if (insert.status() != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 1, video_id) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 2, tag_id) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 3, now) != SQLITE_OK
        || sqlite3_step(insert.get()) != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "associate tag with video", sqlite3_extended_errcode(database_));
    }
    return true;
}

Result<bool> Database::untag_video(
    const std::int64_t video_id,
    const std::int64_t tag_id) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement remove(database_,
        "DELETE FROM video_tags WHERE video_id = ?1 AND tag_id = ?2;");
    if (remove.status() != SQLITE_OK
        || sqlite3_bind_int64(remove.get(), 1, video_id) != SQLITE_OK
        || sqlite3_bind_int64(remove.get(), 2, tag_id) != SQLITE_OK
        || sqlite3_step(remove.get()) != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "remove video tag association", sqlite3_extended_errcode(database_));
    }
    return true;
}

Result<std::int64_t> Database::rename_tag(
    const std::int64_t tag_id,
    const std::string& new_name) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    // The UNIQUE COLLATE NOCASE constraint on tags.name rejects collisions;
    // translate that into a clean InvalidArgument.
    Statement update(database_, "UPDATE tags SET name = ?1 WHERE id = ?2;");
    if (update.status() != SQLITE_OK
        || sqlite3_bind_text(update.get(), 1, new_name.c_str(),
               static_cast<int>(new_name.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, tag_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "rename tag", sqlite3_extended_errcode(database_));
    }
    const int step_status = sqlite3_step(update.get());
    if (step_status == SQLITE_CONSTRAINT || step_status == SQLITE_CONSTRAINT_UNIQUE) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "another tag already has this name"};
    }
    if (step_status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "rename tag", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_changes(database_) == 0) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown tag"};
    }
    return tag_id;
}

Result<bool> Database::delete_tag(const std::int64_t tag_id) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement remove(database_, "DELETE FROM tags WHERE id = ?1;");
    if (remove.status() != SQLITE_OK
        || sqlite3_bind_int64(remove.get(), 1, tag_id) != SQLITE_OK
        || sqlite3_step(remove.get()) != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "delete tag", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_changes(database_) == 0) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown tag"};
    }
    return true;
}

Result<std::vector<TagRow>> Database::tags_for_video(
    const std::int64_t video_id) const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement query(database_,
        "SELECT t.id, t.name, 0 FROM tags t"
        " JOIN video_tags vt ON vt.tag_id = t.id"
        " WHERE vt.video_id = ?1"
        " ORDER BY t.name COLLATE NOCASE;");
    if (query.status() != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 1, video_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video tag listing", sqlite3_extended_errcode(database_));
    }
    std::vector<TagRow> tags;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(query.get())) == SQLITE_ROW) {
        TagRow tag;
        tag.id = sqlite3_column_int64(query.get(), 0);
        if (const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1));
            name != nullptr) {
            tag.name.assign(name, static_cast<std::size_t>(sqlite3_column_bytes(query.get(), 1)));
        }
        tags.push_back(std::move(tag));
    }
    if (status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute video tag listing", sqlite3_extended_errcode(database_));
    }
    return tags;
}

Result<std::vector<TagRow>> Database::list_tags() const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement query(database_,
        "SELECT t.id, t.name, count(vt.video_id)"
        " FROM tags t LEFT JOIN video_tags vt ON vt.tag_id = t.id"
        " GROUP BY t.id ORDER BY t.name COLLATE NOCASE;");
    if (query.status() != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare tag listing", sqlite3_extended_errcode(database_));
    }
    std::vector<TagRow> tags;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(query.get())) == SQLITE_ROW) {
        TagRow tag;
        tag.id = sqlite3_column_int64(query.get(), 0);
        if (const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1));
            name != nullptr) {
            tag.name.assign(name, static_cast<std::size_t>(sqlite3_column_bytes(query.get(), 1)));
        }
        tag.video_count = sqlite3_column_int64(query.get(), 2);
        tags.push_back(std::move(tag));
    }
    if (status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute tag listing", sqlite3_extended_errcode(database_));
    }
    return tags;
}

namespace {

FolderRow folder_from_statement(sqlite3_stmt* statement) {
    FolderRow row;
    row.id = sqlite3_column_int64(statement, 0);
    row.parent_id = sqlite3_column_type(statement, 1) == SQLITE_NULL
        ? 0
        : sqlite3_column_int64(statement, 1);
    if (const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
        name != nullptr) {
        row.name.assign(name, static_cast<std::size_t>(sqlite3_column_bytes(statement, 2)));
    }
    row.created_at = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 3));
    return row;
}

} // namespace

Result<std::vector<FolderRow>> Database::list_folders() const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement query(database_,
        "SELECT id, parent_id, name, created_at FROM folders"
        " ORDER BY name COLLATE NOCASE;");
    if (query.status() != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare folder listing", sqlite3_extended_errcode(database_));
    }
    std::vector<FolderRow> folders;
    int status = SQLITE_OK;
    while ((status = sqlite3_step(query.get())) == SQLITE_ROW) {
        folders.push_back(folder_from_statement(query.get()));
    }
    if (status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute folder listing", sqlite3_extended_errcode(database_));
    }
    return folders;
}

Result<FolderRow> Database::query_folder(const std::int64_t folder_id) const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement query(database_,
        "SELECT id, parent_id, name, created_at FROM folders WHERE id = ?1;");
    if (query.status() != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 1, folder_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare folder query", sqlite3_extended_errcode(database_));
    }
    const int status = sqlite3_step(query.get());
    if (status == SQLITE_DONE) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown folder"};
    }
    if (status != SQLITE_ROW) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute folder query", sqlite3_extended_errcode(database_));
    }
    return folder_from_statement(query.get());
}

Result<bool> Database::name_taken(
    const std::int64_t parent_id,
    const std::string& name,
    const std::int64_t except_folder_id,
    const std::int64_t except_video_id) const {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement query(database_,
        "SELECT 1 FROM folders"
        " WHERE IFNULL(parent_id, 0) = ?1 AND name = ?2 COLLATE NOCASE AND id != ?3"
        " UNION SELECT 1 FROM videos"
        " WHERE IFNULL(folder_id, 0) = ?1 AND display_name = ?2 COLLATE NOCASE AND id != ?4"
        " LIMIT 1;");
    if (query.status() != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 1, parent_id) != SQLITE_OK
        || sqlite3_bind_text(query.get(), 2, name.c_str(),
               static_cast<int>(name.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 3, except_folder_id) != SQLITE_OK
        || sqlite3_bind_int64(query.get(), 4, except_video_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare name check", sqlite3_extended_errcode(database_));
    }
    const int status = sqlite3_step(query.get());
    if (status != SQLITE_ROW && status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "execute name check", sqlite3_extended_errcode(database_));
    }
    return status == SQLITE_ROW;
}

Result<std::int64_t> Database::insert_folder(
    const std::int64_t parent_id,
    const std::string& name) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Statement insert(database_,
        "INSERT INTO folders(parent_id, name, created_at) VALUES(?1, ?2, ?3);");
    if (insert.status() != SQLITE_OK
        || (parent_id > 0
            ? sqlite3_bind_int64(insert.get(), 1, parent_id)
            : sqlite3_bind_null(insert.get(), 1)) != SQLITE_OK
        || sqlite3_bind_text(insert.get(), 2, name.c_str(),
               static_cast<int>(name.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int64(insert.get(), 3, now) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare folder insert", sqlite3_extended_errcode(database_));
    }
    const int status = sqlite3_step(insert.get());
    if (status == SQLITE_CONSTRAINT || status == SQLITE_CONSTRAINT_UNIQUE
        || status == SQLITE_CONSTRAINT_FOREIGNKEY) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "a folder or file with this name is already here"};
    }
    if (status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "insert folder", sqlite3_extended_errcode(database_));
    }
    return sqlite3_last_insert_rowid(database_);
}

Result<bool> Database::rename_folder_row(
    const std::int64_t folder_id,
    const std::string& name) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement update(database_, "UPDATE folders SET name = ?1 WHERE id = ?2;");
    if (update.status() != SQLITE_OK
        || sqlite3_bind_text(update.get(), 1, name.c_str(),
               static_cast<int>(name.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, folder_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare folder rename", sqlite3_extended_errcode(database_));
    }
    const int status = sqlite3_step(update.get());
    if (status == SQLITE_CONSTRAINT || status == SQLITE_CONSTRAINT_UNIQUE) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "a folder or file with this name is already here"};
    }
    if (status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "rename folder", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_changes(database_) == 0) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown folder"};
    }
    return true;
}

Result<bool> Database::set_folder_parent(
    const std::int64_t folder_id,
    const std::int64_t parent_id) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement update(database_, "UPDATE folders SET parent_id = ?1 WHERE id = ?2;");
    if (update.status() != SQLITE_OK
        || (parent_id > 0
            ? sqlite3_bind_int64(update.get(), 1, parent_id)
            : sqlite3_bind_null(update.get(), 1)) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, folder_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare folder move", sqlite3_extended_errcode(database_));
    }
    const int status = sqlite3_step(update.get());
    if (status == SQLITE_CONSTRAINT || status == SQLITE_CONSTRAINT_UNIQUE
        || status == SQLITE_CONSTRAINT_FOREIGNKEY) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "a folder or file with this name is already there"};
    }
    if (status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "move folder", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_changes(database_) == 0) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown folder"};
    }
    return true;
}

Result<bool> Database::remove_folder_row(const std::int64_t folder_id) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    auto all = list_folders();
    if (!all) {
        return all.error();
    }
    const FolderRow* self = nullptr;
    for (const auto& folder : all.value()) {
        if (folder.id == folder_id) {
            self = &folder;
            break;
        }
    }
    if (self == nullptr) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown folder"};
    }
    const std::int64_t parent_id = self->parent_id;
    std::vector<std::int64_t> subtree;
    subtree.push_back(folder_id);
    for (std::size_t index = 0; index < subtree.size(); ++index) {
        for (const auto& folder : all.value()) {
            if (folder.parent_id == subtree[index]) {
                subtree.push_back(folder.id);
            }
        }
    }

    auto transaction = run(database_, "BEGIN IMMEDIATE;");
    if (!transaction) {
        return transaction.error();
    }
    const auto rollback = [this] { (void)run(database_, "ROLLBACK;"); };

    std::string id_list;
    for (std::size_t index = 0; index < subtree.size(); ++index) {
        if (index > 0U) {
            id_list += ',';
        }
        id_list += std::to_string(subtree[index]);
    }
    const std::string sql = parent_id > 0
        ? "UPDATE videos SET folder_id = ?1 WHERE folder_id IN (" + id_list + ");"
        : "UPDATE videos SET folder_id = NULL WHERE folder_id IN (" + id_list + ");";
    Statement update(database_, sql.c_str());
    if (update.status() != SQLITE_OK
        || (parent_id > 0 && sqlite3_bind_int64(update.get(), 1, parent_id) != SQLITE_OK)
        || sqlite3_step(update.get()) != SQLITE_DONE) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "move videos out of folder", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }

    Statement remove(database_, "DELETE FROM folders WHERE id = ?1;");
    if (remove.status() != SQLITE_OK
        || sqlite3_bind_int64(remove.get(), 1, folder_id) != SQLITE_OK
        || sqlite3_step(remove.get()) != SQLITE_DONE) {
        const auto error = database_error(database_, VaultErrorCode::DatabaseFailure,
            "delete folder", sqlite3_extended_errcode(database_));
        rollback();
        return error;
    }

    auto committed = run(database_, "COMMIT;");
    if (!committed) {
        rollback();
        return committed.error();
    }
    return true;
}

Result<bool> Database::rename_video_row(
    const std::int64_t video_id,
    const std::string& name) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement update(database_, "UPDATE videos SET display_name = ?1 WHERE id = ?2;");
    if (update.status() != SQLITE_OK
        || sqlite3_bind_text(update.get(), 1, name.c_str(),
               static_cast<int>(name.size()), SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, video_id) != SQLITE_OK
        || sqlite3_step(update.get()) != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "rename video", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_changes(database_) == 0) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown video"};
    }
    return true;
}

Result<bool> Database::set_video_folder(
    const std::int64_t video_id,
    const std::int64_t folder_id) {
    if (database_ == nullptr) {
        return VaultError{VaultErrorCode::DatabaseFailure, "database is closed"};
    }
    Statement update(database_, "UPDATE videos SET folder_id = ?1 WHERE id = ?2;");
    if (update.status() != SQLITE_OK
        || (folder_id > 0
            ? sqlite3_bind_int64(update.get(), 1, folder_id)
            : sqlite3_bind_null(update.get(), 1)) != SQLITE_OK
        || sqlite3_bind_int64(update.get(), 2, video_id) != SQLITE_OK) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "prepare video move", sqlite3_extended_errcode(database_));
    }
    const int status = sqlite3_step(update.get());
    if (status == SQLITE_CONSTRAINT || status == SQLITE_CONSTRAINT_FOREIGNKEY) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown folder"};
    }
    if (status != SQLITE_DONE) {
        return database_error(database_, VaultErrorCode::DatabaseFailure,
            "move video", sqlite3_extended_errcode(database_));
    }
    if (sqlite3_changes(database_) == 0) {
        return VaultError{VaultErrorCode::InvalidArgument, "unknown video"};
    }
    return true;
}

} // namespace videovault::core::internal
