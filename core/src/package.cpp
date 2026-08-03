#include "internal/package.hpp"

#include "internal/metadata.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace videovault::core::internal {
namespace {

constexpr std::array<unsigned char, 8> kAllOnesIndex{
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

constexpr std::size_t kChunkAdSize = 32U + 16U + 8U + 4U + 1U;
constexpr std::size_t kChunkNonceSize = 16U + 8U;

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

void write_u64_le(std::span<unsigned char> destination, const std::uint64_t value) {
    for (std::size_t i = 0; i < 8U; ++i) {
        destination[i] = static_cast<unsigned char>((value >> (8U * i)) & 0xffU);
    }
}

std::uint64_t read_u64_le(const std::span<const unsigned char> source) {
    std::uint64_t value = 0U;
    for (std::size_t i = 0; i < 8U; ++i) {
        value |= static_cast<std::uint64_t>(source[i]) << (8U * i);
    }
    return value;
}

std::uint32_t chunk_count_for(
    const std::uint64_t plaintext_size,
    const std::uint32_t chunk_size) {
    if (plaintext_size == 0U) {
        return 0U;
    }
    const std::uint64_t full = plaintext_size / chunk_size;
    const std::uint64_t remainder = plaintext_size % chunk_size;
    const std::uint64_t count = full + (remainder == 0U ? 0U : 1U);
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        return 0U;
    }
    return static_cast<std::uint32_t>(count);
}

std::uint64_t chunk_plaintext_size(
    const PackageHeader& header,
    const std::uint32_t chunk_index) {
    if (header.chunk_count == 0U) {
        return 0U;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(chunk_index) * header.chunk_size;
    return std::min<std::uint64_t>(
        header.chunk_size, header.plaintext_size - offset);
}

std::array<unsigned char, kChunkNonceSize> chunk_nonce(
    const PackageHeader& header,
    const std::uint64_t index) {
    std::array<unsigned char, kChunkNonceSize> nonce{};
    std::copy(header.nonce_prefix.begin(), header.nonce_prefix.end(), nonce.begin());
    write_u64_le(std::span(nonce).subspan(16U, 8U), index);
    return nonce;
}

std::array<unsigned char, kChunkNonceSize> header_nonce(const PackageHeader& header) {
    std::array<unsigned char, kChunkNonceSize> nonce{};
    std::copy(header.nonce_prefix.begin(), header.nonce_prefix.end(), nonce.begin());
    std::copy(kAllOnesIndex.begin(), kAllOnesIndex.end(), nonce.begin() + 16U);
    return nonce;
}

std::array<unsigned char, kChunkAdSize> chunk_ad(
    const Sha256Digest& digest,
    const PackageHeader& header,
    const std::uint64_t chunk_index,
    const std::uint32_t plaintext_length,
    const bool final_chunk) {
    std::array<unsigned char, kChunkAdSize> ad{};
    std::copy(digest.begin(), digest.end(), ad.begin());
    std::copy(header.package_id.begin(), header.package_id.end(), ad.begin() + 32U);
    write_u64_le(std::span(ad).subspan(48U, 8U), chunk_index);
    write_u32_le(std::span(ad).subspan(56U, 4U), plaintext_length);
    ad[60] = final_chunk ? 1U : 0U;
    return ad;
}

std::uint64_t package_file_size(
    const PackageHeader& header,
    const std::size_t header_and_tag_size) {
    // Header + tag + plaintext bytes + one AEAD tag per chunk.
    return header_and_tag_size + header.plaintext_size
        + static_cast<std::uint64_t>(header.chunk_count) * kPackageTagSize;
}

VaultError package_error(const char* operation, std::string detail) {
    return {VaultErrorCode::PackageModified,
        std::string(operation) + ": " + std::move(detail)};
}

} // namespace

std::array<unsigned char, kPackageHeaderSize> serialize_package_header(
    const PackageHeader& header) {
    std::array<unsigned char, kPackageHeaderSize> bytes{};
    std::copy(kPackageMagic.begin(), kPackageMagic.end(), bytes.begin());
    write_u32_le(std::span(bytes).subspan(8U, 4U), header.format_version);
    write_u32_le(std::span(bytes).subspan(12U, 4U), header.algorithm_id);
    write_u32_le(std::span(bytes).subspan(16U, 4U), header.header_size);
    write_u32_le(std::span(bytes).subspan(20U, 4U), header.chunk_size);
    write_u64_le(std::span(bytes).subspan(24U, 8U), header.plaintext_size);
    write_u32_le(std::span(bytes).subspan(32U, 4U), header.chunk_count);
    std::copy(header.package_id.begin(), header.package_id.end(), bytes.begin() + 36U);
    std::copy(header.nonce_prefix.begin(), header.nonce_prefix.end(), bytes.begin() + 52U);
    return bytes;
}

Result<PackageHeader> parse_package_header(
    const std::span<const unsigned char> bytes) {
    if (bytes.size() != kPackageHeaderSize) {
        return package_error("parse package header", "header has an invalid size");
    }
    if (!std::equal(kPackageMagic.begin(), kPackageMagic.end(), bytes.begin())) {
        return package_error("parse package header", "magic is invalid");
    }
    PackageHeader header;
    header.format_version = read_u32_le(bytes.subspan(8U, 4U));
    header.algorithm_id = read_u32_le(bytes.subspan(12U, 4U));
    header.header_size = read_u32_le(bytes.subspan(16U, 4U));
    header.chunk_size = read_u32_le(bytes.subspan(20U, 4U));
    header.plaintext_size = read_u64_le(bytes.subspan(24U, 8U));
    header.chunk_count = read_u32_le(bytes.subspan(32U, 4U));
    std::copy_n(bytes.begin() + 36U, header.package_id.size(), header.package_id.begin());
    std::copy_n(bytes.begin() + 52U, header.nonce_prefix.size(), header.nonce_prefix.begin());

    if (header.format_version != kPackageFormatVersion) {
        return VaultError{VaultErrorCode::UnsupportedPackageVersion,
            "unsupported package format version"};
    }
    if (header.algorithm_id != kPackageAlgorithmXChaCha20Poly1305
        || header.header_size != kPackageHeaderSize) {
        return package_error("parse package header", "algorithm or header size is invalid");
    }
    if (header.chunk_size < kMinimumPackageChunkSize
        || header.chunk_size > kMaximumPackageChunkSize) {
        return package_error("parse package header", "chunk size is outside supported bounds");
    }
    const auto expected_count = chunk_count_for(header.plaintext_size, header.chunk_size);
    if (header.chunk_count != expected_count || expected_count == 0U && header.plaintext_size != 0U) {
        return package_error("parse package header", "chunk count does not match the plaintext size");
    }
    return header;
}

Sha256Digest package_header_digest(const PackageHeader& header) {
    return sha256(serialize_package_header(header));
}

Result<PackageWriteResult> write_package_file(
    const std::filesystem::path& destination,
    const SensitiveBuffer& file_key,
    const std::uint32_t chunk_size,
    std::istream& source,
    const std::uint64_t plaintext_size) {
    if (file_key.size() != 32U) {
        return VaultError{VaultErrorCode::CryptoFailure, "invalid file key length"};
    }
    if (chunk_size < kMinimumPackageChunkSize || chunk_size > kMaximumPackageChunkSize) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "package chunk size is outside supported bounds"};
    }
    const auto count = chunk_count_for(plaintext_size, chunk_size);
    if (count == 0U && plaintext_size != 0U) {
        return VaultError{VaultErrorCode::InvalidArgument,
            "package is too large for the requested chunk size"};
    }

    PackageHeader header;
    header.chunk_size = chunk_size;
    header.plaintext_size = plaintext_size;
    header.chunk_count = count;
    random_bytes(header.package_id);
    random_bytes(header.nonce_prefix);
    const auto header_bytes = serialize_package_header(header);
    const auto digest = package_header_digest(header);

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        return VaultError{VaultErrorCode::DatabaseFailure,
            "unable to open package destination for writing"};
    }

    Sha256Accumulator accumulator;
    const auto write_all = [&output, &accumulator](const std::span<const unsigned char> bytes) -> bool {
        if (!bytes.empty()) {
            output.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            accumulator.update(bytes);
        }
        return static_cast<bool>(output);
    };

    if (!write_all(header_bytes)) {
        return VaultError{VaultErrorCode::DatabaseFailure, "package header write failed"};
    }
    auto header_tag = encrypt_xchacha20_poly1305(
        {}, header_bytes, header_nonce(header), file_key);
    if (!header_tag) {
        return header_tag.error();
    }
    if (header_tag.value().size() != kPackageTagSize) {
        return VaultError{VaultErrorCode::CryptoFailure, "package header tag has an invalid size"};
    }
    if (!write_all(header_tag.value())) {
        return VaultError{VaultErrorCode::DatabaseFailure, "package header tag write failed"};
    }

    std::vector<unsigned char> chunk(std::min<std::uint64_t>(
        chunk_size, std::numeric_limits<std::size_t>::max()));
    for (std::uint32_t index = 0U; index < header.chunk_count; ++index) {
        const auto chunk_length = chunk_plaintext_size(header, index);
        const auto current = static_cast<std::size_t>(chunk_length);
        source.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(current));
        if (source.gcount() != static_cast<std::streamsize>(current)) {
            return VaultError{VaultErrorCode::InvalidArgument,
                "source ended before the declared plaintext size"};
        }
        const bool final_chunk = index + 1U == header.chunk_count;
        const auto ad = chunk_ad(digest, header, index,
            static_cast<std::uint32_t>(chunk_length), final_chunk);
        auto ciphertext = encrypt_xchacha20_poly1305(
            std::span(chunk).first(current), ad, chunk_nonce(header, index), file_key);
        if (!ciphertext) {
            return ciphertext.error();
        }
        if (!write_all(ciphertext.value())) {
            return VaultError{VaultErrorCode::DatabaseFailure, "package chunk write failed"};
        }
    }

    output.flush();
    output.close();
    if (!output) {
        return VaultError{VaultErrorCode::DatabaseFailure, "package finalize failed"};
    }
    auto flushed = flush_existing_file(destination);
    if (!flushed) {
        return flushed.error();
    }

    PackageWriteResult result;
    result.sha256 = accumulator.digest();
    result.package_id = header.package_id;
    result.package_size = package_file_size(header, kPackageHeaderAndTagSize);
    return result;
}

