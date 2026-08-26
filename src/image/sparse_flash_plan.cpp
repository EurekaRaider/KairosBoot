// SPDX-License-Identifier: MIT
#include "sparse_flash_plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace kairosboot::image {
namespace {

constexpr std::uint64_t kSparseHeaderBytes = 28;
constexpr std::uint64_t kChunkHeaderBytes = 12;
constexpr std::uint64_t kFillPayloadBytes = 4;
constexpr std::uint64_t kAospResparseReservedOverhead = 56;
constexpr std::uint32_t kRawResparseBlockSize = 4096;
constexpr std::size_t kMaxPlannerFragments = kMaxSparseChunks;

[[nodiscard]] SparseFlashPlanError plan_error(
    const SparseFlashPlanErrorKind kind,
    const std::uint64_t output_offset,
    std::string message) {
    return {
        .kind = kind,
        .output_offset = output_offset,
        .message = std::move(message),
    };
}

[[nodiscard]] SparseFlashPlanError cancelled_error(
    const std::uint64_t output_offset,
    const std::string_view phase) {
    return plan_error(
        SparseFlashPlanErrorKind::Cancelled,
        output_offset,
        "sparse flash planning cancelled while " + std::string(phase));
}

[[nodiscard]] bool checked_add(const std::uint64_t left,
                               const std::uint64_t right,
                               std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(const std::uint64_t left,
                                    const std::uint64_t right,
                                    std::uint64_t& result) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

void write_u16(const std::span<std::byte> output,
               const std::size_t offset,
               const std::uint16_t value) noexcept {
    output[offset] = std::byte{static_cast<unsigned char>(value & 0xFFU)};
    output[offset + 1] =
        std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)};
}

void write_u32(const std::span<std::byte> output,
               const std::size_t offset,
               const std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output[offset + shift / 8] = std::byte{
            static_cast<unsigned char>((value >> shift) & 0xFFU)};
    }
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> input,
    const std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(input[offset]) |
           (std::to_integer<std::uint32_t>(input[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(input[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(input[offset + 3]) << 24U);
}

struct Fragment final {
    SparseChunkKind kind{SparseChunkKind::Raw};
    std::uint32_t start_block{};
    std::uint32_t block_count{};
    std::uint64_t source_offset{};
    std::uint32_t fill_value{};
};

[[nodiscard]] std::uint64_t fragment_end_block(
    const Fragment& fragment) noexcept {
    return static_cast<std::uint64_t>(fragment.start_block) +
           fragment.block_count;
}

[[nodiscard]] std::uint64_t fragment_output_offset(
    const Fragment& fragment,
    const std::uint32_t block_size) noexcept {
    return static_cast<std::uint64_t>(fragment.start_block) * block_size;
}

[[nodiscard]] std::expected<std::uint64_t, SparseFlashPlanError>
fragment_data_bytes(const Fragment& fragment,
                    const std::uint32_t block_size) {
    std::uint64_t bytes = 0;
    if (!checked_multiply(fragment.block_count, block_size, bytes)) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::ArithmeticOverflow,
            fragment_output_offset(fragment, block_size),
            "sparse backed-block length overflows"));
    }
    return bytes;
}

[[nodiscard]] std::expected<std::uint64_t, SparseFlashPlanError>
fragment_encoded_bytes(const Fragment& fragment,
                       const std::uint32_t block_size) {
    if (fragment.kind == SparseChunkKind::Fill) {
        return kChunkHeaderBytes + kFillPayloadBytes;
    }
    auto data_bytes = fragment_data_bytes(fragment, block_size);
    if (!data_bytes) {
        return std::unexpected(std::move(data_bytes.error()));
    }
    std::uint64_t encoded = 0;
    if (!checked_add(kChunkHeaderBytes, *data_bytes, encoded)) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::ArithmeticOverflow,
            fragment_output_offset(fragment, block_size),
            "sparse RAW encoded length overflows"));
    }
    return encoded;
}

