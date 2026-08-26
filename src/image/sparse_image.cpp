// SPDX-License-Identifier: MIT
#include "sparse_image.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace kairosboot::image {
namespace {

inline constexpr std::size_t kSparseHeaderSize = 28;
inline constexpr std::size_t kChunkHeaderSize = 12;
inline constexpr std::size_t kChecksumWindowSize = 64 * 1024;

[[nodiscard]] SparseError error(
    const SparseErrorKind kind,
    const std::uint64_t offset,
    std::string message) {
    return SparseError{kind, offset, std::move(message)};
}

[[nodiscard]] bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] std::uint16_t read_u16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 1]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3])) << 24U);
}

[[nodiscard]] std::expected<void, SparseError> read_exact(
    const IImageSource& source,
    const std::uint64_t source_size,
    const std::uint64_t offset,
    const std::span<std::byte> destination) {
    std::uint64_t end = 0;
    if (!checked_add(offset, destination.size(), end)) {
        return std::unexpected(error(
            SparseErrorKind::Malformed, offset, "input read range overflows 64-bit offset space"));
    }
    if (end > source_size) {
        return std::unexpected(error(
            SparseErrorKind::Truncated, offset, "sparse input ends before the declared data"));
    }

    std::size_t completed = 0;
    while (completed < destination.size()) {
        std::uint64_t current_offset = 0;
        if (!checked_add(offset, completed, current_offset)) {
            return std::unexpected(error(
                SparseErrorKind::Malformed, offset, "input read cursor overflows 64-bit space"));
        }
        auto result = source.read_at(current_offset, destination.subspan(completed));
        if (!result) {
            return std::unexpected(error(
                SparseErrorKind::Source, current_offset, result.error().message));
        }
        if (*result > destination.size() - completed) {
            return std::unexpected(error(
                SparseErrorKind::Source,
                current_offset,
                "image source reported reading more bytes than requested"));
        }
        if (*result == 0) {
            return std::unexpected(error(
                SparseErrorKind::Truncated,
                current_offset,
                "image source made no progress before the declared end"));
        }
        completed += *result;
    }
    return {};
}

[[nodiscard]] std::uint32_t update_crc32(
    std::uint32_t crc,
    const std::span<const std::byte> bytes) noexcept {
    crc = ~crc;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

// A CRC state is a 32-bit vector over GF(2). Each entry stores where one basis
// bit moves after a linear transformation. Squaring composes a transformation
// with itself, allowing an append of N zero bytes to be represented in O(log N)
// operations without walking the logical output.
using CrcLinearOperator = std::array<std::uint32_t, 32>;

[[nodiscard]] std::uint32_t apply_crc_operator(
    const CrcLinearOperator& operation,
    std::uint32_t value) noexcept {
    std::uint32_t result = 0;
    std::size_t bit = 0;
    while (value != 0) {
        if ((value & 1U) != 0) {
            result ^= operation[bit];
        }
        value >>= 1U;
        ++bit;
    }
    return result;
}

[[nodiscard]] CrcLinearOperator square_crc_operator(
    const CrcLinearOperator& operation) noexcept {
    CrcLinearOperator squared{};
    for (std::size_t bit = 0; bit < squared.size(); ++bit) {
        squared[bit] = apply_crc_operator(operation, operation[bit]);
    }
    return squared;
}

[[nodiscard]] CrcLinearOperator zero_byte_crc_operator() noexcept {
    CrcLinearOperator operation{};
    for (std::size_t bit = 0; bit < operation.size(); ++bit) {
        const auto basis = std::uint32_t{1} << bit;
        operation[bit] = (basis >> 1U) ^
                         (((basis & 1U) != 0) ? 0xEDB88320U : 0U);
    }
    // One reflected CRC step consumes one bit. Three squarings produce the
    // transformation for one complete zero byte.
    operation = square_crc_operator(operation);
    operation = square_crc_operator(operation);
    operation = square_crc_operator(operation);
    return operation;
}

[[nodiscard]] std::expected<std::uint32_t, SparseError> update_crc_from_source(
    const IImageSource& source,
    const std::uint64_t source_size,
    std::uint64_t input_offset,
    std::uint64_t byte_count,
    std::uint32_t crc) {
    std::array<std::byte, kChecksumWindowSize> buffer{};
    while (byte_count != 0) {
        const auto amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(byte_count, buffer.size()));
        if (const auto read = read_exact(
                source, source_size, input_offset, std::span(buffer).first(amount));
            !read) {
            return std::unexpected(read.error());
        }
        crc = update_crc32(crc, std::span<const std::byte>(buffer).first(amount));
        if (!checked_add(input_offset, amount, input_offset)) {
            return std::unexpected(error(
                SparseErrorKind::Malformed,
                input_offset,
                "CRC input cursor overflows 64-bit offset space"));
        }
        byte_count -= amount;
    }
    return crc;
}

