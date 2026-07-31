#include "internal/metadata.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace videovault::core::internal {
namespace {

constexpr std::array<unsigned char, 8> kMetadataMagic{
    'M', 'V', 'P', 'V', 'L', 'T', '0', '1'};
constexpr std::array<unsigned char, 8> kReadyMagic{
    'M', 'V', 'P', 'R', 'E', 'A', 'D', 'Y'};
constexpr std::array<unsigned char, 32> kVerifierPlaintext{
    'M', 'V', 'P', '-', 'P', 'A', 'S', 'S', 'W', 'O', 'R', 'D', '-',
    'V', 'E', 'R', 'I', 'F', 'I', 'E', 'R', '-', 'V', '1', 0x8d, 0x31,
    0xa6, 0x5c, 0x77, 0x02, 0xe9, 0x4b};
constexpr std::uint32_t kMetadataVersion = 1U;
constexpr std::uint32_t kKdfArgon2id = 1U;
constexpr std::size_t kNonceOffset = kVaultHeaderSize;
constexpr std::size_t kCiphertextOffset = kNonceOffset + kVerifierNonceSize;

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
private:
    void reset() noexcept {
        if (valid()) {
            (void)CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }
    HANDLE handle_;
};

void write_u32_le(std::span<unsigned char> destination, const std::uint32_t value) {
    destination[0] = static_cast<unsigned char>(value & 0xffU);
    destination[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    destination[2] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    destination[3] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

std::uint32_t read_u32_le(const std::span<const unsigned char> source) {
    return static_cast<std::uint32_t>(source[0])
        | (static_cast<std::uint32_t>(source[1]) << 8U)
        | (static_cast<std::uint32_t>(source[2]) << 16U)
        | (static_cast<std::uint32_t>(source[3]) << 24U);
}

VaultError windows_error(const char* operation, const DWORD code) {
    VaultErrorCode mapped = VaultErrorCode::DatabaseFailure;
    if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) {
        mapped = VaultErrorCode::PermissionDenied;
    } else if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) {
        mapped = VaultErrorCode::DiskFull;
    }
    return {mapped, std::string(operation) + " failed with Windows error " + std::to_string(code)};
}

Result<std::vector<unsigned char>> read_exact_file(
    const std::filesystem::path& path,
    const std::size_t expected_size) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return VaultError{VaultErrorCode::PermissionDenied, "unable to open vault metadata file"};
    }
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) != expected_size) {
        return VaultError{VaultErrorCode::MetadataCorrupt, "vault metadata has an invalid size"};
    }
    input.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(expected_size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return VaultError{VaultErrorCode::MetadataCorrupt, "vault metadata could not be read completely"};
    }
    return bytes;
}

} // namespace

Result<VaultMetadata> create_metadata_header(const Argon2Parameters& parameters) {
    if (!is_valid_argon2_parameters(parameters)) {
        return VaultError{VaultErrorCode::InvalidArgument, "invalid Argon2id parameters"};
    }

    VaultMetadata metadata;
    metadata.parameters = parameters;
    random_bytes(metadata.salt);
    random_bytes(metadata.vault_id);

    std::copy(kMetadataMagic.begin(), kMetadataMagic.end(), metadata.encoded.begin());
    write_u32_le(std::span(metadata.encoded).subspan(8U, 4U), kMetadataVersion);
    write_u32_le(std::span(metadata.encoded).subspan(12U, 4U), kKdfArgon2id);
    write_u32_le(std::span(metadata.encoded).subspan(16U, 4U), parameters.memory_kib);
    write_u32_le(std::span(metadata.encoded).subspan(20U, 4U), parameters.iterations);
    write_u32_le(std::span(metadata.encoded).subspan(24U, 4U), parameters.parallelism);
    write_u32_le(std::span(metadata.encoded).subspan(28U, 4U), 0U);
    std::copy(metadata.salt.begin(), metadata.salt.end(), metadata.encoded.begin() + 32U);
    std::copy(metadata.vault_id.begin(), metadata.vault_id.end(), metadata.encoded.begin() + 48U);
    return metadata;
}

Result<bool> seal_password_verifier(
    VaultMetadata& metadata,
    const SensitiveBuffer& verifier_key) {
    auto nonce = std::span(metadata.encoded).subspan<kNonceOffset, kVerifierNonceSize>();
    random_bytes(nonce);
    auto encrypted = encrypt_xchacha20_poly1305(
        kVerifierPlaintext,
        std::span(metadata.encoded).first(kVaultHeaderSize),
        std::span<const unsigned char, kVerifierNonceSize>(nonce),
        verifier_key);
    if (!encrypted) {
        return encrypted.error();
    }
    if (encrypted.value().size() != kVerifierCiphertextSize) {
        return VaultError{VaultErrorCode::CryptoFailure, "password verifier has an invalid encrypted size"};
    }
    std::copy(
        encrypted.value().begin(), encrypted.value().end(),
        metadata.encoded.begin() + static_cast<std::ptrdiff_t>(kCiphertextOffset));
    return true;
}