[[nodiscard]] std::expected<void, SparseFlashPlanError> append_merged_fragment(
    std::vector<Fragment>& fragments,
    const Fragment& fragment,
    const std::uint32_t block_size) {
    if (fragment.block_count == 0) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::InvalidArgument,
            fragment_output_offset(fragment, block_size),
            "zero-length sparse backed block is invalid"));
    }

    if (!fragments.empty()) {
        auto& previous = fragments.back();
        if (fragment_end_block(previous) == fragment.start_block &&
            previous.kind == fragment.kind) {
            bool merge = false;
            if (fragment.kind == SparseChunkKind::Fill) {
                merge = previous.fill_value == fragment.fill_value;
            } else {
                auto previous_bytes =
                    fragment_data_bytes(previous, block_size);
                if (!previous_bytes) {
                    return std::unexpected(
                        std::move(previous_bytes.error()));
                }
                std::uint64_t expected_source_offset = 0;
                if (!checked_add(previous.source_offset, *previous_bytes,
                                 expected_source_offset)) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::ArithmeticOverflow,
                        fragment_output_offset(previous, block_size),
                        "sparse source interval overflows"));
                }
                merge = expected_source_offset == fragment.source_offset;
            }

            if (merge) {
                const auto merged_blocks =
                    static_cast<std::uint64_t>(previous.block_count) +
                    fragment.block_count;
                if (merged_blocks >
                    std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::ArithmeticOverflow,
                        fragment_output_offset(previous, block_size),
                        "merged sparse block count exceeds 32 bits"));
                }
                previous.block_count =
                    static_cast<std::uint32_t>(merged_blocks);
                return {};
            }
        }
    }

    if (fragments.size() >= kMaxPlannerFragments) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::Unsupported,
            fragment_output_offset(fragment, block_size),
            "sparse planning metadata exceeds its bounded fragment limit"));
    }
    fragments.push_back(fragment);
    return {};
}

[[nodiscard]] std::expected<void, SparseFlashPlanError> read_source_exact(
    const IImageSource& source,
    const std::uint64_t source_offset,
    const std::span<std::byte> destination,
    const std::stop_token cancellation) {
    std::size_t completed = 0;
    while (completed < destination.size()) {
        std::uint64_t current_offset = 0;
        if (!checked_add(source_offset, completed, current_offset)) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                source_offset,
                "raw image read offset overflows"));
        }
        if (cancellation.stop_requested()) {
            return std::unexpected(cancelled_error(
                current_offset, "scanning raw backed blocks"));
        }
        auto read = source.read_at(
            current_offset, destination.subspan(completed));
        if (!read) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Source,
                current_offset,
                "unable to scan raw image: " + read.error().message));
        }
        if (*read == 0 || *read > destination.size() - completed) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Source,
                current_offset,
                "raw image source returned an invalid short read"));
        }
        completed += *read;
        if (cancellation.stop_requested()) {
            return std::unexpected(cancelled_error(
                source_offset + completed, "scanning raw backed blocks"));
        }
    }
    return {};
}

[[nodiscard]] bool block_is_fill(
    const std::span<const std::byte, kRawResparseBlockSize> block,
    std::uint32_t& fill_value) noexcept {
    fill_value = read_u32(block, 0);
    for (std::size_t offset = sizeof(std::uint32_t);
         offset < block.size();
         offset += sizeof(std::uint32_t)) {
        if (read_u32(block, offset) != fill_value) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::expected<std::vector<Fragment>, SparseFlashPlanError>
artifact_fragments(const FlashArtifact& artifact,
                   std::uint32_t& block_size,
                   std::uint32_t& total_blocks,
                   const std::stop_token cancellation) {
    std::vector<Fragment> result;
    if (artifact.metadata().kind == FlashArtifactKind::Raw) {
        block_size = kRawResparseBlockSize;
        if (artifact.metadata().expanded_size % block_size != 0) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Unsupported,
                artifact.metadata().expanded_size,
                "AOSP Fastboot 37.0.1 rejects oversized raw images that are "
                "not 4096-byte aligned"));
        }
        const auto blocks = artifact.metadata().expanded_size / block_size;
        if (blocks > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Unsupported,
                artifact.metadata().expanded_size,
                "raw image is too large for Android sparse block counts"));
        }
        total_blocks = static_cast<std::uint32_t>(blocks);

        std::array<std::byte, kRawResparseBlockSize> block{};
        for (std::uint32_t index = 0; index < total_blocks; ++index) {
            const auto source_offset =
                static_cast<std::uint64_t>(index) * block_size;
            if (cancellation.stop_requested()) {
                return std::unexpected(cancelled_error(
                    source_offset, "scanning raw backed blocks"));
            }
            if (auto read = read_source_exact(
                    *artifact.transfer_source(), source_offset, block,
                    cancellation);
                !read) {
                return std::unexpected(std::move(read.error()));
            }

            std::uint32_t fill_value = 0;
            const auto is_fill = block_is_fill(block, fill_value);
            Fragment fragment{
                .kind = is_fill ? SparseChunkKind::Fill
                                : SparseChunkKind::Raw,
                .start_block = index,
                .block_count = 1,
                .source_offset = source_offset,
                .fill_value = fill_value,
            };
            if (auto appended = append_merged_fragment(
                    result, fragment, block_size);
                !appended) {
                return std::unexpected(std::move(appended.error()));
            }
        }
        return result;
    }

    const auto* sparse = artifact.sparse_image();
    if (sparse == nullptr) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::InvalidArgument,
            0,
            "validated sparse artifact is missing its parsed image"));
    }
    block_size = sparse->header().block_size;
    total_blocks = sparse->header().total_blocks;
    result.reserve(sparse->chunks().size());
    for (const auto& chunk : sparse->chunks()) {
        if (cancellation.stop_requested()) {
            return std::unexpected(cancelled_error(
                chunk.output_offset, "importing sparse backed blocks"));
        }
        if (chunk.kind != SparseChunkKind::Raw &&
            chunk.kind != SparseChunkKind::Fill) {
            continue;
        }
        if (chunk.output_offset % block_size != 0) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::InvalidArgument,
                chunk.output_offset,
                "sparse backed block is not block aligned"));
        }
        if (chunk.kind == SparseChunkKind::Fill &&
            !chunk.fill_value.has_value()) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::InvalidArgument,
                chunk.output_offset,
                "sparse FILL chunk is missing its fill value"));
        }
        Fragment fragment{
            .kind = chunk.kind,
            .start_block = static_cast<std::uint32_t>(
                chunk.output_offset / block_size),
            .block_count = chunk.block_count,
            .source_offset = chunk.input_offset,
            .fill_value = chunk.fill_value.value_or(0),
        };
        if (auto appended = append_merged_fragment(
                result, fragment, block_size);
            !appended) {
            return std::unexpected(std::move(appended.error()));
        }
    }
    return result;
}

