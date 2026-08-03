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
            / (L"MegaVideoProtect_Phase3_" + suffix);
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

    const auto root = temp.path() / L"vault";
    auto created = Vault::create(root, "import test password", testParameters());
    if (!created) {
        return fail("Creating a vault for import tests must succeed.");
    }

    // 200 KiB source: 4 chunks at the default 64 KiB chunk size.
    const std::vector<unsigned char> source = deterministicBytes(200U * 1024U);
    const auto source_path = temp.path() / L"sample.bin";
    writeBinaryFile(source_path, source);

    const auto imported = created.value().import_file(source_path);
    if (!imported) {
        std::cerr << "Importing a video file must succeed. Error: "
                  << imported.error().technical_detail << '\n';
        return EXIT_FAILURE;
    }
    const int64_t video_id = imported.value();
    if (video_id <= 0) {
        return fail("The imported video must receive a positive database id.");
    }

    const auto listed = created.value().list_videos();
    if (!listed) {
        return fail("Listing videos must succeed.");
    }
    if (listed.value().size() != 1U || listed.value()[0].id != video_id) {
        return fail("The gallery must contain exactly the imported video.");
    }
    const auto& video = listed.value()[0];
    if (video.display_name != "sample.bin" || video.original_size != source.size()) {
        return fail("The gallery entry must preserve the source name and size.");
    }

    const auto package = created.value().package_path(video_id);
    if (!package) {
        return fail("Resolving the package path must succeed.");
    }
    if (!std::filesystem::is_regular_file(package.value())) {
        return fail("The resolved package path must point at a stored package file.");
    }
    const auto package_size = std::filesystem::file_size(package.value());
    if (package_size != video.package_size) {
        return fail("The package file size must match the gallery record.");
    }
    // 84-byte header + tag, then 3 full 64 KiB chunks and one 8 KiB tail, each +16 tag.
    const std::uint64_t expected = 84U + 3U * (65536U + 16U) + (8192U + 16U);
    if (package_size != expected) {
        return fail("The package file must match the pinned chunked format size.");
    }
    if (package.value().extension() != L".vvp"
        || package.value().wstring().find(L"vault-data") == std::wstring::npos) {
        return fail("The package must be stored under vault-data with a .vvp extension.");
    }

    // The sha256 of the package must encode its stored relative path.
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string expected;
        for (const unsigned char byte : video.package_sha256) {
            expected.push_back(digits[byte >> 4U]);
            expected.push_back(digits[byte & 0x0fU]);
        }
        const std::string expected_path =
            expected.substr(0, 2) + "/" + expected.substr(2, 2) + "/" + expected + ".vvp";
        if (video.package_relative_path != expected_path) {
            return fail("The package relative path must be derived from its sha256.");
        }
    }

    // Slice B: read_video_bytes must reproduce the exact original content.
    {
        const auto decrypted = created.value().read_video_bytes(video_id);
        if (!decrypted) {
            std::cerr << "Reading a video must succeed. Error: "
                      << decrypted.error().technical_detail << '\n';
            return EXIT_FAILURE;
        }
        if (decrypted.value() != source) {
            return fail("Decrypting an imported package must reproduce the original bytes.");
        }
    }

    // An empty source must round-trip as an empty, header-only package.
    {
        const auto empty_path = temp.path() / L"empty.bin";
        writeBinaryFile(empty_path, {});
        const auto empty_id = created.value().import_file(empty_path);
        if (!empty_id) {
            return fail("Importing an empty file must succeed.");
        }
        const auto decrypted = created.value().read_video_bytes(empty_id.value());
        if (!decrypted || !decrypted.value().empty()) {
            return fail("An empty source must round-trip to an empty package.");
        }
        const auto empty_package = created.value().package_path(empty_id.value());
        if (!empty_package
            || std::filesystem::file_size(empty_package.value()) != 84U) {
            return fail("An empty package must be exactly header plus tag.");
        }
    }

    // Slice C: persistence across lock/reopen.
    created.value().lock();
    auto reopened = Vault::open(root, "import test password");
    if (!reopened) {
        return fail("A vault with imported videos must reopen with the correct password.");
    }
    const auto listed_again = reopened.value().list_videos();
    if (!listed_again || listed_again.value().size() != 2U) {
        return fail("The gallery must persist across lock and reopen.");
    }
    {
        const auto decrypted = reopened.value().read_video_bytes(video_id);
        if (!decrypted || decrypted.value() != source) {
            return fail("An imported video must remain readable after reopen.");
        }
    }

    // Each import draws a fresh random file key, so re-importing identical
    // source content produces a distinct package and a new gallery entry.
    {
        const auto duplicate_id = reopened.value().import_file(source_path);
        if (!duplicate_id || duplicate_id.value() == video_id) {
            return fail("Re-importing must create a fresh package with its own id.");
        }
        const auto listed_three = reopened.value().list_videos();
        if (!listed_three || listed_three.value().size() != 3U) {
            return fail("Each import must add exactly one gallery row.");
        }
    }

    // A modified package must fail authentication with PackageModified.
    {
        const auto package = reopened.value().package_path(video_id).value();
        // Last chunk ciphertext starts at header+tag plus 3 full chunks.
        const std::streampos corrupted_byte = 84 + 3 * (65536 + 16) + 10;
        std::fstream file(package, std::ios::binary | std::ios::in | std::ios::out);
        if (!file) {
            return fail("The tamper test could not open the package file.");
        }
        file.seekg(corrupted_byte);
        char byte = 0;
        file.read(&byte, 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
        file.seekp(corrupted_byte);
        file.write(&byte, 1);
        file.flush();
        if (!file) {
            return fail("The tamper test could not modify the package file.");
        }
        const auto corrupted = reopened.value().read_video_bytes(video_id);
        if (corrupted
            || corrupted.error().code != videovault::core::VaultErrorCode::PackageModified) {
            return fail("A modified package must return PackageModified.");
        }
    }

    // A deleted package file must surface as PackageMissing.
    {
        const auto package = reopened.value().package_path(video_id).value();
        std::error_code ignored;
        std::filesystem::remove(package, ignored);
        const auto missing = reopened.value().read_video_bytes(video_id);
        if (missing
            || missing.error().code != videovault::core::VaultErrorCode::PackageMissing) {
            return fail("A missing package file must return PackageMissing.");
        }
    }

    std::cout << "Import, gallery, round-trip, persistence, and integrity checks succeeded.\n";
    return EXIT_SUCCESS;
}