Result<std::vector<unsigned char>> read_package_content(
    const std::filesystem::path& path,
    const SensitiveBuffer& file_key) {
    if (file_key.size() != 32U) {
        return VaultError{VaultErrorCode::CryptoFailure, "invalid file key length"};
    }
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return VaultError{VaultErrorCode::PackageMissing, "package file is unreadable"};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return VaultError{VaultErrorCode::PackageMissing, "unable to open package file"};
    }

    std::vector<unsigned char> header_bytes(kPackageHeaderSize);
    input.read(reinterpret_cast<char*>(header_bytes.data()),
        static_cast<std::streamsize>(header_bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(kPackageHeaderSize)) {
        return package_error("read package header", "file is truncated");
    }
    auto header = parse_package_header(header_bytes);
    if (!header) {
        return header.error();
    }

    const auto expected = package_file_size(header.value(), kPackageHeaderAndTagSize);
    if (file_size != expected) {
        return package_error("verify package size", "file size does not match the header");
    }
    const auto digest = package_header_digest(header.value());

    std::array<unsigned char, kPackageTagSize> header_tag{};
    input.read(reinterpret_cast<char*>(header_tag.data()),
        static_cast<std::streamsize>(header_tag.size()));
    if (input.gcount() != static_cast<std::streamsize>(kPackageTagSize)) {
        return package_error("read package header tag", "file is truncated");
    }
    auto header_verified = decrypt_xchacha20_poly1305(
        header_tag, header_bytes, header_nonce(header.value()), file_key,
        VaultErrorCode::PackageModified);
    if (!header_verified) {
        return header_verified.error();
    }

    std::vector<unsigned char> plaintext;
    plaintext.reserve(static_cast<std::size_t>(header.value().plaintext_size));
    std::vector<unsigned char> chunk(header.value().chunk_size + kPackageTagSize);
    for (std::uint32_t index = 0U; index < header.value().chunk_count; ++index) {
        const auto chunk_length = chunk_plaintext_size(header.value(), index);
        const auto current = static_cast<std::size_t>(chunk_length);
        input.read(reinterpret_cast<char*>(chunk.data()),
            static_cast<std::streamsize>(current + kPackageTagSize));
        if (input.gcount() != static_cast<std::streamsize>(current + kPackageTagSize)) {
            return package_error("read package chunk", "file is truncated");
        }
        const bool final_chunk = index + 1U == header.value().chunk_count;
        const auto ad = chunk_ad(digest, header.value(), index,
            static_cast<std::uint32_t>(chunk_length), final_chunk);
        auto decrypted = decrypt_xchacha20_poly1305(
            std::span(chunk).first(current + kPackageTagSize), ad,
            chunk_nonce(header.value(), index), file_key, VaultErrorCode::PackageModified);
        if (!decrypted) {
            return decrypted.error();
        }
        plaintext.insert(plaintext.end(), decrypted.value().begin(), decrypted.value().end());
    }
    return plaintext;
}