[[nodiscard]] std::uint32_t update_crc_repeated(
    const std::uint32_t prefix_crc,
    const std::array<std::byte, 4>& pattern,
    const std::uint64_t byte_count) {
    auto remaining_repetitions = byte_count / pattern.size();
    const auto remainder = static_cast<std::size_t>(byte_count % pattern.size());

    // Appending one pattern is an affine GF(2) transformation:
    //     next = shift_four_bytes(current) XOR crc(pattern)
    // Square both its linear and constant parts to represent 2, 4, 8, ...
    // repetitions. Applying selected powers advances the prefix in O(log N).
    auto power_operator = zero_byte_crc_operator();
    power_operator = square_crc_operator(power_operator);  // two zero bytes
    power_operator = square_crc_operator(power_operator);  // four zero bytes
    auto power_bias = update_crc32(0, pattern);
    auto crc = prefix_crc;
    while (remaining_repetitions != 0) {
        if ((remaining_repetitions & 1U) != 0) {
            crc = apply_crc_operator(power_operator, crc) ^ power_bias;
        }
        remaining_repetitions >>= 1U;
        if (remaining_repetitions != 0) {
            power_bias = apply_crc_operator(power_operator, power_bias) ^ power_bias;
            power_operator = square_crc_operator(power_operator);
        }
    }

    if (remainder != 0) {
        crc = update_crc32(
            crc, std::span<const std::byte>(pattern).first(remainder));
    }
    return crc;
}

[[nodiscard]] std::array<std::byte, 4> fill_pattern(const std::uint32_t value) noexcept {
    return {
        static_cast<std::byte>(value & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 24U) & 0xFFU),
    };
}

[[nodiscard]] std::expected<void, SparseError> verify_checksums(
    const IImageSource& source,
    const std::uint64_t source_size,
    const SparseHeader& header,
    const std::span<const SparseChunk> chunks) {
    const auto has_crc_chunk = std::ranges::any_of(chunks, [](const SparseChunk& chunk) {
        return chunk.kind == SparseChunkKind::Crc32;
    });
    if (!has_crc_chunk && header.image_checksum == 0) {
        return {};
    }

    std::uint32_t crc = 0;
    const std::array<std::byte, 4> zeros{};
    for (const auto& chunk : chunks) {
        switch (chunk.kind) {
            case SparseChunkKind::Raw: {
                auto result = update_crc_from_source(
                    source, source_size, chunk.input_offset, chunk.output_size, crc);
                if (!result) {
                    return std::unexpected(result.error());
                }
                crc = *result;
                break;
            }
            case SparseChunkKind::Fill: {
                crc = update_crc_repeated(
                    crc, fill_pattern(*chunk.fill_value), chunk.output_size);
                break;
            }
            case SparseChunkKind::DontCare: {
                crc = update_crc_repeated(crc, zeros, chunk.output_size);
                break;
            }
            case SparseChunkKind::Crc32:
                if (!chunk.checksum || *chunk.checksum != crc) {
                    return std::unexpected(error(
                        SparseErrorKind::Malformed,
                        chunk.input_offset,
                        "CRC32 chunk does not match the expanded data preceding it"));
                }
                break;
        }
    }

    if (header.image_checksum != 0 && header.image_checksum != crc) {
        return std::unexpected(error(
            SparseErrorKind::Malformed,
            24,
            "sparse header checksum does not match the expanded image"));
    }
    return {};
}

}  // namespace

