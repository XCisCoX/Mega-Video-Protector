#include "internal/crypto.hpp"

#include <argon2.h>
#include <sodium.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace videovault::core::internal {
namespace {

constexpr std::size_t kMasterKeyBytes = 32U;
constexpr std::uint32_t kMinimumMemoryKiB = 8U * 1024U;
constexpr std::uint32_t kMaximumMemoryKiB = 1024U * 1024U;
constexpr std::uint32_t kMaximumIterations = 20U;
constexpr std::uint32_t kMaximumParallelism = 16U;
constexpr std::size_t kMaximumPasswordBytes = 4096U;

std::once_flag g_sodium_once;
int g_sodium_status = -1;

VaultError crypto_error(std::string detail) {
    return {VaultErrorCode::CryptoFailure, std::move(detail)};
}

} // namespace

SensitiveBuffer::SensitiveBuffer(const std::size_t size) : size_(size) {
    if (size == 0U) {
        return;
    }
    data_ = static_cast<unsigned char*>(sodium_malloc(size));
    if (data_ == nullptr) {
        size_ = 0U;
        throw std::bad_alloc{};
    }
    locked_ = sodium_mlock(data_, size_) == 0;
    sodium_memzero(data_, size_);
}

SensitiveBuffer::~SensitiveBuffer() {
    release();
}

SensitiveBuffer::SensitiveBuffer(SensitiveBuffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0U)),
      locked_(std::exchange(other.locked_, false)) {}

SensitiveBuffer& SensitiveBuffer::operator=(SensitiveBuffer&& other) noexcept {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0U);
        locked_ = std::exchange(other.locked_, false);
    }
    return *this;
}

void SensitiveBuffer::release() noexcept {
    if (data_ == nullptr) {
        return;
    }
    sodium_memzero(data_, size_);
    if (locked_) {
        (void)sodium_munlock(data_, size_);
    }
    sodium_free(data_);
    data_ = nullptr;
    size_ = 0U;
    locked_ = false;
}

Result<bool> initialize_crypto() {
    std::call_once(g_sodium_once, [] { g_sodium_status = sodium_init(); });
    if (g_sodium_status < 0) {
        return crypto_error("libsodium initialization failed");
    }
    return true;
}

bool is_valid_argon2_parameters(const Argon2Parameters& parameters) noexcept {
    return parameters.memory_kib >= kMinimumMemoryKiB
        && parameters.memory_kib <= kMaximumMemoryKiB
        && parameters.iterations >= 1U
        && parameters.iterations <= kMaximumIterations
        && parameters.parallelism >= 1U
        && parameters.parallelism <= kMaximumParallelism;
}

VaultError validate_argon2_parameters(
    const Argon2Parameters& parameters,
    const std::string_view password) {
    if (password.empty()) {
        return {VaultErrorCode::InvalidArgument, "password must not be empty"};
    }
    if (password.size() > kMaximumPasswordBytes) {
        return {VaultErrorCode::InvalidArgument, "password exceeds the supported byte length"};
    }
    if (!is_valid_argon2_parameters(parameters)) {
        return {VaultErrorCode::InvalidArgument, "Argon2id parameters are outside supported safety bounds"};
    }
    return {VaultErrorCode::InvalidArgument, {}};
}

Result<SensitiveBuffer> derive_master_key(
    const std::string_view password,
    const std::span<const unsigned char, 16> salt,
    const Argon2Parameters& parameters) {
    const auto validation = validate_argon2_parameters(parameters, password);
    if (!validation.technical_detail.empty()) {
        return validation;
    }

    try {
        SensitiveBuffer output(kMasterKeyBytes);
        const int status = argon2id_hash_raw(
            parameters.iterations,
            parameters.memory_kib,
            parameters.parallelism,
            password.data(),
            password.size(),
            salt.data(),
            salt.size(),
            output.data(),
            output.size());
        if (status != ARGON2_OK) {
            return crypto_error(std::string("Argon2id failed: ") + argon2_error_message(status));
        }
        return std::move(output);
    } catch (const std::bad_alloc&) {
        return crypto_error("secure memory allocation failed during Argon2id");
    }
}