enum class SourcePieceKind : std::uint8_t {
    Literal,
    Source,
    Zero,
};

struct SourcePiece final {
    std::uint64_t logical_offset{};
    std::uint64_t backing_offset{};
    std::uint64_t size{};
    SourcePieceKind kind{SourcePieceKind::Literal};
};

class SparsePartSource final : public IImageSource {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<SparsePartSource>,
                                       SparseFlashPlanError>
    create(std::shared_ptr<const IImageSource> input,
           const std::uint32_t block_size,
           const std::uint32_t total_blocks,
           const std::span<const Fragment> fragments,
           const std::stop_token cancellation) {
        try {
            auto result = std::shared_ptr<SparsePartSource>(
                new SparsePartSource(std::move(input)));
            auto built = result->build(
                block_size, total_blocks, fragments, cancellation);
            if (!built) {
                return std::unexpected(std::move(built.error()));
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Source,
                0,
                "unable to allocate bounded sparse flash part metadata"));
        }
    }

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return size_;
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset > size_) {
            return std::unexpected(ImageSourceError{
                .message = "sparse flash part read starts beyond its end",
            });
        }
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(destination.size(), size_ - offset));
        if (requested == 0) {
            return 0;
        }

        auto piece = std::upper_bound(
            pieces_.begin(), pieces_.end(), offset,
            [](const std::uint64_t position, const SourcePiece& candidate) {
                return position < candidate.logical_offset;
            });
        if (piece == pieces_.begin()) {
            return std::unexpected(ImageSourceError{
                .message = "sparse flash part metadata has an uncovered read",
            });
        }
        --piece;

        std::size_t completed = 0;
        while (completed < requested) {
            if (piece == pieces_.end()) {
                return std::unexpected(ImageSourceError{
                    .message =
                        "sparse flash part metadata ended before the read",
                });
            }
            const auto position = offset + completed;
            if (position < piece->logical_offset ||
                position - piece->logical_offset >= piece->size) {
                ++piece;
                continue;
            }
            const auto within = position - piece->logical_offset;
            const auto amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(piece->size - within,
                                        requested - completed));
            auto output = destination.subspan(completed, amount);
            if (piece->kind == SourcePieceKind::Literal) {
                const auto literal_offset =
                    static_cast<std::size_t>(piece->backing_offset + within);
                std::copy_n(
                    literals_.begin() +
                        static_cast<std::ptrdiff_t>(literal_offset),
                    amount,
                    output.begin());
                completed += amount;
            } else if (piece->kind == SourcePieceKind::Zero) {
                std::ranges::fill(output, std::byte{0});
                completed += amount;
            } else {
                auto read = input_->read_at(
                    piece->backing_offset + within, output);
                if (!read) {
                    return std::unexpected(std::move(read.error()));
                }
                if (*read == 0 || *read > amount) {
                    return std::unexpected(ImageSourceError{
                        .message = "sparse flash part backing source returned "
                                   "an invalid short read",
                    });
                }
                completed += *read;
                if (*read < amount) {
                    return completed;
                }
            }
            if (within + amount == piece->size) {
                ++piece;
            }
        }
        return completed;
    }

