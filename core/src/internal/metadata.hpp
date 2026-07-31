#pragma once

#include "internal/crypto.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>

namespace videovault::core::internal {

inline constexpr std::size_t kVaultHeaderSize = 64U;
inline constexpr std::size_t kVerifierNonceSize = 24U;
inline constexpr std::size_t kVerifierCiphertextSize = 48U;
inline constexpr std::size_t kVaultMetadataSize =
    kVaultHeaderSize + kVerifierNonceSize + kVerifierCiphertextSize;

struct VaultMetadata {
    Argon2Parameters parameters;
    std::array<unsigned char, 16> salt{};
    std::array<unsigned char, 16> vault_id{};
    std::array<unsigned char, kVaultMetadataSize> encoded{};
};

[[nodiscard]] Result<VaultMetadata> create_metadata_header(
    const Argon2Parameters& parameters);
[[nodiscard]] Result<bool> seal_password_verifier(
    VaultMetadata& metadata,
    const SensitiveBuffer& verifier_key);
[[nodiscard]] Result<bool> verify_password_verifier(
    const VaultMetadata& metadata,
    const SensitiveBuffer& verifier_key,
    VaultErrorCode failure_code);

[[nodiscard]] Result<VaultMetadata> read_metadata_file(
    const std::filesystem::path& path);
[[nodiscard]] Result<bool> write_atomic_file(
    const std::filesystem::path& path,
    std::span<const unsigned char> bytes);
[[nodiscard]] Result<bool> flush_existing_file(
    const std::filesystem::path& path);
[[nodiscard]] Result<bool> move_file_atomic(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

[[nodiscard]] Result<bool> write_ready_marker(
    const std::filesystem::path& path,
    const VaultMetadata& metadata);
[[nodiscard]] Result<bool> verify_ready_marker(
    const std::filesystem::path& path,
    const VaultMetadata& metadata);

} // namespace videovault::core::internal