Result<bool> verify_password_verifier(
    const VaultMetadata& metadata,
    const SensitiveBuffer& verifier_key,
    const VaultErrorCode failure_code) {
    const auto nonce = std::span(metadata.encoded).subspan<kNonceOffset, kVerifierNonceSize>();
    const auto ciphertext = std::span(metadata.encoded).subspan<kCiphertextOffset, kVerifierCiphertextSize>();
    auto plaintext = decrypt_xchacha20_poly1305(
        ciphertext,
        std::span(metadata.encoded).first(kVaultHeaderSize),
        std::span<const unsigned char, kVerifierNonceSize>(nonce),
        verifier_key,
        failure_code);
    if (!plaintext) {
        return plaintext.error();
    }
    if (!constant_time_equal(plaintext.value(), kVerifierPlaintext)) {
        return VaultError{failure_code, "password-verification plaintext did not match"};
    }
    return true;
}

Result<VaultMetadata> read_metadata_file(const std::filesystem::path& path) {
    auto loaded = read_exact_file(path, kVaultMetadataSize);
    if (!loaded) {
        return loaded.error();
    }

    VaultMetadata metadata;
    std::copy(loaded.value().begin(), loaded.value().end(), metadata.encoded.begin());
    if (!std::equal(kMetadataMagic.begin(), kMetadataMagic.end(), metadata.encoded.begin())) {
        return VaultError{VaultErrorCode::MetadataCorrupt, "vault metadata magic is invalid"};
    }
    const auto bytes = std::span<const unsigned char>(metadata.encoded);
    const auto version = read_u32_le(bytes.subspan(8U, 4U));
    if (version != kMetadataVersion) {
        return VaultError{VaultErrorCode::UnsupportedVaultVersion, "unsupported vault metadata version"};
    }
    if (read_u32_le(bytes.subspan(12U, 4U)) != kKdfArgon2id
        || read_u32_le(bytes.subspan(28U, 4U)) != 0U) {
        return VaultError{VaultErrorCode::MetadataCorrupt, "vault metadata algorithm or reserved field is invalid"};
    }

    metadata.parameters.memory_kib = read_u32_le(bytes.subspan(16U, 4U));
    metadata.parameters.iterations = read_u32_le(bytes.subspan(20U, 4U));
    metadata.parameters.parallelism = read_u32_le(bytes.subspan(24U, 4U));
    if (!is_valid_argon2_parameters(metadata.parameters)) {
        return VaultError{VaultErrorCode::MetadataCorrupt, "vault Argon2id parameters are outside safety bounds"};
    }
    std::copy_n(metadata.encoded.begin() + 32U, metadata.salt.size(), metadata.salt.begin());
    std::copy_n(metadata.encoded.begin() + 48U, metadata.vault_id.size(), metadata.vault_id.begin());
    return metadata;
}

Result<bool> write_atomic_file(
    const std::filesystem::path& path,
    const std::span<const unsigned char> bytes) {
    auto temporary = path;
    temporary += L".write.tmp";
    UniqueHandle file(CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!file.valid()) {
        return windows_error("CreateFileW", GetLastError());
    }

    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD));
        DWORD written = 0U;
        if (!WriteFile(file.get(), bytes.data() + offset, request, &written, nullptr) || written == 0U) {
            return windows_error("WriteFile", GetLastError());
        }
        offset += written;
    }
    if (!FlushFileBuffers(file.get())) {
        return windows_error("FlushFileBuffers", GetLastError());
    }
    file = UniqueHandle{};

    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return windows_error("MoveFileExW", GetLastError());
    }
    return true;
}

Result<bool> flush_existing_file(const std::filesystem::path& path) {
    UniqueHandle file(CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!file.valid()) {
        return windows_error("open file for flush", GetLastError());
    }
    if (!FlushFileBuffers(file.get())) {
        return windows_error("flush existing file", GetLastError());
    }
    return true;
}

Result<bool> move_file_atomic(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return windows_error("atomic file move", GetLastError());
    }
    return true;
}

Result<bool> write_ready_marker(
    const std::filesystem::path& path,
    const VaultMetadata& metadata) {
    std::array<unsigned char, 24> marker{};
    std::copy(kReadyMagic.begin(), kReadyMagic.end(), marker.begin());
    std::copy(metadata.vault_id.begin(), metadata.vault_id.end(), marker.begin() + 8U);
    return write_atomic_file(path, marker);
}

Result<bool> verify_ready_marker(
    const std::filesystem::path& path,
    const VaultMetadata& metadata) {
    auto loaded = read_exact_file(path, 24U);
    if (!loaded) {
        return VaultError{VaultErrorCode::MetadataCorrupt, "vault ready marker is missing or invalid"};
    }
    const bool magic_matches = std::equal(kReadyMagic.begin(), kReadyMagic.end(), loaded.value().begin());
    const bool id_matches = constant_time_equal(
        std::span(loaded.value()).subspan(8U, 16U), metadata.vault_id);
    if (!magic_matches || !id_matches) {
        return VaultError{VaultErrorCode::MetadataCorrupt, "vault ready marker does not match metadata"};
    }
    return true;
}

} // namespace videovault::core::internal