private:
    explicit SparsePartSource(std::shared_ptr<const IImageSource> input)
        : input_(std::move(input)) {}

    [[nodiscard]] std::expected<void, SparseFlashPlanError> append_piece(
        const SourcePieceKind kind,
        const std::uint64_t backing_offset,
        const std::uint64_t bytes) {
        if (bytes == 0) {
            return {};
        }
        std::uint64_t new_size = 0;
        if (!checked_add(size_, bytes, new_size)) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                size_,
                "sparse part size overflows"));
        }

        if (!pieces_.empty()) {
            auto& previous = pieces_.back();
            std::uint64_t previous_backing_end = 0;
            const auto backing_is_contiguous =
                kind == SourcePieceKind::Zero ||
                (checked_add(previous.backing_offset, previous.size,
                             previous_backing_end) &&
                 previous_backing_end == backing_offset);
            if (previous.kind == kind && backing_is_contiguous &&
                previous.logical_offset + previous.size == size_) {
                previous.size += bytes;
                size_ = new_size;
                return {};
            }
        }

        pieces_.push_back(SourcePiece{
            .logical_offset = size_,
            .backing_offset = backing_offset,
            .size = bytes,
            .kind = kind,
        });
        size_ = new_size;
        return {};
    }

    [[nodiscard]] std::expected<void, SparseFlashPlanError> append_literal(
        const std::span<const std::byte> bytes) {
        std::uint64_t ignored = 0;
        if (!checked_add(size_, bytes.size(), ignored)) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                size_,
                "sparse literal interval overflows"));
        }
        const auto backing_offset =
            static_cast<std::uint64_t>(literals_.size());
        literals_.insert(literals_.end(), bytes.begin(), bytes.end());
        return append_piece(
            SourcePieceKind::Literal, backing_offset, bytes.size());
    }

    [[nodiscard]] std::expected<void, SparseFlashPlanError> append_source(
        const std::uint64_t source_offset,
        const std::uint64_t bytes) {
        if (source_offset > input_->size() ||
            bytes > input_->size() - source_offset) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Source,
                source_offset,
                "sparse RAW backing interval exceeds its source"));
        }
        return append_piece(SourcePieceKind::Source, source_offset, bytes);
    }

    [[nodiscard]] std::expected<void, SparseFlashPlanError> append_zero(
        const std::uint64_t bytes) {
        return append_piece(SourcePieceKind::Zero, 0, bytes);
    }

    [[nodiscard]] std::expected<void, SparseFlashPlanError>
    append_chunk_header(const std::uint16_t type,
                        const std::uint32_t block_count,
                        const std::uint32_t payload_bytes) {
        std::array<std::byte, kChunkHeaderBytes> bytes{};
        write_u16(bytes, 0, type);
        write_u16(bytes, 2, 0);
        write_u32(bytes, 4, block_count);
        if (payload_bytes >
            std::numeric_limits<std::uint32_t>::max() -
                kChunkHeaderBytes) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                size_,
                "sparse chunk total size exceeds 32 bits"));
        }
        write_u32(
            bytes,
            8,
            static_cast<std::uint32_t>(kChunkHeaderBytes) + payload_bytes);
        return append_literal(bytes);
    }

    [[nodiscard]] std::expected<void, SparseFlashPlanError> build(
        const std::uint32_t block_size,
        const std::uint32_t total_blocks,
        const std::span<const Fragment> fragments,
        const std::stop_token cancellation) {
        if (cancellation.stop_requested()) {
            return std::unexpected(cancelled_error(
                0, "building sparse part metadata"));
        }

        if (fragments.size() >
            (std::numeric_limits<std::size_t>::max() - 3) / 2) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                0,
                "sparse part descriptor count overflows"));
        }
        pieces_.reserve(fragments.size() * 2 + 3);

        std::uint64_t chunk_count = 0;
        std::uint64_t cursor = 0;
        for (const auto& fragment : fragments) {
            if (fragment.start_block > cursor) {
                ++chunk_count;
            }
            ++chunk_count;
            cursor = fragment_end_block(fragment);
        }
        if (cursor < total_blocks) {
            ++chunk_count;
        }
        if (chunk_count > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                0,
                "sparse output chunk count exceeds 32 bits"));
        }

        std::uint64_t literal_reserve = kSparseHeaderBytes;
        std::uint64_t maximum_chunk_headers = 0;
        if (!checked_multiply(fragments.size(), 2, maximum_chunk_headers) ||
            !checked_add(maximum_chunk_headers, 1,
                         maximum_chunk_headers) ||
            !checked_multiply(maximum_chunk_headers, kChunkHeaderBytes,
                              maximum_chunk_headers) ||
            !checked_add(literal_reserve, maximum_chunk_headers,
                         literal_reserve)) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                0,
                "sparse literal metadata size overflows"));
        }
        std::uint64_t maximum_fill_bytes = 0;
        if (!checked_multiply(fragments.size(), kFillPayloadBytes,
                              maximum_fill_bytes) ||
            !checked_add(literal_reserve, maximum_fill_bytes,
                         literal_reserve) ||
            literal_reserve > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                0,
                "sparse literal metadata is not addressable"));
        }
        literals_.reserve(static_cast<std::size_t>(literal_reserve));

        std::array<std::byte, kSparseHeaderBytes> header{};
        write_u32(header, 0, kAndroidSparseMagic);
        write_u16(header, 4, kAndroidSparseMajorVersion);
        write_u16(header, 6, 0);
        write_u16(
            header, 8, static_cast<std::uint16_t>(kSparseHeaderBytes));
        write_u16(
            header, 10, static_cast<std::uint16_t>(kChunkHeaderBytes));
        write_u32(header, 12, block_size);
        write_u32(header, 16, total_blocks);
        write_u32(header, 20, static_cast<std::uint32_t>(chunk_count));
        write_u32(header, 24, 0);
        if (auto appended = append_literal(header); !appended) {
            return appended;
        }

        cursor = 0;
        for (const auto& fragment : fragments) {
            if (cancellation.stop_requested()) {
                return std::unexpected(cancelled_error(
                    fragment_output_offset(fragment, block_size),
                    "building sparse part metadata"));
            }
            if (fragment.start_block > cursor) {
                if (auto appended = append_chunk_header(
                        kSparseChunkDontCare,
                        static_cast<std::uint32_t>(
                            fragment.start_block - cursor),
                        0);
                    !appended) {
                    return appended;
                }
            }

            auto data_bytes = fragment_data_bytes(fragment, block_size);
            if (!data_bytes) {
                return std::unexpected(std::move(data_bytes.error()));
            }
            if (fragment.kind == SparseChunkKind::Raw) {
                if (*data_bytes >
                    std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::ArithmeticOverflow,
                        fragment_output_offset(fragment, block_size),
                        "sparse RAW payload exceeds 32 bits"));
                }
                if (auto appended = append_chunk_header(
                        kSparseChunkRaw,
                        fragment.block_count,
                        static_cast<std::uint32_t>(*data_bytes));
                    !appended) {
                    return appended;
                }
                if (auto appended = append_source(
                        fragment.source_offset, *data_bytes);
                    !appended) {
                    return appended;
                }
                const auto rounded_bytes =
                    static_cast<std::uint64_t>(fragment.block_count) *
                    block_size;
                if (rounded_bytes > *data_bytes) {
                    if (auto appended =
                            append_zero(rounded_bytes - *data_bytes);
                        !appended) {
                        return appended;
                    }
                }
            } else {
                if (auto appended = append_chunk_header(
                        kSparseChunkFill, fragment.block_count, 4);
                    !appended) {
                    return appended;
                }
                std::array<std::byte, 4> fill{};
                write_u32(fill, 0, fragment.fill_value);
                if (auto appended = append_literal(fill); !appended) {
                    return appended;
                }
            }
            cursor = fragment_end_block(fragment);
        }
        if (cursor < total_blocks) {
            if (auto appended = append_chunk_header(
                    kSparseChunkDontCare,
                    static_cast<std::uint32_t>(total_blocks - cursor),
                    0);
                !appended) {
                return appended;
            }
        }
        return {};
    }

    std::shared_ptr<const IImageSource> input_;
    std::vector<SourcePiece> pieces_;
    std::vector<std::byte> literals_;
    std::uint64_t size_{};
};

