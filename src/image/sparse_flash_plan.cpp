// SPDX-License-Identifier: MIT
#include "sparse_flash_plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace kairosboot::image {
namespace {

constexpr std::uint64_t kSparseHeaderBytes = 28;
constexpr std::uint64_t kChunkHeaderBytes = 12;
constexpr std::uint32_t kRawResparseBlockSize = 4096;

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

void append_u16(std::vector<std::byte>& output, const std::uint16_t value) {
    output.push_back(std::byte{static_cast<unsigned char>(value & 0xFFU)});
    output.push_back(
        std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)});
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(std::byte{
            static_cast<unsigned char>((value >> shift) & 0xFFU)});
    }
}

struct Fragment final {
    SparseChunkKind kind{SparseChunkKind::Raw};
    std::uint32_t start_block{};
    std::uint32_t block_count{};
    std::uint64_t source_offset{};
    std::optional<std::uint32_t> fill_value;
};

[[nodiscard]] std::uint64_t fragment_end_block(
    const Fragment& fragment) noexcept {
    return static_cast<std::uint64_t>(fragment.start_block) +
           fragment.block_count;
}

[[nodiscard]] std::expected<std::uint64_t, SparseFlashPlanError>
encoded_size(const std::span<const Fragment> fragments,
             const std::uint32_t total_blocks,
             const std::uint32_t block_size) {
    std::uint64_t size = kSparseHeaderBytes;
    std::uint64_t cursor = 0;
    for (const auto& fragment : fragments) {
        if (fragment.start_block > cursor &&
            !checked_add(size, kChunkHeaderBytes, size)) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                cursor * block_size,
                "sparse part metadata size overflows"));
        }
        if (!checked_add(size, kChunkHeaderBytes, size)) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::ArithmeticOverflow,
                static_cast<std::uint64_t>(fragment.start_block) * block_size,
                "sparse part metadata size overflows"));
        }
        if (fragment.kind == SparseChunkKind::Raw) {
            std::uint64_t payload = 0;
            if (!checked_multiply(fragment.block_count, block_size, payload) ||
                !checked_add(size, payload, size)) {
                return std::unexpected(plan_error(
                    SparseFlashPlanErrorKind::ArithmeticOverflow,
                    static_cast<std::uint64_t>(fragment.start_block) *
                        block_size,
                    "sparse RAW payload size overflows"));
            }
        } else if (fragment.kind == SparseChunkKind::Fill) {
            if (!checked_add(size, 4, size)) {
                return std::unexpected(plan_error(
                    SparseFlashPlanErrorKind::ArithmeticOverflow,
                    static_cast<std::uint64_t>(fragment.start_block) *
                        block_size,
                    "sparse FILL payload size overflows"));
            }
        }
        cursor = fragment_end_block(fragment);
    }
    if (cursor < total_blocks &&
        !checked_add(size, kChunkHeaderBytes, size)) {
        return std::unexpected(plan_error(
            SparseFlashPlanErrorKind::ArithmeticOverflow,
            cursor * block_size,
            "sparse trailing metadata size overflows"));
    }
    return size;
}

struct SourcePiece final {
    std::uint64_t logical_offset{};
    std::vector<std::byte> literal;
    std::shared_ptr<const IImageSource> source;
    std::uint64_t source_offset{};
    std::uint64_t size{};
};