Result<SensitiveBuffer> derive_subkey(
    const SensitiveBuffer& master_key,
    const std::uint64_t subkey_id,
    const std::array<char, 8>& context) {
    if (master_key.size() != crypto_kdf_KEYBYTES) {
        return crypto_error("invalid master key length for domain-separated derivation");
    }
    try {
        SensitiveBuffer output(32U);
        if (crypto_kdf_derive_from_key(
                output.data(), output.size(), subkey_id, context.data(), master_key.data()) != 0) {
            return crypto_error("domain-separated key derivation failed");
        }
        return std::move(output);
    } catch (const std::bad_alloc&) {
        return crypto_error("secure memory allocation failed during subkey derivation");
    }
}

void random_bytes(const std::span<unsigned char> destination) noexcept {
    randombytes_buf(destination.data(), destination.size());
}

Sha256Digest sha256(const std::span<const unsigned char> data) {
    Sha256Digest digest{};
    crypto_hash_sha256(digest.data(), data.data(), data.size());
    return digest;
}

Sha256Accumulator::Sha256Accumulator() {
    crypto_hash_sha256_init(&state_);
}

void Sha256Accumulator::update(const std::span<const unsigned char> data) noexcept {
    if (!finished_ && !data.empty()) {
        crypto_hash_sha256_update(&state_, data.data(), data.size());
    }
}

Sha256Digest Sha256Accumulator::digest() noexcept {
    Sha256Digest digest{};
    if (!finished_) {
        crypto_hash_sha256_final(&state_, digest.data());
        finished_ = true;
    }
    return digest;
}

Result<std::vector<unsigned char>> encrypt_xchacha20_poly1305(
    const std::span<const unsigned char> plaintext,
    const std::span<const unsigned char> associated_data,
    const std::span<const unsigned char, 24> nonce,
    const SensitiveBuffer& key) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        return crypto_error("invalid XChaCha20-Poly1305 key length");
    }

    std::vector<unsigned char> ciphertext(
        plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long ciphertext_size = 0U;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            ciphertext.data(), &ciphertext_size,
            plaintext.data(), plaintext.size(),
            associated_data.data(), associated_data.size(),
            nullptr, nonce.data(), key.data()) != 0) {
        sodium_memzero(ciphertext.data(), ciphertext.size());
        return crypto_error("XChaCha20-Poly1305 encryption failed");
    }
    ciphertext.resize(static_cast<std::size_t>(ciphertext_size));
    return ciphertext;
}

Result<std::vector<unsigned char>> decrypt_xchacha20_poly1305(
    const std::span<const unsigned char> ciphertext,
    const std::span<const unsigned char> associated_data,
    const std::span<const unsigned char, 24> nonce,
    const SensitiveBuffer& key,
    const VaultErrorCode authentication_error) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES
        || ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return VaultError{authentication_error, "invalid authenticated record"};
    }

    std::vector<unsigned char> plaintext(
        ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long plaintext_size = 0U;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            plaintext.data(), &plaintext_size, nullptr,
            ciphertext.data(), ciphertext.size(),
            associated_data.data(), associated_data.size(),
            nonce.data(), key.data()) != 0) {
        sodium_memzero(plaintext.data(), plaintext.size());
        return VaultError{authentication_error, "authenticated password-verification record rejected"};
    }
    plaintext.resize(static_cast<std::size_t>(plaintext_size));
    return plaintext;
}

bool constant_time_equal(
    const std::span<const unsigned char> left,
    const std::span<const unsigned char> right) noexcept {
    return left.size() == right.size()
        && sodium_memcmp(left.data(), right.data(), left.size()) == 0;
}

} // namespace videovault::core::internal
