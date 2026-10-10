#include "videovault/core/vault.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path()
            / (L"MegaVaultProtect_Phase4_" + suffix);
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }

    ~TemporaryDirectory() {
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

videovault::core::Argon2Parameters testParameters() {
    videovault::core::Argon2Parameters parameters;
    parameters.memory_kib = 8U * 1024U;
    parameters.iterations = 2U;
    parameters.parallelism = 1U;
    return parameters;
}

void writeBinaryFile(
    const std::filesystem::path& path,
    const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<unsigned char> deterministicBytes(const std::size_t size) {
    std::vector<unsigned char> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<unsigned char>((i * 31U + 7U) & 0xffU);
    }
    return bytes;
}

} // namespace

int main() {
    TemporaryDirectory temp;
    using namespace videovault::core;

    constexpr const char* kPassword = "current vault password";
    const auto root = temp.path() / L"vault";
    auto created = Vault::create(root, kPassword, testParameters());
    if (!created) {
        return fail("Creating a vault for administration tests must succeed.");
    }

    // Two distinct sources so removal can be verified against a survivor.
    const std::vector<unsigned char> first = deterministicBytes(64U * 1024U);
    const std::vector<unsigned char> second = deterministicBytes(128U * 1024U);
    const auto first_path = temp.path() / L"first.bin";
    const auto second_path = temp.path() / L"second.bin";
    writeBinaryFile(first_path, first);
    writeBinaryFile(second_path, second);

    const auto first_id = created.value().import_file(first_path);
    const auto second_id = created.value().import_file(second_path);
    if (!first_id || !second_id || first_id.value() == second_id.value()) {
        return fail("Importing two distinct sources must yield two distinct ids.");
    }

    // Removing a video must remove its row and package, keep the survivor.
    {
        const auto removed = created.value().remove_video(first_id.value());
        if (!removed || !removed.value()) {
            return fail("Removing an imported video must succeed.");
        }
        const auto listed = created.value().list_videos();
        if (!listed || listed.value().size() != 1U
            || listed.value()[0].id != second_id.value()) {
            return fail("Removal must leave exactly the other video in the gallery.");
        }
        const auto package = created.value().package_path(first_id.value());
        if (package || package.error().code != VaultErrorCode::InvalidArgument) {
            return fail("A removed video's package path must be unresolvable.");
        }
        const auto read = created.value().read_video_bytes(first_id.value());
        if (read || read.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Reading a removed video must fail with an unknown-id error.");
        }
        // The stored package file itself must be gone from disk.
        const auto first_rel = created.value().list_videos(); // survivor only
        (void)first_rel;
        const auto survivor_package = created.value().package_path(second_id.value());
        if (!survivor_package || !std::filesystem::is_regular_file(survivor_package.value())) {
            return fail("The surviving video's package must still exist.");
        }
        const auto removed_again = created.value().remove_video(first_id.value());
        if (removed_again || removed_again.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Removing an already removed video must fail with an unknown-id error.");
        }
        // The survivor remains readable.
        const auto survivor = created.value().read_video_bytes(second_id.value());
        if (!survivor || survivor.value() != second) {
            return fail("The surviving video must remain fully readable.");
        }
    }

    // Changing the password must re-key the vault while keeping videos intact.
    {
        constexpr const char* kNewPassword = "brand new vault password";
        auto changed = created.value().change_password(kPassword, kNewPassword, testParameters());
        if (!changed || !changed.value()) {
            return fail("Changing the vault password must succeed.");
        }
        // The old password must now be rejected on open and validation.
        const auto old_validate = Vault::validate_password(root, kPassword);
        if (!old_validate || old_validate.value()) {
            return fail("The old password must no longer validate.");
        }
        const auto old_open = Vault::open(root, kPassword);
        if (old_open || old_open.error().code != VaultErrorCode::WrongPassword) {
            return fail("Opening with the old password must return WrongPassword.");
        }
        // The new password must open the vault with the gallery intact.
        auto new_open = Vault::open(root, kNewPassword);
        if (!new_open) {
            return fail("Opening with the new password must succeed.");
        }
        const auto listed = new_open.value().list_videos();
        if (!listed || listed.value().size() != 1U
            || listed.value()[0].id != second_id.value()) {
            return fail("The gallery must survive the password change.");
        }
        const auto survivor = new_open.value().read_video_bytes(second_id.value());
        if (!survivor || survivor.value() != second) {
            return fail("A stored video must remain readable after the password change.");
        }
        // A second change must also work.
        constexpr const char* kThirdPassword = "third password version";
        auto changed_twice =
            new_open.value().change_password(kNewPassword, kThirdPassword, testParameters());
        if (!changed_twice || !changed_twice.value()) {
            return fail("Changing the password twice must succeed.");
        }
        const auto third_validate = Vault::validate_password(root, kThirdPassword);
        if (!third_validate || !third_validate.value()) {
            return fail("The third password must validate after the second change.");
        }
        auto third_open = Vault::open(root, kThirdPassword);
        if (!third_open) {
            return fail("Opening with the third password must succeed.");
        }
        const auto third_survivor = third_open.value().read_video_bytes(second_id.value());
        if (!third_survivor || third_survivor.value() != second) {
            return fail("A stored video must remain readable after two password changes.");
        }
        third_open.value().lock();
    }

    // A wrong current password must be rejected without touching the vault.
    {
        const auto rejected = created.value().change_password(
            "definitely not the current password", "another password", testParameters());
        if (rejected || rejected.error().code != VaultErrorCode::WrongPassword) {
            return fail("Changing with a wrong current password must return WrongPassword.");
        }
        auto still_open = Vault::open(root, "third password version");
        if (!still_open) {
            return fail("The vault must still open after a rejected password change.");
        }
        const auto listed = still_open.value().list_videos();
        if (!listed || listed.value().size() != 1U) {
            return fail("The gallery must be untouched after a rejected password change.");
        }
        still_open.value().lock();
    }

    // Administration calls on a locked vault must fail cleanly.
    {
        auto locked_vault = Vault::open(root, "third password version");
        if (!locked_vault) {
            return fail("Opening for the locked-call checks must succeed.");
        }
        locked_vault.value().lock();
        if (locked_vault.value().is_unlocked()) {
            return fail("Lock must transition the vault to the locked state.");
        }
        const auto locked_remove = locked_vault.value().remove_video(1);
        if (locked_remove || locked_remove.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Removing on a locked vault must fail cleanly.");
        }
        const auto locked_change = locked_vault.value().change_password(
            "third password version", "another password", testParameters());
        if (locked_change || locked_change.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Changing the password on a locked vault must fail cleanly.");
        }
    }

    std::cout << "Video removal and password-change checks succeeded.\n";
    return EXIT_SUCCESS;
}