Result<PackageReader> PackageReader::open(
    const std::filesystem::path& path,
    SensitiveBuffer file_key) {
    if (file_key.size() != 32U) {
        return VaultError{VaultErrorCode::CryptoFailure, "invalid file key length"};
    }
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return VaultError{VaultErrorCode::PackageMissing, "package file is unreadable"};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return VaultError{VaultErrorCode::PackageMissing, "unable to open package file"};
    }

    std::vector<unsigned char> header_bytes(kPackageHeaderSize);
    input.read(reinterpret_cast<char*>(header_bytes.data()),
        static_cast<std::streamsize>(header_bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(kPackageHeaderSize)) {
        return package_error("read package header", "file is truncated");
    }
    auto header = parse_package_header(header_bytes);
    if (!header) {
        return header.error();
    }
    const auto expected = package_file_size(header.value(), kPackageHeaderAndTagSize);
    if (file_size != expected) {
        return package_error("verify package size", "file size does not match the header");
    }

    std::array<unsigned char, kPackageTagSize> header_tag{};
    input.read(reinterpret_cast<char*>(header_tag.data()),
        static_cast<std::streamsize>(header_tag.size()));
    if (input.gcount() != static_cast<std::streamsize>(kPackageTagSize)) {
        return package_error("read package header tag", "file is truncated");
    }
    auto header_verified = decrypt_xchacha20_poly1305(
        header_tag, header_bytes, header_nonce(header.value()), file_key,
        VaultErrorCode::PackageModified);
    if (!header_verified) {
        return header_verified.error();
    }

    PackageReader reader;
    reader.input_ = std::move(input);
    reader.header_ = std::move(header.value());
    reader.digest_ = package_header_digest(reader.header_);
    reader.file_key_ = std::move(file_key);
    reader.plaintext_size_ = reader.header_.plaintext_size;
    return reader;
}

