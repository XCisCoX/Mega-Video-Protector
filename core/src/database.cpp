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

} // namespace videovault::core::internal