std::expected<SparseImage, SparseError> SparseImage::open(
    std::shared_ptr<const IImageSource> source) {
    if (!source) {
        return std::unexpected(error(
            SparseErrorKind::InvalidArgument, 0, "sparse image source is null"));
    }

    const auto source_size = source->size();
    std::array<std::byte, kSparseHeaderSize> raw_header{};
    if (const auto read = read_exact(*source, source_size, 0, raw_header); !read) {
        return std::unexpected(read.error());
    }

    const auto raw_header_span = std::span<const std::byte>(raw_header);
    if (read_u32(raw_header_span, 0) != kAndroidSparseMagic) {
        return std::unexpected(error(
            SparseErrorKind::Malformed, 0, "input does not have the Android sparse magic"));
    }

    SparseHeader header{
        .major_version = read_u16(raw_header_span, 4),
        .minor_version = read_u16(raw_header_span, 6),
        .file_header_size = read_u16(raw_header_span, 8),
        .chunk_header_size = read_u16(raw_header_span, 10),
        .block_size = read_u32(raw_header_span, 12),
        .total_blocks = read_u32(raw_header_span, 16),
        .total_chunks = read_u32(raw_header_span, 20),
        .image_checksum = read_u32(raw_header_span, 24),
    };

    if (header.major_version != kAndroidSparseMajorVersion) {
        return std::unexpected(error(
            SparseErrorKind::Unsupported, 4, "unsupported Android sparse major version"));
    }
    if (header.file_header_size < kSparseHeaderSize ||
        header.chunk_header_size < kChunkHeaderSize) {
        return std::unexpected(error(
            SparseErrorKind::Malformed,
            8,
            "sparse file or chunk header is smaller than the version 1 header"));
    }
    if (header.block_size == 0 || header.block_size % 4U != 0) {
        return std::unexpected(error(
            SparseErrorKind::Malformed,
            12,
            "sparse block size must be a non-zero multiple of four"));
    }
    if (header.file_header_size > source_size) {
        return std::unexpected(error(
            SparseErrorKind::Truncated,
            kSparseHeaderSize,
            "sparse file header extension is truncated"));
    }
    if (header.total_chunks > kMaxSparseChunks) {
        return std::unexpected(error(
            SparseErrorKind::Unsupported,
            20,
            "declared sparse chunk count exceeds the metadata safety limit"));
    }

    std::uint64_t declared_output_size = 0;
    if (!checked_multiply(header.total_blocks, header.block_size, declared_output_size)) {
        return std::unexpected(error(
            SparseErrorKind::Malformed, 16, "declared sparse output size overflows 64 bits"));
    }

    const auto available_for_chunks = source_size - header.file_header_size;
    std::uint64_t minimum_chunk_bytes = 0;
    if (!checked_multiply(header.total_chunks, header.chunk_header_size, minimum_chunk_bytes)) {
        return std::unexpected(error(
            SparseErrorKind::Malformed, 20, "minimum sparse chunk table size overflows 64 bits"));
    }
    if (minimum_chunk_bytes > available_for_chunks) {
        return std::unexpected(error(
            SparseErrorKind::Truncated,
            header.file_header_size,
            "sparse input cannot contain the declared number of chunk headers"));
    }

    std::vector<SparseChunk> chunks;
    std::vector<std::size_t> output_chunk_indices;
    const auto maximum_addressable_chunks =
        static_cast<std::uint64_t>(chunks.max_size());
    if (static_cast<std::uint64_t>(header.total_chunks) > maximum_addressable_chunks) {
        return std::unexpected(error(
            SparseErrorKind::Unsupported, 20, "declared sparse chunk count is not addressable"));
    }
    chunks.reserve(header.total_chunks);
    output_chunk_indices.reserve(header.total_chunks);

    std::uint64_t input_offset = header.file_header_size;
    std::uint64_t output_offset = 0;
    std::uint64_t produced_blocks = 0;
    for (std::uint32_t index = 0; index < header.total_chunks; ++index) {
        std::array<std::byte, kChunkHeaderSize> raw_chunk_header{};
        if (const auto read = read_exact(
                *source, source_size, input_offset, raw_chunk_header);
            !read) {
            return std::unexpected(read.error());
        }
        const auto chunk_header_span = std::span<const std::byte>(raw_chunk_header);
        const auto chunk_type = read_u16(chunk_header_span, 0);
        const auto chunk_blocks = read_u32(chunk_header_span, 4);
        const auto chunk_total_size = read_u32(chunk_header_span, 8);

        if (chunk_total_size < header.chunk_header_size) {
            return std::unexpected(error(
                SparseErrorKind::Malformed,
                input_offset + 8,
                "sparse chunk total size is smaller than its chunk header"));
        }

        std::uint64_t chunk_end = 0;
        if (!checked_add(input_offset, chunk_total_size, chunk_end)) {
            return std::unexpected(error(
                SparseErrorKind::Malformed, input_offset, "sparse chunk end offset overflows"));
        }
        if (chunk_end > source_size) {
            return std::unexpected(error(
                SparseErrorKind::Truncated, input_offset, "sparse chunk payload is truncated"));
        }

        std::uint64_t payload_offset = 0;
        if (!checked_add(input_offset, header.chunk_header_size, payload_offset)) {
            return std::unexpected(error(
                SparseErrorKind::Malformed, input_offset, "sparse chunk payload offset overflows"));
        }
        const auto payload_size =
            static_cast<std::uint64_t>(chunk_total_size - header.chunk_header_size);

        SparseChunkKind kind{};
        switch (chunk_type) {
            case kSparseChunkRaw:
                kind = SparseChunkKind::Raw;
                break;
            case kSparseChunkFill:
                kind = SparseChunkKind::Fill;
                break;
            case kSparseChunkDontCare:
                kind = SparseChunkKind::DontCare;
                break;
            case kSparseChunkCrc32:
                kind = SparseChunkKind::Crc32;
                break;
            default:
                return std::unexpected(error(
                    SparseErrorKind::Unsupported,
                    input_offset,
                    "unsupported Android sparse chunk type"));
        }

        std::uint64_t chunk_output_size = 0;
        if (!checked_multiply(chunk_blocks, header.block_size, chunk_output_size)) {
            return std::unexpected(error(
                SparseErrorKind::Malformed,
                input_offset + 4,
                "sparse chunk output size overflows 64 bits"));
        }

        std::optional<std::uint32_t> fill_value;
        std::optional<std::uint32_t> checksum;
        if (kind == SparseChunkKind::Crc32) {
            if (chunk_blocks != 0 || payload_size != 4) {
                return std::unexpected(error(
                    SparseErrorKind::Malformed,
                    input_offset,
                    "CRC32 chunk must contain zero output blocks and four payload bytes"));
            }
            std::array<std::byte, 4> crc_bytes{};
            if (const auto read = read_exact(
                    *source, source_size, payload_offset, crc_bytes);
                !read) {
                return std::unexpected(read.error());
            }
            checksum = read_u32(std::span<const std::byte>(crc_bytes), 0);
        } else {
            if (chunk_blocks == 0) {
                return std::unexpected(error(
                    SparseErrorKind::Malformed,
                    input_offset + 4,
                    "data-producing sparse chunk contains zero output blocks"));
            }

            std::uint64_t output_end = 0;
            if (!checked_add(output_offset, chunk_output_size, output_end)) {
                return std::unexpected(error(
                    SparseErrorKind::Malformed,
                    input_offset + 4,
                    "aggregate sparse output size overflows 64 bits"));
            }
            std::uint64_t next_produced_blocks = 0;
            if (!checked_add(produced_blocks, chunk_blocks, next_produced_blocks) ||
                next_produced_blocks > header.total_blocks || output_end > declared_output_size) {
                return std::unexpected(error(
                    SparseErrorKind::Malformed,
                    input_offset + 4,
                    "sparse chunks produce more blocks than declared"));
            }

            switch (kind) {
                case SparseChunkKind::Raw:
                    if (payload_size != chunk_output_size) {
                        return std::unexpected(error(
                            SparseErrorKind::Malformed,
                            input_offset + 8,
                            "RAW chunk payload size does not match its output blocks"));
                    }
                    break;
                case SparseChunkKind::Fill: {
                    if (payload_size != 4) {
                        return std::unexpected(error(
                            SparseErrorKind::Malformed,
                            input_offset + 8,
                            "FILL chunk must contain exactly four payload bytes"));
                    }
                    std::array<std::byte, 4> fill_bytes{};
                    if (const auto read = read_exact(
                            *source, source_size, payload_offset, fill_bytes);
                        !read) {
                        return std::unexpected(read.error());
                    }
                    fill_value = read_u32(std::span<const std::byte>(fill_bytes), 0);
                    break;
                }
                case SparseChunkKind::DontCare:
                    if (payload_size != 0) {
                        return std::unexpected(error(
                            SparseErrorKind::Malformed,
                            input_offset + 8,
                            "DONT_CARE chunk cannot contain a payload"));
                    }
                    break;
                case SparseChunkKind::Crc32:
                    break;
            }

            output_chunk_indices.push_back(chunks.size());
            output_offset = output_end;
            produced_blocks = next_produced_blocks;
        }

        chunks.push_back(SparseChunk{
            .kind = kind,
            .block_count = chunk_blocks,
            .input_offset = payload_offset,
            .output_offset = output_offset - chunk_output_size,
            .output_size = chunk_output_size,
            .fill_value = fill_value,
            .checksum = checksum,
        });
        input_offset = chunk_end;
    }

    if (input_offset != source_size) {
        return std::unexpected(error(
            SparseErrorKind::Malformed,
            input_offset,
            "sparse input contains trailing bytes after the declared chunks"));
    }
    if (produced_blocks != header.total_blocks || output_offset != declared_output_size) {
        return std::unexpected(error(
            SparseErrorKind::Malformed,
            input_offset,
            "sparse chunks do not produce the declared total block count"));
    }
    if (const auto verified = verify_checksums(
            *source, source_size, header, chunks);
        !verified) {
        return std::unexpected(verified.error());
    }

    return SparseImage(
        std::move(source),
        header,
        declared_output_size,
        std::move(chunks),
        std::move(output_chunk_indices));
}