class SparsePartSource final : public IImageSource {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<SparsePartSource>,
                                       SparseFlashPlanError>
    create(std::shared_ptr<const IImageSource> input,
           const std::uint32_t block_size,
           const std::uint32_t total_blocks,
           const std::span<const Fragment> fragments) {
        try {
            auto result = std::shared_ptr<SparsePartSource>(
                new SparsePartSource(std::move(input)));
            auto built = result->build(block_size, total_blocks, fragments);
            if (!built) {
                return std::unexpected(std::move(built.error()));
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Source,
                0,
                "unable to allocate sparse flash part metadata"));
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
        if (piece != pieces_.begin()) {
            --piece;
        }

        std::size_t completed = 0;
        while (completed < requested && piece != pieces_.end()) {
            const auto position = offset + completed;
            if (position < piece->logical_offset ||
                position >= piece->logical_offset + piece->size) {
                ++piece;
                continue;
            }
            const auto within = position - piece->logical_offset;
            const auto amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(piece->size - within,
                                        requested - completed));
            if (!piece->literal.empty()) {
                std::ranges::copy_n(
                    piece->literal.begin() +
                        static_cast<std::ptrdiff_t>(within),
                    amount,
                    destination.begin() +
                        static_cast<std::ptrdiff_t>(completed));
                completed += amount;
            } else {
                auto read = piece->source->read_at(
                    piece->source_offset + within,
                    destination.subspan(completed, amount));
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

    void append_literal(std::vector<std::byte> bytes) {
        const auto piece_size = static_cast<std::uint64_t>(bytes.size());
        pieces_.push_back(SourcePiece{
            .logical_offset = size_,
            .literal = std::move(bytes),
            .source = {},
            .source_offset = 0,
            .size = piece_size,
        });
        size_ += piece_size;
    }

    void append_source(const std::uint64_t source_offset,
                       const std::uint64_t bytes) {
        pieces_.push_back(SourcePiece{
            .logical_offset = size_,
            .literal = {},
            .source = input_,
            .source_offset = source_offset,
            .size = bytes,
        });
        size_ += bytes;
    }

    void append_chunk_header(const std::uint16_t type,
                             const std::uint32_t block_count,
                             const std::uint32_t payload_bytes) {
        std::vector<std::byte> bytes;
        bytes.reserve(kChunkHeaderBytes);
        append_u16(bytes, type);
        append_u16(bytes, 0);
        append_u32(bytes, block_count);
        append_u32(bytes,
                   static_cast<std::uint32_t>(kChunkHeaderBytes) +
                       payload_bytes);
        append_literal(std::move(bytes));
    }

    [[nodiscard]] std::expected<void, SparseFlashPlanError> build(
        const std::uint32_t block_size,
        const std::uint32_t total_blocks,
        const std::span<const Fragment> fragments) {
        std::uint32_t chunk_count = 0;
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

        std::vector<std::byte> header;
        header.reserve(kSparseHeaderBytes);
        append_u32(header, kAndroidSparseMagic);
        append_u16(header, kAndroidSparseMajorVersion);
        append_u16(header, 0);
        append_u16(header, static_cast<std::uint16_t>(kSparseHeaderBytes));
        append_u16(header, static_cast<std::uint16_t>(kChunkHeaderBytes));
        append_u32(header, block_size);
        append_u32(header, total_blocks);
        append_u32(header, chunk_count);
        append_u32(header, 0);
        append_literal(std::move(header));

        cursor = 0;
        for (const auto& fragment : fragments) {
            if (fragment.start_block > cursor) {
                append_chunk_header(
                    kSparseChunkDontCare,
                    static_cast<std::uint32_t>(fragment.start_block - cursor),
                    0);
            }
            if (fragment.kind == SparseChunkKind::Raw) {
                std::uint64_t raw_bytes = 0;
                if (!checked_multiply(fragment.block_count, block_size,
                                      raw_bytes) ||
                    raw_bytes > std::numeric_limits<std::uint32_t>::max()) {
                    return std::unexpected(plan_error(
                        SparseFlashPlanErrorKind::ArithmeticOverflow,
                        static_cast<std::uint64_t>(fragment.start_block) *
                            block_size,
                        "sparse RAW chunk exceeds the format's 32-bit size"));
                }
                append_chunk_header(
                    kSparseChunkRaw,
                    fragment.block_count,
                    static_cast<std::uint32_t>(raw_bytes));
                append_source(fragment.source_offset, raw_bytes);
            } else {
                append_chunk_header(kSparseChunkFill, fragment.block_count, 4);
                std::vector<std::byte> fill;
                fill.reserve(4);
                append_u32(fill, fragment.fill_value.value_or(0));
                append_literal(std::move(fill));
            }
            cursor = fragment_end_block(fragment);
        }
        if (cursor < total_blocks) {
            append_chunk_header(
                kSparseChunkDontCare,
                static_cast<std::uint32_t>(total_blocks - cursor),
                0);
        }
        return {};
    }

    std::shared_ptr<const IImageSource> input_;
    std::vector<SourcePiece> pieces_;
    std::uint64_t size_{};
};

[[nodiscard]] std::expected<std::vector<Fragment>, SparseFlashPlanError>
artifact_fragments(const FlashArtifact& artifact,
                   std::uint32_t& block_size,
                   std::uint32_t& total_blocks) {
    std::vector<Fragment> result;
    if (artifact.metadata().kind == FlashArtifactKind::Raw) {
        block_size = kRawResparseBlockSize;
        if (artifact.metadata().expanded_size % block_size != 0) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Unsupported,
                artifact.metadata().expanded_size,
                "raw images must be 4096-byte aligned before they can be "
                "split into Android sparse downloads"));
        }
        const auto blocks = artifact.metadata().expanded_size / block_size;
        if (blocks > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Unsupported,
                artifact.metadata().expanded_size,
                "raw image is too large for Android sparse block counts"));
        }
        total_blocks = static_cast<std::uint32_t>(blocks);
        if (total_blocks != 0) {
            result.push_back(Fragment{
                .kind = SparseChunkKind::Raw,
                .start_block = 0,
                .block_count = total_blocks,
                .source_offset = 0,
            });
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
        if (chunk.kind != SparseChunkKind::Raw &&
            chunk.kind != SparseChunkKind::Fill) {
            continue;
        }
        result.push_back(Fragment{
            .kind = chunk.kind,
            .start_block = static_cast<std::uint32_t>(
                chunk.output_offset / block_size),
            .block_count = chunk.block_count,
            .source_offset = chunk.input_offset,
            .fill_value = chunk.fill_value,
        });
    }
    return result;
}

