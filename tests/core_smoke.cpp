#include "videovault/core/vault.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

class TemporaryVaultDirectory {
public:
    TemporaryVaultDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVideoProtect_Phase2_\u6d4b\u8bd5_" + suffix);
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ~TemporaryVaultDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

int fail(const char* message) {
    std::cerr << message << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main() {
    TemporaryVaultDirectory directory;
    const std::string password = "correct horse battery staple";

    videovault::core::Argon2Parameters testParameters;
    testParameters.memory_kib = 8U * 1024U;
    testParameters.iterations = 2U;
    testParameters.parallelism = 1U;

    auto created = videovault::core::Vault::create(
        directory.path(), password, testParameters);
    if (!created) {
        return fail("Creating a vault with valid parameters must succeed.");
    }
    if (!created.value().is_unlocked()) {
        return fail("A newly created vault must be unlocked.");
    }
    if (!std::filesystem::is_regular_file(directory.path() / L"vault.meta")
        || !std::filesystem::is_regular_file(directory.path() / L"vault.db")) {
        return fail("Vault creation must persist metadata and a SQLCipher database.");
    }

    created.value().lock();
    if (created.value().is_unlocked()) {
        return fail("Lock must transition the session to locked state.");
    }

    auto opened = videovault::core::Vault::open(directory.path(), password);
    if (!opened) {
        return fail("The correct password must open an existing vault.");
    }
    if (!opened.value().is_unlocked()) {
        return fail("A successfully opened vault must be unlocked.");
    }

    opened.value().lock();

    auto wrongPassword = videovault::core::Vault::open(
        directory.path(), "definitely the wrong password");
    if (wrongPassword
        || wrongPassword.error().code != videovault::core::VaultErrorCode::WrongPassword) {
        return fail("An incorrect password must return WrongPassword.");
    }

    auto validated = videovault::core::Vault::validate_password(
        directory.path(), "definitely the wrong password");
    if (!validated || validated.value()) {
        return fail("Password validation must return false for an incorrect password.");
    }

    const auto metadataPath = directory.path() / L"vault.meta";
    {
        std::fstream metadata(metadataPath, std::ios::binary | std::ios::in | std::ios::out);
        if (!metadata) {
            return fail("The verifier-tamper test could not open vault metadata.");
        }
        metadata.seekg(-1, std::ios::end);
        char byte = 0;
        metadata.read(&byte, 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
        metadata.seekp(-1, std::ios::end);
        metadata.write(&byte, 1);
        metadata.flush();
        if (!metadata) {
            return fail("The verifier-tamper test could not modify vault metadata.");
        }
    }

    auto tampered = videovault::core::Vault::open(directory.path(), password);
    if (tampered
        || tampered.error().code != videovault::core::VaultErrorCode::AuthenticationFailed) {
        return fail("A modified password verifier must return AuthenticationFailed.");
    }

    std::cout << "Vault create/open/lock and password-verifier checks succeeded.\n";
    return EXIT_SUCCESS;
}