SparseImage::SparseImage(
    std::shared_ptr<const IImageSource> source,
    SparseHeader header,
    const std::uint64_t output_size,
    std::vector<SparseChunk> chunks,
    std::vector<std::size_t> output_chunk_indices)
    : source_(std::move(source)),
      header_(header),
      output_size_(output_size),
      chunks_(std::move(chunks)),
      output_chunk_indices_(std::move(output_chunk_indices)) {}

const SparseHeader& SparseImage::header() const noexcept {
    return header_;
}

std::span<const SparseChunk> SparseImage::chunks() const noexcept {
    return chunks_;
}

std::uint64_t SparseImage::output_size() const noexcept {
    return output_size_;
}

std::expected<std::size_t, SparseError> SparseImage::read_at(
    const std::uint64_t requested_offset,
    const std::span<std::byte> destination) const {
    if (requested_offset > output_size_) {
        return std::unexpected(error(
            SparseErrorKind::InvalidArgument,
            requested_offset,
            "sparse output read starts beyond end of image"));
    }
    std::uint64_t requested_end = 0;
    if (!checked_add(requested_offset, destination.size(), requested_end)) {
        return std::unexpected(error(
            SparseErrorKind::InvalidArgument,
            requested_offset,
            "sparse output read range overflows 64-bit offset space"));
    }
    const auto available = output_size_ - requested_offset;
    const auto target_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(available, destination.size()));
    if (target_size == 0) {
        return 0;
    }

    auto chunk_position = std::lower_bound(
        output_chunk_indices_.begin(),
        output_chunk_indices_.end(),
        requested_offset,
        [this](const std::size_t index, const std::uint64_t offset) {
            const auto& chunk = chunks_[index];
            return chunk.output_offset + chunk.output_size <= offset;
        });

    std::uint64_t current_offset = requested_offset;
    std::size_t completed = 0;
    while (completed < target_size) {
        if (chunk_position == output_chunk_indices_.end()) {
            return std::unexpected(error(
                SparseErrorKind::Malformed,
                current_offset,
                "validated sparse chunk index has an output gap"));
        }
        const auto& chunk = chunks_[*chunk_position];
        if (current_offset < chunk.output_offset ||
            current_offset >= chunk.output_offset + chunk.output_size) {
            return std::unexpected(error(
                SparseErrorKind::Malformed,
                current_offset,
                "validated sparse chunk index is inconsistent"));
        }

        const auto within_chunk = current_offset - chunk.output_offset;
        const auto chunk_available = chunk.output_size - within_chunk;
        const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
            chunk_available, target_size - completed));
        const auto output = destination.subspan(completed, amount);

        switch (chunk.kind) {
            case SparseChunkKind::Raw: {
                std::uint64_t source_offset = 0;
                if (!checked_add(chunk.input_offset, within_chunk, source_offset)) {
                    return std::unexpected(error(
                        SparseErrorKind::Malformed,
                        chunk.input_offset,
                        "RAW chunk read offset overflows 64-bit space"));
                }
                if (const auto read = read_exact(
                        *source_, source_->size(), source_offset, output);
                    !read) {
                    return std::unexpected(read.error());
                }
                break;
            }
            case SparseChunkKind::Fill: {
                const auto pattern = fill_pattern(*chunk.fill_value);
                for (std::size_t index = 0; index < output.size(); ++index) {
                    output[index] = pattern[(within_chunk + index) % pattern.size()];
                }
                break;
            }
            case SparseChunkKind::DontCare:
                std::ranges::fill(output, std::byte{0});
                break;
            case SparseChunkKind::Crc32:
                return std::unexpected(error(
                    SparseErrorKind::Malformed,
                    current_offset,
                    "CRC32 chunk unexpectedly appeared in the output index"));
        }

        completed += amount;
        current_offset += amount;
        if (within_chunk + amount == chunk.output_size) {
            ++chunk_position;
        }
    }
    return completed;
}

}  // namespace kairosboot::image