[[nodiscard]] std::expected<std::uint64_t, SparseFlashPlanError>
counter_bytes_for_fragment(const Fragment& fragment,
                           const std::uint64_t last_block,
                           const std::uint32_t block_size) {
    auto count = fragment_encoded_bytes(fragment, block_size);
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    if (fragment.start_block > last_block &&
        !checked_add(*count, kChunkHeaderBytes, *count)) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::ArithmeticOverflow,
            fragment_output_offset(fragment, block_size),
            "sparse gap metadata size overflows"));
    }
    return count;
}

[[nodiscard]] std::expected<Fragment, SparseFlashPlanError> take_prefix(
    Fragment& remainder,
    const std::uint64_t available_bytes,
    const std::uint32_t block_size) {
    const auto available_blocks = available_bytes / block_size;
    const auto prefix_blocks = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(available_blocks, remainder.block_count));
    if (prefix_blocks == 0) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::Unsupported,
            fragment_output_offset(remainder, block_size),
            "device maximum download size cannot hold one AOSP sparse "
            "backed block"));
    }

    auto prefix = remainder;
    prefix.block_count = prefix_blocks;
    if (prefix_blocks == remainder.block_count) {
        remainder.block_count = 0;
        return prefix;
    }

    const auto prefix_bytes =
        static_cast<std::uint64_t>(prefix_blocks) * block_size;
    remainder.start_block += prefix_blocks;
    remainder.block_count -= prefix_blocks;
    if (remainder.kind == SparseChunkKind::Raw) {
        if (!checked_add(remainder.source_offset, prefix_bytes,
                         remainder.source_offset)) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                fragment_output_offset(prefix, block_size),
                "split sparse RAW source offset overflows"));
        }
    }
    return prefix;
}

struct Segmentation final {
    std::vector<Fragment> fragments;
    std::size_t part_count{};
};