[[nodiscard]] std::expected<std::uint32_t, SparseFlashPlanError>
largest_raw_prefix(const std::span<const Fragment> current,
                   const Fragment& remainder,
                   const std::uint32_t total_blocks,
                   const std::uint32_t block_size,
                   const std::uint64_t limit) {
    std::uint32_t low = 0;
    std::uint32_t high = remainder.block_count;
    std::vector<Fragment> candidate(current.begin(), current.end());
    while (low < high) {
        const auto middle = static_cast<std::uint32_t>(
            low + (static_cast<std::uint64_t>(high) - low + 1) / 2);
        candidate.resize(current.size());
        auto prefix = remainder;
        prefix.block_count = middle;
        candidate.push_back(prefix);
        auto size = encoded_size(candidate, total_blocks, block_size);
        if (!size) {
            return std::unexpected(std::move(size.error()));
        }
        if (*size <= limit) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return low;
}

[[nodiscard]] std::expected<SparseFlashPart, SparseFlashPlanError>
build_part(const std::shared_ptr<const IImageSource>& input,
           const std::span<const Fragment> fragments,
           const std::uint32_t block_size,
           const std::uint32_t total_blocks) {
    auto source = SparsePartSource::create(
        input, block_size, total_blocks, fragments);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    std::uint64_t first = 0;
    std::uint64_t end = 0;
    if (!fragments.empty()) {
        first = static_cast<std::uint64_t>(fragments.front().start_block) *
                block_size;
        end = fragment_end_block(fragments.back()) * block_size;
    }
    return SparseFlashPart{
        .source = std::move(*source),
        .first_data_offset = first,
        .data_end_offset = end,
    };
}

}  // namespace

std::expected<SparseFlashPlan, SparseFlashPlanError> SparseFlashPlan::create(
    const FlashArtifact& artifact,
    const std::uint64_t target_max_download_size,
    const std::uint64_t host_resparse_limit) {
    const auto protocol_limit = static_cast<std::uint64_t>(
        std::numeric_limits<std::uint32_t>::max());
    const auto target_limit = target_max_download_size == 0
        ? protocol_limit
        : std::min(target_max_download_size, protocol_limit);
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
    const auto effective_limit = std::min(target_limit, host_resparse_limit);

    std::uint32_t block_size = 0;
    std::uint32_t total_blocks = 0;
    auto fragments = artifact_fragments(artifact, block_size, total_blocks);
    if (!fragments) {
        return std::unexpected(std::move(fragments.error()));
    }

    std::vector<SparseFlashPart> parts;
    std::vector<Fragment> current;
    std::uint64_t total_transfer_size = 0;
    const auto flush = [&]() -> std::expected<void, SparseFlashPlanError> {
        if (current.empty()) {
            return {};
        }
        auto part = build_part(artifact.transfer_source(), current, block_size,
                               total_blocks);
        if (!part) {
            return std::unexpected(std::move(part.error()));
        }
        if (part->source->size() > effective_limit) {
            return std::unexpected(plan_error(
                SparseFlashPlanErrorKind::Unsupported,
                part->first_data_offset,
                "device maximum download size cannot hold one sparse part"));
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

    for (auto fragment : *fragments) {
        while (fragment.block_count != 0) {
            std::vector<Fragment> candidate = current;
            candidate.push_back(fragment);
            auto candidate_size =
                encoded_size(candidate, total_blocks, block_size);
            if (!candidate_size) {
                return std::unexpected(std::move(candidate_size.error()));
            }
            if (*candidate_size <= effective_limit) {
                current.push_back(fragment);
                fragment.block_count = 0;
                continue;
            }

            if (fragment.kind == SparseChunkKind::Raw) {
                auto prefix_blocks = largest_raw_prefix(
                    current, fragment, total_blocks, block_size,
                    effective_limit);
                if (!prefix_blocks) {
                    return std::unexpected(std::move(prefix_blocks.error()));
                }
                if (*prefix_blocks != 0) {
                    auto prefix = fragment;
                    prefix.block_count = *prefix_blocks;
                    current.push_back(prefix);
                    fragment.start_block += *prefix_blocks;
                    fragment.block_count -= *prefix_blocks;
                    fragment.source_offset +=
                        static_cast<std::uint64_t>(*prefix_blocks) * block_size;
                }
            }

            if (current.empty()) {
                return std::unexpected(plan_error(
                    SparseFlashPlanErrorKind::Unsupported,
                    static_cast<std::uint64_t>(fragment.start_block) *
                        block_size,
                    "device maximum download size is smaller than one sparse "
                    "data block and its metadata"));
            }
            if (auto flushed = flush(); !flushed) {
                return std::unexpected(std::move(flushed.error()));
            }
        }
    }

    if (current.empty() && parts.empty()) {
        auto empty = build_part(
            artifact.transfer_source(), {}, block_size, total_blocks);
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
    } else if (auto flushed = flush(); !flushed) {
        return std::unexpected(std::move(flushed.error()));
    }

    return SparseFlashPlan(
        std::move(parts), true, artifact.metadata().expanded_size,
        total_transfer_size);
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