Result<bool> PackageReader::seek(const std::uint64_t offset) {
    if (offset > plaintext_size_) {
        position_ = plaintext_size_;
        return true;
    }
    position_ = offset;
    return true;
}

void PackageReader::note_access(const std::size_t slot) noexcept {
    last_used_[slot] = ++clock_;
}

Result<std::vector<unsigned char>> PackageReader::chunk(const std::uint32_t index) {
    if (index >= header_.chunk_count) {
        return package_error("read chunk", "chunk index is out of range");
    }
    for (std::size_t slot = 0U; slot < cache_.size(); ++slot) {
        if (cache_[slot].index == index) {
            note_access(slot);
            return cache_[slot].plaintext;
        }
    }

    const auto chunk_length = chunk_plaintext_size(header_, index);
    const auto current = static_cast<std::size_t>(chunk_length);
    const auto file_offset = kPackageHeaderAndTagSize
        + static_cast<std::uint64_t>(index) * (header_.chunk_size + kPackageTagSize);

    input_.clear();
    input_.seekg(static_cast<std::streamoff>(file_offset));
    if (!input_) {
        return package_error("seek to chunk", "package file is unreadable");
    }
    std::vector<unsigned char> ciphertext(current + kPackageTagSize);
    input_.read(reinterpret_cast<char*>(ciphertext.data()),
        static_cast<std::streamsize>(ciphertext.size()));
    if (input_.gcount() != static_cast<std::streamsize>(ciphertext.size())) {
        return package_error("read chunk", "package file is truncated");
    }

    const bool final_chunk = index + 1U == header_.chunk_count;
    const auto ad = chunk_ad(digest_, header_, index,
        static_cast<std::uint32_t>(chunk_length), final_chunk);
    auto plaintext = decrypt_xchacha20_poly1305(
        ciphertext, ad, chunk_nonce(header_, index), file_key_,
        VaultErrorCode::PackageModified);
    if (!plaintext) {
        return plaintext.error();
    }

    std::size_t victim = 0U;
    for (std::size_t slot = 1U; slot < cache_.size(); ++slot) {
        if (last_used_[slot] < last_used_[victim]) {
            victim = slot;
        }
    }
    cache_[victim].index = index;
    cache_[victim].plaintext = std::move(plaintext.value());
    note_access(victim);
    return cache_[victim].plaintext;
}

Result<std::size_t> PackageReader::read(const std::span<unsigned char> destination) {
    std::size_t total = 0U;
    while (total < destination.size() && position_ < plaintext_size_) {
        const auto index = static_cast<std::uint32_t>(position_ / header_.chunk_size);
        const auto offset_in_chunk =
            static_cast<std::size_t>(position_ % header_.chunk_size);

        auto chunk_plaintext = chunk(index);
        if (!chunk_plaintext) {
            return chunk_plaintext.error();
        }
        const auto available = chunk_plaintext.value().size() - offset_in_chunk;
        const auto take = std::min<std::size_t>(
            destination.size() - total, available);
        std::copy_n(chunk_plaintext.value().begin() + offset_in_chunk, take,
            destination.begin() + static_cast<std::ptrdiff_t>(total));
        position_ += take;
        total += take;
    }
    return total;
}

} // namespace videovault::core::internal