// AOSP fastboot calls sparse_file_resparse twice. The first counting pass
// permanently splits backed blocks, restores them in order, and the second pass
// serializes that segmented list. Reproducing this detail is required for exact
// chunk boundaries rather than merely producing equivalent sparse images.
[[nodiscard]] std::expected<Segmentation, SparseFlashPlanError>
segment_like_aosp_first_pass(const std::span<const Fragment> input,
                             const std::uint64_t payload_budget,
                             const std::uint32_t block_size,
                             const std::stop_token cancellation) {
    Segmentation result;
    result.fragments.reserve(input.size());
    if (input.empty()) {
        result.part_count = 1;
        return result;
    }

    std::size_t input_index = 0;
    std::optional<Fragment> pending;
    while (pending.has_value() || input_index < input.size()) {
        bool has_last = false;
        bool part_counted = false;
        bool trailing_empty_part = false;
        std::uint64_t file_len = 0;
        std::uint64_t last_block = 0;

        while (pending.has_value() || input_index < input.size()) {
            if (!pending.has_value()) {
                pending = input[input_index++];
            }
            auto& fragment = *pending;
            const auto current_output_offset =
                fragment_output_offset(fragment, block_size);
            if (cancellation.stop_requested()) {
                return std::unexpected(cancelled_error(
                    fragment_output_offset(fragment, block_size),
                    "computing AOSP resparse boundaries"));
            }

            auto count = counter_bytes_for_fragment(
                fragment, last_block, block_size);
            if (!count) {
                return std::unexpected(std::move(count.error()));
            }
            std::uint64_t candidate_size = 0;
            if (!checked_add(file_len, *count, candidate_size)) {
                return std::unexpected(plan_error(
                    SparseFlashPlanErrorKind::ArithmeticOverflow,
                    fragment_output_offset(fragment, block_size),
                    "AOSP sparse counter size overflows"));
            }
            if (candidate_size <= payload_budget) {
                if (result.fragments.size() >= kMaxPlannerFragments) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::Unsupported,
                        fragment_output_offset(fragment, block_size),
                        "AOSP resparse segmentation exceeds its bounded "
                        "metadata limit"));
                }
                result.fragments.push_back(fragment);
                file_len = candidate_size;
                last_block = fragment_end_block(fragment);
                has_last = true;
                pending.reset();
                continue;
            }

            std::uint64_t split_accounting = 0;
            if (!checked_add(file_len, kChunkHeaderBytes,
                             split_accounting)) {
                return std::unexpected(plan_error(
                    SparseFlashPlanErrorKind::ArithmeticOverflow,
                    fragment_output_offset(fragment, block_size),
                    "AOSP sparse split accounting overflows"));
            }
            const auto remaining = split_accounting <= payload_budget
                ? payload_budget - split_accounting
                : 0;
            const auto should_split = !has_last ||
                (split_accounting <= payload_budget &&
                 remaining > payload_budget / 8);
            if (should_split) {
                if (split_accounting > payload_budget) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::Unsupported,
                        fragment_output_offset(fragment, block_size),
                        "device maximum download size is too small for AOSP "
                        "resparse overhead"));
                }
                auto prefix = take_prefix(
                    fragment, remaining, block_size);
                if (!prefix) {
                    return std::unexpected(std::move(prefix.error()));
                }
                if (result.fragments.size() >= kMaxPlannerFragments) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::Unsupported,
                        fragment_output_offset(*prefix, block_size),
                        "AOSP resparse segmentation exceeds its bounded "
                        "metadata limit"));
                }
                result.fragments.push_back(*prefix);
                has_last = true;
                const auto consumed_whole = fragment.block_count == 0;
                if (fragment.block_count == 0) {
                    pending.reset();
                }
                trailing_empty_part =
                    consumed_whole && input_index == input.size();
            }

            if (!has_last) {
                return std::unexpected(plan_error(
                    SparseFlashPlanErrorKind::Unsupported,
                    fragment_output_offset(fragment, block_size),
                    "AOSP resparse pass made no forward progress"));
            }
            if (result.part_count >= kMaxPlannerFragments) {
                return std::unexpected(plan_error(
                    SparseFlashPlanErrorKind::Unsupported,
                    current_output_offset,
                    "AOSP resparse part count exceeds its bounded limit"));
            }
            ++result.part_count;
            part_counted = true;
            break;
        }

        if (!pending.has_value() && input_index == input.size()) {
            if (has_last && !part_counted) {
                if (result.part_count >= kMaxPlannerFragments) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::Unsupported,
                        0,
                        "AOSP resparse part count exceeds its bounded limit"));
                }
                ++result.part_count;
            }
            if (trailing_empty_part) {
                if (result.part_count >= kMaxPlannerFragments) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::Unsupported,
                        0,
                        "AOSP resparse part count exceeds its bounded limit"));
                }
                ++result.part_count;
            }
            break;
        }
    }
    return result;
}

