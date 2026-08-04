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
            / (L"MegaVideoProtect_Phase5_" + suffix);
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

bool rangeMatches(
    const std::vector<unsigned char>& actual,
    const std::vector<unsigned char>& source,
    const std::uint64_t offset) {
    if (actual.size() > source.size()
        || offset + actual.size() > source.size()) {
        return false;
    }
    return std::equal(actual.begin(), actual.end(),
        source.begin() + static_cast<std::ptrdiff_t>(offset));
}

} // namespace

int main() {
    TemporaryDirectory temp;
    using namespace videovault::core;

    // 300 KiB source: 5 chunks at the default 64 KiB chunk size (last is
    // partial with 45056 bytes).
    constexpr std::size_t kSourceSize = 300U * 1024U;
    const std::vector<unsigned char> source = deterministicBytes(kSourceSize);

    const auto root = temp.path() / L"vault";
    auto created = Vault::create(root, "reader test password", testParameters());
    if (!created) {
        return fail("Creating a vault for reader tests must succeed.");
    }
    const auto source_path = temp.path() / L"source.bin";
    writeBinaryFile(source_path, source);
    const auto imported = created.value().import_file(source_path);
    if (!imported) {
        return fail("Importing the reader test source must succeed.");
    }
    const int64_t video_id = imported.value();

    // Whole-file read still reproduces the source exactly.
    {
        const auto whole = created.value().read_video_bytes(video_id);
        if (!whole || whole.value() != source) {
            return fail("Whole-file read must reproduce the original bytes.");
        }
    }

    // Random-access ranges across chunk boundaries and the partial tail.
    {
        const auto head = created.value().read_video_range(video_id, 0U, 1000U);
        if (!head || !rangeMatches(head.value(), source, 0U)) {
            return fail("A range at the start must match the source.");
        }
        const auto cross = created.value().read_video_range(video_id, 65536U, 70000U - 65536U);
        if (!cross || !rangeMatches(cross.value(), source, 65536U)) {
            return fail("A range crossing a chunk boundary must match the source.");
        }
        const auto middle = created.value().read_video_range(video_id, 200000U, 100U);
        if (!middle || !rangeMatches(middle.value(), source, 200000U)) {
            return fail("A range inside a later chunk must match the source.");
        }
        const auto tail = created.value().read_video_range(
            video_id, kSourceSize - 100U, 300U);
        if (!tail || tail.value().size() != 100U
            || !rangeMatches(tail.value(), source, kSourceSize - 100U)) {
            return fail("A range overlapping EOF must stop at the plaintext end.");
        }
        const auto exact_tail = created.value().read_video_range(
            video_id, kSourceSize - 100U, 100U);
        if (!exact_tail || exact_tail.value().size() != 100U
            || !rangeMatches(exact_tail.value(), source, kSourceSize - 100U)) {
            return fail("The final partial chunk must read correctly.");
        }
    }

    // EOF and out-of-range offsets yield empty results.
    {
        const auto at_eof = created.value().read_video_range(video_id, kSourceSize, 10U);
        if (!at_eof || !at_eof.value().empty()) {
            return fail("A range starting at EOF must be empty.");
        }
        const auto past_eof = created.value().read_video_range(video_id, kSourceSize + 100U, 10U);
        if (!past_eof || !past_eof.value().empty()) {
            return fail("A range starting past EOF must be empty.");
        }
        const auto zero = created.value().read_video_range(video_id, 0U, 0U);
        if (!zero || !zero.value().empty()) {
            return fail("A zero-length range must be empty.");
        }
    }

    // Tampering with a stored chunk must fail authentication.
    {
        const auto package = created.value().package_path(video_id).value();
        // Second chunk ciphertext starts at header+tag plus one full chunk.
        const std::streampos corrupted_byte = 84 + (65536 + 16) + 5;
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
        // The tampered byte lives in chunk 1: a range starting at chunk 1.
        const auto corrupted = created.value().read_video_range(video_id, 65536U, 100U);
        if (corrupted
            || corrupted.error().code != VaultErrorCode::PackageModified) {
            return fail("Reading a tampered chunk must return PackageModified.");
        }
        // Untouched chunks elsewhere in the same package stay readable.
        const auto intact = created.value().read_video_range(video_id, 0U, 100U);
        if (!intact || !rangeMatches(intact.value(), source, 0U)) {
            return fail("Untouched chunks must remain readable after a tamper.");
        }
    }

    // A deleted package must surface as PackageMissing.
    {
        const auto package = created.value().package_path(video_id).value();
        std::error_code ignored;
        std::filesystem::remove(package, ignored);
        const auto missing = created.value().read_video_range(video_id, 0U, 100U);
        if (missing
            || missing.error().code != VaultErrorCode::PackageMissing) {
            return fail("A missing package must return PackageMissing.");
        }
    }

    // Unknown id and locked vault must fail cleanly.
    {
        const auto unknown = created.value().read_video_range(99999, 0U, 10U);
        if (unknown || unknown.error().code != VaultErrorCode::InvalidArgument) {
            return fail("An unknown video id must return InvalidArgument.");
        }
        created.value().lock();
        const auto locked = created.value().read_video_range(video_id, 0U, 10U);
        if (locked || locked.error().code != VaultErrorCode::InvalidArgument) {
            return fail("Reading on a locked vault must fail cleanly.");
        }
    }

    std::cout << "Streaming authenticated reader checks succeeded.\n";
    return EXIT_SUCCESS;
}
