#pragma once

#include "videovault/core/vault.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace videovault::core::internal {

class SensitiveBuffer final {
public:
    SensitiveBuffer() noexcept = default;
    explicit SensitiveBuffer(std::size_t size);
    ~SensitiveBuffer();

    SensitiveBuffer(SensitiveBuffer&& other) noexcept;
    SensitiveBuffer& operator=(SensitiveBuffer&& other) noexcept;

    SensitiveBuffer(const SensitiveBuffer&) = delete;
    SensitiveBuffer& operator=(const SensitiveBuffer&) = delete;

    [[nodiscard]] unsigned char* data() noexcept { return data_; }
    [[nodiscard]] const unsigned char* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

private:
    void release() noexcept;

    unsigned char* data_{nullptr};
    std::size_t size_{0};
    bool locked_{false};
};

[[nodiscard]] Result<bool> initialize_crypto();
[[nodiscard]] VaultError validate_argon2_parameters(
    const Argon2Parameters& parameters,
    std::string_view password);
[[nodiscard]] bool is_valid_argon2_parameters(
    const Argon2Parameters& parameters) noexcept;

[[nodiscard]] Result<SensitiveBuffer> derive_master_key(
    std::string_view password,
    std::span<const unsigned char, 16> salt,
    const Argon2Parameters& parameters);

[[nodiscard]] Result<SensitiveBuffer> derive_subkey(
    const SensitiveBuffer& master_key,
    std::uint64_t subkey_id,
    const std::array<char, 8>& context);

void random_bytes(std::span<unsigned char> destination) noexcept;

[[nodiscard]] Result<std::vector<unsigned char>> encrypt_xchacha20_poly1305(
    std::span<const unsigned char> plaintext,
    std::span<const unsigned char> associated_data,
    std::span<const unsigned char, 24> nonce,
    const SensitiveBuffer& key);

[[nodiscard]] Result<std::vector<unsigned char>> decrypt_xchacha20_poly1305(
    std::span<const unsigned char> ciphertext,
    std::span<const unsigned char> associated_data,
    std::span<const unsigned char, 24> nonce,
    const SensitiveBuffer& key,
    VaultErrorCode authentication_error);

[[nodiscard]] bool constant_time_equal(
    std::span<const unsigned char> left,
    std::span<const unsigned char> right) noexcept;

} // namespace videovault::core::internal