[[nodiscard]] std::expected<SparseFlashPart, SparseFlashPlanError>
build_part(const std::shared_ptr<const IImageSource>& input,
           const std::span<const Fragment> fragments,
           const std::uint32_t block_size,
           const std::uint32_t total_blocks,
           const std::stop_token cancellation) {
    auto source = SparsePartSource::create(
        input, block_size, total_blocks, fragments, cancellation);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    std::uint64_t first = 0;
    std::uint64_t end = 0;
    if (!fragments.empty()) {
        first = fragment_output_offset(fragments.front(), block_size);
        end = fragment_end_block(fragments.back()) * block_size;
    }
    return SparseFlashPart{
        .source = std::move(*source),
        .first_data_offset = first,
        .data_end_offset = end,
    };
}

}  // namespace

std::expected<SparseFlashPlan, SparseFlashPlanError>
SparseFlashPlan::create_impl(
    const FlashArtifact& artifact,
    const std::uint64_t target_max_download_size,
    const std::uint64_t host_resparse_limit,
    const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        return std::unexpected(cancelled_error(0, "starting"));
    }

    const auto protocol_limit = static_cast<std::uint64_t>(
        std::numeric_limits<std::uint32_t>::max());
    const auto target_limit = target_max_download_size == 0
        ? protocol_limit
        : std::min(target_max_download_size, protocol_limit);

    // This is deliberately tested before applying the 1 GiB host cap. AOSP
    // only invokes resparse when the source's encoded length exceeds the
    // target-reported limit; a fitting payload is preserved byte-for-byte.
    if (artifact.metadata().transfer_size <= target_limit) {
        std::vector<SparseFlashPart> parts;
        parts.push_back(SparseFlashPart{
            .source = artifact.transfer_source(),
            .first_data_offset = 0,
            .data_end_offset = artifact.metadata().expanded_size,
        });
        return SparseFlashPlan(
            std::move(parts), false, artifact.metadata().expanded_size,
            artifact.metadata().transfer_size);
    }
    if (host_resparse_limit == 0) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::InvalidArgument,
            0,
            "host sparse split limit must not be zero"));
    }
    const auto effective_limit =
        std::min(target_limit, host_resparse_limit);
    if (effective_limit <= kAospResparseReservedOverhead) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::Unsupported,
            0,
            "device maximum download size cannot hold AOSP sparse "
            "resparse overhead"));
    }
    const auto payload_budget =
        effective_limit - kAospResparseReservedOverhead;

    std::uint32_t block_size = 0;
    std::uint32_t total_blocks = 0;
    auto initial_fragments = artifact_fragments(
        artifact, block_size, total_blocks, cancellation);
    if (!initial_fragments) {
        return std::unexpected(std::move(initial_fragments.error()));
    }

    auto segmentation = segment_like_aosp_first_pass(
        *initial_fragments, payload_budget, block_size, cancellation);
    if (!segmentation) {
        return std::unexpected(std::move(segmentation.error()));
    }

    std::vector<SparseFlashPart> parts;
    parts.reserve(segmentation->part_count);
    std::vector<Fragment> current;
    current.reserve(segmentation->fragments.size());
    std::uint64_t total_transfer_size = 0;

    const auto flush = [&]() -> std::expected<void, SparseFlashPlanError> {
        if (current.empty()) {
            return {};
        }
        auto part = build_part(
            artifact.transfer_source(), current, block_size, total_blocks,
            cancellation);
        if (!part) {
            return std::unexpected(std::move(part.error()));
        }
        if (part->source->size() > effective_limit) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Unsupported,
                part->first_data_offset,
                "AOSP sparse part exceeds the effective download limit"));
        }
        if (!checked_add(total_transfer_size, part->source->size(),
                         total_transfer_size)) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                part->first_data_offset,
                "aggregate sparse transfer size overflows"));
        }
        parts.push_back(std::move(*part));
        current.clear();
        return {};
    };

    if (segmentation->fragments.empty()) {
        auto empty = build_part(
            artifact.transfer_source(), {}, block_size, total_blocks,
            cancellation);
        if (!empty) {
            return std::unexpected(std::move(empty.error()));
        }
        if (empty->source->size() > effective_limit) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Unsupported,
                0,
                "device maximum download size cannot hold sparse metadata"));
        }
        total_transfer_size = empty->source->size();
        parts.push_back(std::move(*empty));
    } else {
        std::size_t fragment_index = 0;
        std::optional<Fragment> pending;
        while (pending.has_value() ||
               fragment_index < segmentation->fragments.size()) {
            bool has_last = false;
            bool trailing_empty_part = false;
            std::uint64_t file_len = 0;
            std::uint64_t last_block = 0;

            while (pending.has_value() ||
                   fragment_index < segmentation->fragments.size()) {
                if (!pending.has_value()) {
                    pending =
                        segmentation->fragments[fragment_index++];
                }
                auto& fragment = *pending;
                if (cancellation.stop_requested()) {
                    return std::unexpected(cancelled_error(
                        fragment_output_offset(fragment, block_size),
                        "serializing AOSP resparse parts"));
                }

                auto count = counter_bytes_for_fragment(
                    fragment, last_block, block_size);
                if (!count) {
                    return std::unexpected(std::move(count.error()));
                }
                std::uint64_t candidate_size = 0;
                if (!checked_add(file_len, *count, candidate_size)) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::ArithmeticOverflow,
                        fragment_output_offset(fragment, block_size),
                        "AOSP sparse counter size overflows"));
                }
                if (candidate_size <= payload_budget) {
                    current.push_back(fragment);
                    file_len = candidate_size;
                    last_block = fragment_end_block(fragment);
                    has_last = true;
                    pending.reset();
                    continue;
                }

                std::uint64_t split_accounting = 0;
                if (!checked_add(file_len, kChunkHeaderBytes,
                                 split_accounting)) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::ArithmeticOverflow,
                        fragment_output_offset(fragment, block_size),
                        "AOSP sparse split accounting overflows"));
                }
                const auto remaining = split_accounting <= payload_budget
                    ? payload_budget - split_accounting
                    : 0;
                const auto should_split = !has_last ||
                    (split_accounting <= payload_budget &&
                     remaining > payload_budget / 8);
                if (should_split) {
                    if (split_accounting > payload_budget) {
                        return std::unexpected(plan_error(
                            SparseFlashPlanErrorKind::Unsupported,
                            fragment_output_offset(fragment, block_size),
                            "device maximum download size is too small for "
                            "AOSP resparse overhead"));
                    }
                    auto prefix = take_prefix(
                        fragment, remaining, block_size);
                    if (!prefix) {
                        return std::unexpected(std::move(prefix.error()));
                    }
                    current.push_back(*prefix);
                    has_last = true;
                    const auto consumed_whole = fragment.block_count == 0;
                    if (fragment.block_count == 0) {
                        pending.reset();
                    }
                    trailing_empty_part = consumed_whole &&
                        fragment_index == segmentation->fragments.size();
                }
                if (!has_last) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::Unsupported,
                        fragment_output_offset(fragment, block_size),
                        "AOSP resparse pass made no forward progress"));
                }
                if (auto flushed = flush(); !flushed) {
                    return std::unexpected(std::move(flushed.error()));
                }
                if (trailing_empty_part) {
                    auto empty = build_part(
                        artifact.transfer_source(), {}, block_size,
                        total_blocks, cancellation);
                    if (!empty) {
                        return std::unexpected(std::move(empty.error()));
                    }
                    if (empty->source->size() > effective_limit) {
                        return std::unexpected(plan_error(
                            SparseFlashPlanErrorKind::Unsupported,
                            0,
                            "device maximum download size cannot hold "
                            "sparse metadata"));
                    }
                    if (!checked_add(
                            total_transfer_size, empty->source->size(),
                            total_transfer_size)) {
                        return std::unexpected(plan_error(
                            SparseFlashPlanErrorKind::ArithmeticOverflow,
                            0,
                            "aggregate sparse transfer size overflows"));
                    }
                    parts.push_back(std::move(*empty));
                }
                break;
            }

            if (!pending.has_value() &&
                fragment_index == segmentation->fragments.size()) {
                if (auto flushed = flush(); !flushed) {
                    return std::unexpected(std::move(flushed.error()));
                }
                break;
            }
        }
    }

    if (parts.size() != segmentation->part_count) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::Unsupported,
            0,
            "AOSP resparse counting and serialization passes disagree"));
    }
    return SparseFlashPlan(
        std::move(parts), true, artifact.metadata().expanded_size,
        total_transfer_size);
}

std::expected<SparseFlashPlan, SparseFlashPlanError> SparseFlashPlan::create(
    const FlashArtifact& artifact,
    const std::uint64_t target_max_download_size,
    const std::uint64_t host_resparse_limit,
    const std::stop_token cancellation) {
    try {
        return create_impl(
            artifact, target_max_download_size, host_resparse_limit,
            cancellation);
    } catch (const std::bad_alloc&) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::Source,
            0,
            "unable to allocate bounded sparse flash planning metadata"));
    }
}

SparseFlashPlan::SparseFlashPlan(std::vector<SparseFlashPart> parts,
                                 const bool reparsed,
                                 const std::uint64_t expanded_size,
                                 const std::uint64_t transfer_size) noexcept
    : parts_(std::move(parts)),
      reparsed_(reparsed),
      expanded_size_(expanded_size),
      transfer_size_(transfer_size) {}

std::span<const SparseFlashPart> SparseFlashPlan::parts() const noexcept {
    return parts_;
}

bool SparseFlashPlan::reparsed() const noexcept {
    return reparsed_;
}

std::uint64_t SparseFlashPlan::expanded_size() const noexcept {
    return expanded_size_;
}

std::uint64_t SparseFlashPlan::transfer_size() const noexcept {
    return transfer_size_;
}

}  // namespace kairosboot::image
