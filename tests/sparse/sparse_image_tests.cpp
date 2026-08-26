// SPDX-License-Identifier: MIT
#include "flash_artifact.hpp"
#include "sparse_flash_plan.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::FlashArtifact;
using kairosboot::image::FlashArtifactKind;
using kairosboot::image::SparseChunkKind;
using kairosboot::image::SparseErrorKind;
using kairosboot::image::SparseFlashPlan;
using kairosboot::image::SparseFlashPlanErrorKind;
using kairosboot::image::SparseImage;
using kairosboot::image::kAndroidSparseMagic;
using kairosboot::image::kMaxSparseChunks;
using kairosboot::image::kSparseChunkCrc32;
using kairosboot::image::kSparseChunkDontCare;
using kairosboot::image::kSparseChunkFill;
using kairosboot::image::kSparseChunkRaw;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            throw CheckFailure(std::string("check failed: ") + #condition + " at line " + \
                               std::to_string(__LINE__));                                     \
        }                                                                                    \
    } while (false)

class MemorySource final : public IImageSource {
public:
    explicit MemorySource(
        std::vector<std::byte> bytes,
        const std::size_t max_read = std::numeric_limits<std::size_t>::max())
        : bytes_(std::move(bytes)), max_read_(max_read) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset >= bytes_.size() || destination.empty()) {
            return 0;
        }
        const auto available = bytes_.size() - static_cast<std::size_t>(offset);
        const auto amount = std::min({available, destination.size(), max_read_});
        std::ranges::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            amount,
            destination.begin());
        return amount;
    }

private:
    std::vector<std::byte> bytes_;
    std::size_t max_read_;
};

class ObservingSource final : public IImageSource {
public:
    struct Read final {
        std::uint64_t offset{0};
        std::size_t requested{0};
    };

    explicit ObservingSource(
        std::vector<std::byte> bytes,
        const std::size_t max_read = std::numeric_limits<std::size_t>::max(),
        const std::optional<std::size_t> failing_call = std::nullopt)
        : bytes_(std::move(bytes)),
          max_read_(max_read),
          failing_call_(failing_call) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        reads_.push_back(Read{offset, destination.size()});
        const auto call = reads_.size() - 1;
        if (failing_call_ == call) {
            return std::unexpected(ImageSourceError{"scripted source failure"});
        }
        if (offset >= bytes_.size() || destination.empty()) {
            return 0;
        }
        const auto available = bytes_.size() - static_cast<std::size_t>(offset);
        const auto amount = std::min({available, destination.size(), max_read_});
        std::ranges::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            amount,
            destination.begin());
        return amount;
    }

    [[nodiscard]] std::span<const Read> reads() const noexcept {
        return reads_;
    }

private:
    std::vector<std::byte> bytes_;
    std::size_t max_read_;
    std::optional<std::size_t> failing_call_;
    mutable std::vector<Read> reads_;
};

class CancellingSource final : public IImageSource {
public:
    CancellingSource(
        std::vector<std::byte> bytes,
        std::stop_source& cancellation,
        const std::uint64_t trigger_offset,
        const std::size_t max_read = std::numeric_limits<std::size_t>::max())
        : bytes_(std::move(bytes)),
          cancellation_(cancellation),
          trigger_offset_(trigger_offset),
          max_read_(max_read) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset >= bytes_.size() || destination.empty()) {
            return 0;
        }
        const auto available = bytes_.size() - static_cast<std::size_t>(offset);
        const auto amount = std::min({available, destination.size(), max_read_});
        std::ranges::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            amount,
            destination.begin());
        if (offset == trigger_offset_) {
            cancellation_.request_stop();
        }
        return amount;
    }

private:
    std::vector<std::byte> bytes_;
    std::stop_source& cancellation_;
    std::uint64_t trigger_offset_;
    std::size_t max_read_;
};

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
}

[[nodiscard]] std::vector<std::byte> little_u32(const std::uint32_t value) {
    std::vector<std::byte> bytes;
    append_u32(bytes, value);
    return bytes;
}

[[nodiscard]] std::vector<std::byte> text_bytes(const std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const unsigned char character : text) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> oracle_pattern(
    const std::size_t size,
    const unsigned int seed = 0) {
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = std::byte{static_cast<unsigned char>(
            (index * 17 + 3 + seed) % 251)};
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> repeated_u32_blocks(
    const std::uint32_t value,
    const std::uint32_t blocks) {
    std::vector<std::byte> bytes(
        static_cast<std::size_t>(blocks) * 4096);
    const auto pattern = little_u32(value);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4) {
        std::ranges::copy(pattern, bytes.begin() +
            static_cast<std::ptrdiff_t>(offset));
    }
    return bytes;
}

void append_header(
    std::vector<std::byte>& bytes,
    const std::uint32_t block_size,
    const std::uint32_t total_blocks,
    const std::uint32_t total_chunks,
    const std::uint32_t checksum = 0,
    const std::uint16_t file_header_size = 28,
    const std::uint16_t chunk_header_size = 12,
    const std::uint16_t major_version = 1,
    const std::uint32_t magic = kAndroidSparseMagic) {
    append_u32(bytes, magic);
    append_u16(bytes, major_version);
    append_u16(bytes, 0);
    append_u16(bytes, file_header_size);
    append_u16(bytes, chunk_header_size);
    append_u32(bytes, block_size);
    append_u32(bytes, total_blocks);
    append_u32(bytes, total_chunks);
    append_u32(bytes, checksum);
    while (bytes.size() < file_header_size) {
        bytes.push_back(std::byte{0xA5});
    }
}

void append_chunk(
    std::vector<std::byte>& bytes,
    const std::uint16_t type,
    const std::uint32_t blocks,
    const std::span<const std::byte> payload,
    const std::uint16_t chunk_header_size = 12,
    const std::optional<std::uint32_t> total_size_override = std::nullopt) {
    append_u16(bytes, type);
    append_u16(bytes, 0);
    append_u32(bytes, blocks);
    append_u32(
        bytes,
        total_size_override.value_or(
            static_cast<std::uint32_t>(chunk_header_size + payload.size())));
    for (std::size_t index = 12; index < chunk_header_size; ++index) {
        bytes.push_back(std::byte{0x5A});
    }
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

[[nodiscard]] std::uint32_t crc32(
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

struct MixedImage {
    std::vector<std::byte> sparse;
    std::vector<std::byte> expanded;
};

[[nodiscard]] MixedImage make_mixed_image() {
    auto raw = text_bytes("abcdefgh");
    const auto fill = little_u32(0x04030201U);

    std::vector<std::byte> expanded = raw;
    expanded.insert(expanded.end(), fill.begin(), fill.end());
    expanded.insert(expanded.end(), fill.begin(), fill.end());
    expanded.insert(expanded.end(), 4, std::byte{0});
    const auto checksum = crc32(0, expanded);

    std::vector<std::byte> sparse;
    append_header(sparse, 4, 5, 4, checksum);
    append_chunk(sparse, kSparseChunkRaw, 2, raw);
    append_chunk(sparse, kSparseChunkFill, 2, fill);
    append_chunk(sparse, kSparseChunkDontCare, 1, {});
    const auto checksum_bytes = little_u32(checksum);
    append_chunk(sparse, kSparseChunkCrc32, 0, checksum_bytes);
    return {std::move(sparse), std::move(expanded)};
}

[[nodiscard]] auto open_bytes(
    std::vector<std::byte> bytes,
    const std::size_t max_read = std::numeric_limits<std::size_t>::max()) {
    return SparseImage::open(std::make_shared<MemorySource>(std::move(bytes), max_read));
}

[[nodiscard]] std::vector<std::byte> read_all(
    const std::shared_ptr<const IImageSource>& source,
    const std::size_t maximum_read = std::numeric_limits<std::size_t>::max()) {
    CHECK(source->size() <= std::numeric_limits<std::size_t>::max());
    std::vector<std::byte> result(static_cast<std::size_t>(source->size()));
    std::size_t completed = 0;
    while (completed < result.size()) {
        const auto amount = std::min(maximum_read, result.size() - completed);
        auto read = source->read_at(completed,
                                    std::span(result).subspan(completed, amount));
        CHECK(read.has_value());
        CHECK(*read != 0);
        CHECK(*read <= amount);
        completed += *read;
    }
    return result;
}

void pre_cancelled_entry_points_do_not_read_sources() {
    std::vector<std::byte> sparse;
    append_header(sparse, 4, 0, 0);
    auto sparse_source = std::make_shared<ObservingSource>(std::move(sparse));
    std::stop_source sparse_cancellation;
    sparse_cancellation.request_stop();

    const auto parsed = SparseImage::open(
        sparse_source, sparse_cancellation.get_token());
    CHECK(!parsed);
    CHECK(parsed.error().kind == SparseErrorKind::Cancelled);
    CHECK(parsed.error().input_offset == 0);
    CHECK(parsed.error().message ==
          "sparse image parsing cancelled while reading the file header");
    CHECK(sparse_source->reads().empty());

    auto artifact_source = std::make_shared<ObservingSource>(
        std::vector<std::byte>(16, std::byte{0xA5}));
    std::stop_source artifact_cancellation;
    artifact_cancellation.request_stop();

    const auto artifact = FlashArtifact::inspect(
        artifact_source, artifact_cancellation.get_token());
    CHECK(!artifact);
    CHECK(artifact.error().kind == SparseErrorKind::Cancelled);
    CHECK(artifact.error().input_offset == 0);
    CHECK(artifact.error().message ==
          "flash artifact inspection cancelled while classifying the input");
    CHECK(artifact_source->reads().empty());
}

void short_header_reads_are_cancellable_at_the_exact_cursor() {
    std::vector<std::byte> sparse;
    append_header(sparse, 4, 0, 0);
    std::stop_source cancellation;
    constexpr std::uint64_t cancellation_offset = 7;
    auto source = std::make_shared<CancellingSource>(
        std::move(sparse), cancellation, cancellation_offset, 1);

    const auto parsed = SparseImage::open(source, cancellation.get_token());
    CHECK(!parsed);
    CHECK(parsed.error().kind == SparseErrorKind::Cancelled);
    CHECK(parsed.error().input_offset == cancellation_offset);
    CHECK(parsed.error().message ==
          "sparse image parsing cancelled while reading the file header");
}

void chunk_table_parsing_is_cancellable_at_the_exact_chunk() {
    std::vector<std::byte> sparse;
    append_header(sparse, 4, 2, 2);
    append_chunk(sparse, kSparseChunkDontCare, 1, {});
    append_chunk(sparse, kSparseChunkDontCare, 1, {});

    std::stop_source cancellation;
    constexpr std::uint64_t first_chunk_offset = 28;
    auto source = std::make_shared<CancellingSource>(
        std::move(sparse), cancellation, first_chunk_offset);

    const auto parsed = SparseImage::open(source, cancellation.get_token());
    CHECK(!parsed);
    CHECK(parsed.error().kind == SparseErrorKind::Cancelled);
    CHECK(parsed.error().input_offset == first_chunk_offset);
    CHECK(parsed.error().message ==
          "sparse image parsing cancelled while reading the chunk table");
}

void raw_checksum_scans_are_cancellable_between_64k_windows() {
    constexpr std::size_t checksum_window_size = 64U * 1024U;
    constexpr std::size_t raw_size = checksum_window_size * 3U;
    constexpr std::uint64_t raw_payload_offset = 28U + 12U;
    constexpr std::uint64_t second_window_offset =
        raw_payload_offset + checksum_window_size;

    std::vector<std::byte> raw(raw_size, std::byte{0xA5});
    std::vector<std::byte> sparse;
    append_header(
        sparse,
        4,
        static_cast<std::uint32_t>(raw_size / 4U),
        1,
        1);
    append_chunk(
        sparse,
        kSparseChunkRaw,
        static_cast<std::uint32_t>(raw_size / 4U),
        raw);

    std::stop_source cancellation;
    auto source = std::make_shared<CancellingSource>(
        std::move(sparse), cancellation, second_window_offset);

    const auto parsed = SparseImage::open(source, cancellation.get_token());
    CHECK(!parsed);
    CHECK(parsed.error().kind == SparseErrorKind::Cancelled);
    CHECK(parsed.error().input_offset == second_window_offset);
    CHECK(parsed.error().message ==
          "sparse checksum verification cancelled while scanning RAW data");
}

void sparse_flash_plan_preserves_payloads_that_fit() {
    auto source = std::make_shared<MemorySource>(
        std::vector<std::byte>(4096, std::byte{0x4B}));
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());

    auto plan = SparseFlashPlan::create(*artifact, 4096);
    CHECK(plan.has_value());
    CHECK(!plan->reparsed());
    CHECK(plan->parts().size() == 1);
    CHECK(plan->parts().front().source.get() == source.get());
    CHECK(plan->transfer_size() == 4096);
    CHECK(plan->expanded_size() == 4096);
}

void raw_images_are_split_without_materializing_their_expansion() {
    auto raw = oracle_pattern(3 * 4096);
    auto source = std::make_shared<MemorySource>(raw, 73);
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());

    auto plan = SparseFlashPlan::create(*artifact, 4200);
    CHECK(plan.has_value());
    CHECK(plan->reparsed());
    CHECK(plan->parts().size() == 3);
    CHECK(plan->expanded_size() == raw.size());

    for (std::size_t index = 0; index < plan->parts().size(); ++index) {
        const auto& part = plan->parts()[index];
        CHECK(part.source->size() <= 4200);
        CHECK(part.first_data_offset == index * 4096);
        CHECK(part.data_end_offset == (index + 1) * 4096);

        auto encoded = read_all(part.source, 97);
        std::vector<std::byte> expected;
        const auto chunk_count = index == 1 ? 3U : 2U;
        append_header(expected, 4096, 3, chunk_count);
        if (index != 0) {
            append_chunk(
                expected,
                kSparseChunkDontCare,
                static_cast<std::uint32_t>(index),
                {});
        }
        append_chunk(
            expected,
            kSparseChunkRaw,
            1,
            std::span(raw).subspan(index * 4096, 4096));
        if (index != 2) {
            append_chunk(
                expected,
                kSparseChunkDontCare,
                static_cast<std::uint32_t>(2 - index),
                {});
        }
        // Frozen Platform-Tools 37.0.1 Darwin binary SHA-256 values are
        // 50cdb9c2e5f0104ae83181a1156728ccd7e5a1acc347353c393b09df29c9eee5,
        // 8dbdff7c3e9d4aca632cc722b5cddc0a50a9a52de924f33407525878ea7a86cc,
        // be02dc22cd33ceeb28ffdf60853f10eecf76d90e3c9eba54f4ac04adca3300e4.
        CHECK(encoded == expected);
        auto parsed = open_bytes(std::move(encoded), 41);
        CHECK(parsed.has_value());
        CHECK(parsed->output_size() == raw.size());
        std::array<std::byte, 4096> expanded_block{};
        auto read = parsed->read_at(index * 4096, expanded_block);
        CHECK(read.has_value());
        CHECK(*read == expanded_block.size());
        CHECK(std::ranges::equal(
            expanded_block,
            std::span(raw).subspan(index * 4096, expanded_block.size())));
        const auto chunks = parsed->chunks();
        CHECK(std::ranges::count_if(chunks, [](const auto& chunk) {
                  return chunk.kind == SparseChunkKind::Raw;
              }) == 1);
    }
}

void raw_fill_detection_and_encoded_size_boundaries_match_aosp() {
    auto repeated = repeated_u32_blocks(0x04030201U, 2);
    auto repeated_source = std::make_shared<MemorySource>(repeated, 31);
    auto repeated_artifact = FlashArtifact::inspect(repeated_source);
    CHECK(repeated_artifact.has_value());

    auto fitting = SparseFlashPlan::create(
        *repeated_artifact,
        repeated.size(),
        1);
    CHECK(fitting.has_value());
    CHECK(!fitting->reparsed());
    CHECK(fitting->parts().size() == 1);
    CHECK(fitting->parts().front().source.get() == repeated_source.get());

    auto one_byte_over =
        SparseFlashPlan::create(*repeated_artifact, repeated.size() - 1);
    CHECK(one_byte_over.has_value());
    CHECK(one_byte_over->reparsed());
    CHECK(one_byte_over->parts().size() == 1);

    std::vector<std::byte> expected_merged_fill;
    append_header(expected_merged_fill, 4096, 2, 1);
    const auto merged_fill = little_u32(0x04030201U);
    append_chunk(
        expected_merged_fill, kSparseChunkFill, 2, merged_fill);
    CHECK(read_all(one_byte_over->parts().front().source, 7) ==
          expected_merged_fill);

    std::vector<std::byte> different_fills(2 * 4096);
    std::ranges::fill(
        std::span(different_fills).first(4096), std::byte{0x01});
    std::ranges::fill(
        std::span(different_fills).last(4096), std::byte{0x02});
    auto different_source =
        std::make_shared<MemorySource>(different_fills, 43);
    auto different_artifact = FlashArtifact::inspect(different_source);
    CHECK(different_artifact.has_value());
    auto different_plan = SparseFlashPlan::create(*different_artifact, 4200);
    CHECK(different_plan.has_value());
    CHECK(different_plan->parts().size() == 1);

    std::vector<std::byte> expected_different_fills;
    append_header(expected_different_fills, 4096, 2, 2);
    const auto first_fill = little_u32(0x01010101U);
    const auto second_fill = little_u32(0x02020202U);
    append_chunk(
        expected_different_fills, kSparseChunkFill, 1, first_fill);
    append_chunk(
        expected_different_fills, kSparseChunkFill, 1, second_fill);
    const auto encoded = read_all(different_plan->parts().front().source, 5);
    CHECK(encoded == expected_different_fills);
    CHECK(encoded.size() == 60);
    // Frozen Platform-Tools 37.0.1 Darwin binary SHA-256:
    // 270cec22bcd7b6ce57d80502c9e3013e9f78765da11a925accc2a75478a74f64.
}

void aosp_56_byte_overhead_boundary_is_exact() {
    auto raw = oracle_pattern(2 * 4096);
    auto source = std::make_shared<MemorySource>(raw, 67);
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());

    // 56 bytes are reserved before block-aligned splitting. At 4164 bytes the
    // two-pass libsparse algorithm also emits its observed empty tail part.
    auto exact = SparseFlashPlan::create(*artifact, 4164);
    if (!exact) {
        throw CheckFailure("exact AOSP overhead boundary failed: " +
                           exact.error().message);
    }
    CHECK(exact->parts().size() == 3);
    CHECK(exact->parts()[0].source->size() == 4148);
    CHECK(exact->parts()[1].source->size() == 4148);
    CHECK(exact->parts()[2].source->size() == 40);

    std::vector<std::byte> expected_first;
    append_header(expected_first, 4096, 2, 2);
    append_chunk(
        expected_first,
        kSparseChunkRaw,
        1,
        std::span(raw).first(4096));
    append_chunk(expected_first, kSparseChunkDontCare, 1, {});
    std::vector<std::byte> expected_second;
    append_header(expected_second, 4096, 2, 2);
    append_chunk(expected_second, kSparseChunkDontCare, 1, {});
    append_chunk(
        expected_second,
        kSparseChunkRaw,
        1,
        std::span(raw).last(4096));
    std::vector<std::byte> expected_empty;
    append_header(expected_empty, 4096, 2, 1);
    append_chunk(expected_empty, kSparseChunkDontCare, 2, {});
    CHECK(read_all(exact->parts()[0].source) == expected_first);
    CHECK(read_all(exact->parts()[1].source) == expected_second);
    CHECK(read_all(exact->parts()[2].source) == expected_empty);

    auto one_byte_short = SparseFlashPlan::create(*artifact, 4163);
    CHECK(!one_byte_short);
    CHECK(one_byte_short.error().kind ==
          SparseFlashPlanErrorKind::Unsupported);
}

void sparse_backed_blocks_merge_like_aosp_libsparse() {
    constexpr std::uint32_t blocks = 10;
    constexpr std::uint32_t fill_value = 0x11223344U;
    std::vector<std::byte> sparse;
    append_header(sparse, 4096, blocks, blocks);
    const auto fill = little_u32(fill_value);
    for (std::uint32_t index = 0; index < blocks; ++index) {
        append_chunk(sparse, kSparseChunkFill, 1, fill);
    }

    auto source = std::make_shared<MemorySource>(sparse, 11);
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());
    CHECK(artifact->metadata().transfer_size == 188);

    auto fitting = SparseFlashPlan::create(*artifact, 188);
    CHECK(fitting.has_value());
    CHECK(!fitting->reparsed());
    CHECK(fitting->parts().front().source.get() == source.get());

    auto plan = SparseFlashPlan::create(*artifact, 100);
    CHECK(plan.has_value());
    CHECK(plan->reparsed());
    CHECK(plan->parts().size() == 1);
    std::vector<std::byte> expected;
    append_header(expected, 4096, blocks, 1);
    append_chunk(expected, kSparseChunkFill, blocks, fill);
    CHECK(read_all(plan->parts().front().source, 3) == expected);
}

void sparse_raw_chunks_obey_the_aosp_seven_eighths_heuristic() {
    constexpr std::uint32_t first_blocks = 11;
    constexpr std::uint32_t second_blocks = 2;
    auto first = oracle_pattern(first_blocks * 4096);
    auto second = oracle_pattern(second_blocks * 4096, 29);

    std::vector<std::byte> sparse;
    append_header(
        sparse, 4096, first_blocks + second_blocks, 2);
    append_chunk(sparse, kSparseChunkRaw, first_blocks, first);
    append_chunk(sparse, kSparseChunkRaw, second_blocks, second);

    auto source = std::make_shared<MemorySource>(sparse, 101);
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());
    auto plan = SparseFlashPlan::create(*artifact, 50000);
    CHECK(plan.has_value());
    CHECK(plan->parts().size() == 2);

    std::vector<std::byte> expected_first;
    append_header(
        expected_first, 4096, first_blocks + second_blocks, 2);
    append_chunk(expected_first, kSparseChunkRaw, first_blocks, first);
    append_chunk(
        expected_first, kSparseChunkDontCare, second_blocks, {});
    std::vector<std::byte> expected_second;
    append_header(
        expected_second, 4096, first_blocks + second_blocks, 2);
    append_chunk(
        expected_second, kSparseChunkDontCare, first_blocks, {});
    append_chunk(expected_second, kSparseChunkRaw, second_blocks, second);

    const auto encoded_first = read_all(plan->parts()[0].source, 113);
    const auto encoded_second = read_all(plan->parts()[1].source, 127);
    CHECK(encoded_first == expected_first);
    CHECK(encoded_second == expected_second);
    CHECK(encoded_first.size() == 45108);
    CHECK(encoded_second.size() == 8244);
    CHECK(plan->parts()[0].first_data_offset == 0);
    CHECK(plan->parts()[0].data_end_offset == first_blocks * 4096ULL);
    CHECK(plan->parts()[1].first_data_offset == first_blocks * 4096ULL);
    CHECK(plan->parts()[1].data_end_offset ==
          (first_blocks + second_blocks) * 4096ULL);
    // Frozen Platform-Tools 37.0.1 Darwin binary SHA-256 values:
    // 4a771e5374457c2d58c0976499b2c1b98253ccd328684acccf054c54344ed79e,
    // a2c4874f86206520bbcbce21f33bad7df7cacdf0a62696c1936dd7a53df7c8f4.
}

void sparse_chunks_are_repacked_with_partition_offsets_preserved() {
    auto mixed = make_mixed_image();
    auto source = std::make_shared<MemorySource>(mixed.sparse, 5);
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());
    CHECK(artifact->metadata().kind == FlashArtifactKind::AndroidSparse);

    auto plan = SparseFlashPlan::create(*artifact, 88);
    CHECK(plan.has_value());
    CHECK(plan->reparsed());
    CHECK(plan->parts().size() >= 2);

    std::vector<std::byte> reconstructed(mixed.expanded.size());
    std::vector<std::uint8_t> written(mixed.expanded.size());
    for (const auto& part : plan->parts()) {
        CHECK(part.source->size() <= 88);
        auto parsed = SparseImage::open(part.source);
        CHECK(parsed.has_value());
        CHECK(parsed->output_size() == mixed.expanded.size());
        for (const auto& chunk : parsed->chunks()) {
            if (chunk.kind != SparseChunkKind::Raw &&
                chunk.kind != SparseChunkKind::Fill) {
                continue;
            }
            auto destination = std::span(reconstructed).subspan(
                static_cast<std::size_t>(chunk.output_offset),
                static_cast<std::size_t>(chunk.output_size));
            auto read = parsed->read_at(chunk.output_offset, destination);
            CHECK(read.has_value());
            CHECK(*read == destination.size());
            std::ranges::fill(
                std::span(written).subspan(
                    static_cast<std::size_t>(chunk.output_offset),
                    static_cast<std::size_t>(chunk.output_size)),
                std::uint8_t{1});
        }
    }
    for (std::size_t index = 0; index < written.size(); ++index) {
        if (written[index]) {
            CHECK(reconstructed[index] == mixed.expanded[index]);
        }
    }
    CHECK(std::ranges::count(written, std::uint8_t{1}) == 16);
}

void sparse_flash_plan_rejects_unsafe_split_inputs() {
    {
        auto source = std::make_shared<MemorySource>(
            std::vector<std::byte>(4097, std::byte{0x2A}));
        auto artifact = FlashArtifact::inspect(source);
        CHECK(artifact.has_value());

        auto fitting = SparseFlashPlan::create(*artifact, 4097);
        CHECK(fitting.has_value());
        CHECK(!fitting->reparsed());
        CHECK(fitting->parts().front().source.get() == source.get());

        auto plan = SparseFlashPlan::create(*artifact, 4096);
        CHECK(!plan);
        CHECK(plan.error().kind == SparseFlashPlanErrorKind::Unsupported);
    }
    {
        auto raw = oracle_pattern(2 * 4096 + 1);
        auto source = std::make_shared<MemorySource>(std::move(raw));
        auto artifact = FlashArtifact::inspect(source);
        CHECK(artifact.has_value());
        auto plan = SparseFlashPlan::create(*artifact, 4200);
        CHECK(!plan);
        CHECK(plan.error().kind == SparseFlashPlanErrorKind::Unsupported);
        // The frozen Platform-Tools 37.0.1 binary likewise closes without a
        // DATA command for this 8193-byte fixture.
    }
    {
        auto source = std::make_shared<MemorySource>(
            std::vector<std::byte>(4096, std::byte{0x2A}));
        auto artifact = FlashArtifact::inspect(source);
        CHECK(artifact.has_value());
        auto plan = SparseFlashPlan::create(*artifact, 40);
        CHECK(!plan);
        CHECK(plan.error().kind == SparseFlashPlanErrorKind::Unsupported);
    }
}

void sparse_flash_planning_is_cancellable_during_raw_scans() {
    auto raw = oracle_pattern(3 * 4096);
    std::stop_source cancellation;
    auto source = std::make_shared<CancellingSource>(
        std::move(raw), cancellation, 4096);
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());

    auto plan = SparseFlashPlan::create(
        *artifact, 4200, kairosboot::image::kDefaultResparseLimitBytes,
        cancellation.get_token());
    CHECK(!plan);
    CHECK(plan.error().kind == SparseFlashPlanErrorKind::Cancelled);

    auto fitting_source = std::make_shared<MemorySource>(
        std::vector<std::byte>(4096, std::byte{0x5A}));
    auto fitting_artifact = FlashArtifact::inspect(fitting_source);
    CHECK(fitting_artifact.has_value());
    std::stop_source pre_cancelled;
    pre_cancelled.request_stop();
    auto cancelled_fitting = SparseFlashPlan::create(
        *fitting_artifact,
        4096,
        kairosboot::image::kDefaultResparseLimitBytes,
        pre_cancelled.get_token());
    CHECK(!cancelled_fitting);
    CHECK(cancelled_fitting.error().kind ==
          SparseFlashPlanErrorKind::Cancelled);
}

void sparse_flash_planning_surfaces_raw_source_failures() {
    auto raw = oracle_pattern(2 * 4096);
    auto source = std::make_shared<ObservingSource>(
        std::move(raw),
        std::numeric_limits<std::size_t>::max(),
        1);
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());

    auto plan = SparseFlashPlan::create(*artifact, 4200);
    CHECK(!plan);
    CHECK(plan.error().kind == SparseFlashPlanErrorKind::Source);
    CHECK(plan.error().output_offset == 0);
}

void one_hundred_thousand_fragments_remain_near_linear() {
    constexpr std::uint32_t fragment_count = 100'000;
    constexpr std::uint64_t target_size = 1'000'000;
    std::vector<std::byte> sparse;
    sparse.reserve(28 + static_cast<std::size_t>(fragment_count) * 16);
    append_header(sparse, 4096, fragment_count, fragment_count);
    for (std::uint32_t index = 0; index < fragment_count; ++index) {
        const auto fill = little_u32(index & 1U ? 0x55555555U
                                                : 0xAAAAAAAAU);
        append_chunk(sparse, kSparseChunkFill, 1, fill);
    }

    auto source = std::make_shared<MemorySource>(std::move(sparse), 257);
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());
    const auto started = std::chrono::steady_clock::now();
    auto plan = SparseFlashPlan::create(*artifact, target_size);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!plan) {
        throw CheckFailure("100k sparse plan failed: " +
                           plan.error().message);
    }
    CHECK(plan->reparsed());
    CHECK(plan->parts().size() >= 2);
    CHECK(std::ranges::all_of(plan->parts(), [](const auto& part) {
        return part.source->size() <= target_size;
    }));
    CHECK(elapsed < std::chrono::seconds(10));
    std::cout << "METRIC: 100k sparse planner "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                     .count()
              << " ms\n";
}

void flash_artifact_classifies_raw_without_materializing_it() {
    auto short_source = std::make_shared<ObservingSource>(
        std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}});
    auto short_artifact = FlashArtifact::inspect(short_source);
    CHECK(short_artifact.has_value());
    CHECK(short_artifact->metadata().kind == FlashArtifactKind::Raw);
    CHECK(short_artifact->metadata().transfer_size == 2);
    CHECK(short_artifact->metadata().expanded_size == 2);
    CHECK(!short_artifact->metadata().sparse_header.has_value());
    CHECK(short_artifact->transfer_source().get() == short_source.get());
    CHECK(short_artifact->sparse_image() == nullptr);
    CHECK(short_source->reads().empty());

    auto near_magic = little_u32(kAndroidSparseMagic ^ 0x01000000U);
    near_magic.resize(1024U * 1024U, std::byte{0xA5});
    auto raw_source = std::make_shared<ObservingSource>(std::move(near_magic));
    auto raw_artifact = FlashArtifact::inspect(raw_source);
    CHECK(raw_artifact.has_value());
    CHECK(raw_artifact->metadata().kind == FlashArtifactKind::Raw);
    CHECK(raw_artifact->metadata().transfer_size == 1024U * 1024U);
    CHECK(raw_artifact->transfer_source().get() == raw_source.get());
    CHECK(raw_source->reads().size() == 1);
    CHECK(raw_source->reads().front().offset == 0);
    CHECK(raw_source->reads().front().requested == sizeof(std::uint32_t));
}

void flash_artifact_validates_sparse_but_preserves_encoded_source() {
    auto fixture = make_mixed_image();
    const auto encoded_size = fixture.sparse.size();
    const auto expanded_size = fixture.expanded.size();
    auto source = std::make_shared<ObservingSource>(
        std::move(fixture.sparse), 3);

    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact.has_value());
    CHECK(artifact->metadata().kind == FlashArtifactKind::AndroidSparse);
    CHECK(artifact->metadata().transfer_size == encoded_size);
    CHECK(artifact->metadata().expanded_size == expanded_size);
    CHECK(artifact->metadata().sparse_header.has_value());
    CHECK(artifact->metadata().sparse_header->total_chunks == 4);
    CHECK(artifact->transfer_source().get() == source.get());
    CHECK(artifact->transfer_source()->size() == encoded_size);
    CHECK(artifact->sparse_image() != nullptr);
    CHECK(artifact->sparse_image()->output_size() == expanded_size);
    CHECK(!source->reads().empty());
    for (const auto& read : source->reads()) {
        CHECK(read.requested <= 64U * 1024U);
    }
}

void flash_artifact_surfaces_sparse_and_source_failures() {
    std::vector<std::byte> truncated;
    append_u32(truncated, kAndroidSparseMagic);
    truncated.resize(10, std::byte{0});
    const auto truncated_result = FlashArtifact::inspect(
        std::make_shared<MemorySource>(std::move(truncated)));
    CHECK(!truncated_result);
    CHECK(truncated_result.error().kind == SparseErrorKind::Truncated);

    auto corrupted = make_mixed_image().sparse;
    CHECK(corrupted.size() > 40);
    corrupted[40] ^= std::byte{0x01};
    const auto checksum_result = FlashArtifact::inspect(
        std::make_shared<MemorySource>(std::move(corrupted)));
    CHECK(!checksum_result);
    CHECK(checksum_result.error().kind == SparseErrorKind::Malformed);

    auto source_failure_bytes = little_u32(kAndroidSparseMagic);
    auto failing_source = std::make_shared<ObservingSource>(
        std::move(source_failure_bytes),
        std::numeric_limits<std::size_t>::max(), 0);
    const auto source_result = FlashArtifact::inspect(failing_source);
    CHECK(!source_result);
    CHECK(source_result.error().kind == SparseErrorKind::Source);
}

void mixed_chunks_expand_with_bounded_reads() {
    auto fixture = make_mixed_image();
    auto parsed = open_bytes(std::move(fixture.sparse), 3);
    CHECK(parsed.has_value());
    CHECK(parsed->header().block_size == 4);
    CHECK(parsed->output_size() == fixture.expanded.size());
    CHECK(parsed->chunks().size() == 4);
    CHECK(parsed->chunks()[0].kind == SparseChunkKind::Raw);
    CHECK(parsed->chunks()[1].kind == SparseChunkKind::Fill);
    CHECK(parsed->chunks()[2].kind == SparseChunkKind::DontCare);
    CHECK(parsed->chunks()[3].kind == SparseChunkKind::Crc32);

    std::vector<std::byte> output(fixture.expanded.size());
    const auto read = parsed->read_at(0, output);
    CHECK(read.has_value());
    CHECK(*read == output.size());
    CHECK(output == fixture.expanded);
}

void output_windows_cross_chunk_boundaries() {
    auto fixture = make_mixed_image();
    auto parsed = open_bytes(std::move(fixture.sparse), 2);
    CHECK(parsed.has_value());

    std::array<std::byte, 11> window{};
    const auto read = parsed->read_at(6, window);
    CHECK(read.has_value());
    CHECK(*read == window.size());
    CHECK(std::ranges::equal(window, std::span(fixture.expanded).subspan(6, window.size())));

    std::array<std::byte, 7> unaligned_fill{};
    const auto fill_read = parsed->read_at(9, unaligned_fill);
    CHECK(fill_read.has_value());
    CHECK(*fill_read == unaligned_fill.size());
    CHECK(std::ranges::equal(
        unaligned_fill,
        std::span(fixture.expanded).subspan(9, unaligned_fill.size())));

    std::array<std::byte, 8> tail{};
    const auto tail_read = parsed->read_at(parsed->output_size() - 2, tail);
    CHECK(tail_read.has_value());
    CHECK(*tail_read == 2);
    CHECK(tail[0] == std::byte{0});
    CHECK(tail[1] == std::byte{0});
}

void zero_block_image_is_valid_but_zero_block_data_chunk_is_not() {
    std::vector<std::byte> empty_image;
    append_header(empty_image, 4096, 0, 0);
    auto parsed = open_bytes(std::move(empty_image));
    CHECK(parsed.has_value());
    CHECK(parsed->output_size() == 0);
    CHECK(parsed->chunks().empty());

    std::array<std::byte, 1> destination{};
    const auto eof = parsed->read_at(0, destination);
    CHECK(eof.has_value());
    CHECK(*eof == 0);
    const auto beyond = parsed->read_at(1, destination);
    CHECK(!beyond);
    CHECK(beyond.error().kind == SparseErrorKind::InvalidArgument);

    std::vector<std::byte> zero_raw;
    append_header(zero_raw, 4096, 0, 1);
    append_chunk(zero_raw, kSparseChunkRaw, 0, {});
    const auto rejected = open_bytes(std::move(zero_raw));
    CHECK(!rejected);
    CHECK(rejected.error().kind == SparseErrorKind::Malformed);
}

void truncated_headers_and_payloads_are_distinct() {
    std::vector<std::byte> short_header(10, std::byte{0});
    const auto header_result = open_bytes(std::move(short_header));
    CHECK(!header_result);
    CHECK(header_result.error().kind == SparseErrorKind::Truncated);

    std::vector<std::byte> short_raw;
    append_header(short_raw, 4, 1, 1);
    const auto partial_payload = text_bytes("ab");
    append_chunk(short_raw, kSparseChunkRaw, 1, partial_payload, 12, 16);
    const auto payload_result = open_bytes(std::move(short_raw));
    CHECK(!payload_result);
    CHECK(payload_result.error().kind == SparseErrorKind::Truncated);
}

void aggregate_output_overflow_is_rejected() {
    std::vector<std::byte> sparse;
    constexpr auto almost_max_block_size = std::uint32_t{0xFFFFFFFCU};
    constexpr auto max_blocks = std::numeric_limits<std::uint32_t>::max();
    append_header(sparse, almost_max_block_size, max_blocks, 2);
    append_chunk(sparse, kSparseChunkDontCare, max_blocks, {});
    append_chunk(sparse, kSparseChunkDontCare, max_blocks, {});

    const auto parsed = open_bytes(std::move(sparse));
    CHECK(!parsed);
    CHECK(parsed.error().kind == SparseErrorKind::Malformed);
}

void unknown_chunks_and_major_versions_are_unsupported() {
    std::vector<std::byte> unknown_chunk;
    append_header(unknown_chunk, 4, 0, 1);
    append_chunk(unknown_chunk, 0xCAFEU, 0, {});
    const auto chunk_result = open_bytes(std::move(unknown_chunk));
    CHECK(!chunk_result);
    CHECK(chunk_result.error().kind == SparseErrorKind::Unsupported);

    std::vector<std::byte> new_major;
    append_header(new_major, 4, 0, 0, 0, 28, 12, 2);
    const auto version_result = open_bytes(std::move(new_major));
    CHECK(!version_result);
    CHECK(version_result.error().kind == SparseErrorKind::Unsupported);
}

void extended_file_and_chunk_headers_are_skipped() {
    std::vector<std::byte> sparse;
    const auto raw = text_bytes("data");
    append_header(sparse, 4, 1, 1, 0, 32, 16);
    append_chunk(sparse, kSparseChunkRaw, 1, raw, 16);

    auto parsed = open_bytes(std::move(sparse), 1);
    CHECK(parsed.has_value());
    CHECK(parsed->header().file_header_size == 32);
    CHECK(parsed->header().chunk_header_size == 16);
    std::array<std::byte, 4> output{};
    const auto read = parsed->read_at(0, output);
    CHECK(read.has_value());
    CHECK(std::ranges::equal(output, raw));
}

void declared_totals_and_trailing_bytes_are_enforced() {
    const auto raw = text_bytes("data");
    {
        std::vector<std::byte> wrong_blocks;
        append_header(wrong_blocks, 4, 2, 1);
        append_chunk(wrong_blocks, kSparseChunkRaw, 1, raw);
        const auto parsed = open_bytes(std::move(wrong_blocks));
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
    {
        std::vector<std::byte> missing_chunk;
        append_header(missing_chunk, 4, 1, 2);
        append_chunk(missing_chunk, kSparseChunkRaw, 1, raw);
        const auto parsed = open_bytes(std::move(missing_chunk));
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Truncated);
    }
    {
        std::vector<std::byte> trailing;
        append_header(trailing, 4, 1, 1);
        append_chunk(trailing, kSparseChunkRaw, 1, raw);
        trailing.push_back(std::byte{0xFF});
        const auto parsed = open_bytes(std::move(trailing));
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
}

void crc_chunks_and_header_checksum_are_verified() {
    const auto raw = text_bytes("data");
    const auto good_crc = crc32(0, raw);
    {
        // Independent standard CRC-32 vector (verified against zlib), so this
        // does not rely only on the test helper sharing the production logic.
        std::vector<std::byte> known_checksum;
        const auto known_raw = text_bytes("123456789abc");
        append_header(known_checksum, 12, 1, 1, 0xBDB0C0E4U);
        append_chunk(known_checksum, kSparseChunkRaw, 1, known_raw);
        const auto parsed = open_bytes(std::move(known_checksum), 2);
        CHECK(parsed.has_value());
    }
    {
        std::vector<std::byte> nonzero_crc_blocks;
        append_header(nonzero_crc_blocks, 4, 0, 1);
        const auto checksum = little_u32(0);
        append_chunk(nonzero_crc_blocks, kSparseChunkCrc32, 1, checksum);
        const auto parsed = open_bytes(std::move(nonzero_crc_blocks));
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
    {
        std::vector<std::byte> bad_crc_payload;
        append_header(bad_crc_payload, 4, 0, 1);
        append_chunk(bad_crc_payload, kSparseChunkCrc32, 0, {});
        const auto parsed = open_bytes(std::move(bad_crc_payload));
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
    {
        std::vector<std::byte> wrong_crc;
        append_header(wrong_crc, 4, 1, 2);
        append_chunk(wrong_crc, kSparseChunkRaw, 1, raw);
        const auto checksum = little_u32(good_crc ^ 1U);
        append_chunk(wrong_crc, kSparseChunkCrc32, 0, checksum);
        const auto parsed = open_bytes(std::move(wrong_crc), 1);
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
    {
        std::vector<std::byte> wrong_header_crc;
        append_header(wrong_header_crc, 4, 1, 1, good_crc ^ 1U);
        append_chunk(wrong_header_crc, kSparseChunkRaw, 1, raw);
        const auto parsed = open_bytes(std::move(wrong_header_crc), 1);
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
}

void repeated_crc_matches_small_byte_oracle() {
    constexpr std::array<std::uint32_t, 8> block_counts{
        1, 2, 3, 7, 16, 31, 257, 1024,
    };
    const std::array<std::byte, 4> fill_pattern{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
    };

    for (const auto blocks : block_counts) {
        for (const bool is_fill : {false, true}) {
            std::vector<std::byte> expanded(
                static_cast<std::size_t>(blocks) * fill_pattern.size());
            for (std::size_t index = 0; index < expanded.size(); ++index) {
                expanded[index] = is_fill
                    ? fill_pattern[index % fill_pattern.size()]
                    : std::byte{0};
            }
            const auto expected_crc = crc32(0, expanded);

            std::vector<std::byte> sparse;
            append_header(sparse, 4, blocks, 2);
            if (is_fill) {
                append_chunk(sparse, kSparseChunkFill, blocks, fill_pattern);
            } else {
                append_chunk(sparse, kSparseChunkDontCare, blocks, {});
            }
            const auto checksum = little_u32(expected_crc);
            append_chunk(sparse, kSparseChunkCrc32, 0, checksum);

            const auto parsed = open_bytes(std::move(sparse), 2);
            CHECK(parsed.has_value());
        }
    }
}

void huge_virtual_chunks_have_logarithmic_crc_cost() {
    constexpr std::uint32_t block_size = 0x7FFFFFFCU;
    constexpr std::uint32_t block_count = 0xFFFFFFFEU;
    constexpr std::uint64_t expected_output_size =
        static_cast<std::uint64_t>(block_size) * block_count;

    const auto verify_huge_chunk = [&](const bool is_fill) {
        std::vector<std::byte> sparse;
        append_header(sparse, block_size, block_count, 2);
        if (is_fill) {
            const auto fill = little_u32(0x04030201U);
            append_chunk(sparse, kSparseChunkFill, block_count, fill);
        } else {
            append_chunk(sparse, kSparseChunkDontCare, block_count, {});
        }

        // Generated independently with Ruby zlib's crc32_combine over
        // 2,305,843,003,844,984,834 four-byte repetitions.
        const auto expected_crc = is_fill ? 0x88871035U : 0x7927CF45U;
        const auto checksum = little_u32(expected_crc);
        append_chunk(sparse, kSparseChunkCrc32, 0, checksum);

        const auto started = std::chrono::steady_clock::now();
        auto parsed = open_bytes(std::move(sparse), 1);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(parsed.has_value());
        CHECK(parsed->output_size() == expected_output_size);
        CHECK(elapsed < std::chrono::seconds(2));

        std::array<std::byte, 4> tail{};
        const auto read = parsed->read_at(expected_output_size - tail.size(), tail);
        CHECK(read.has_value());
        CHECK(*read == tail.size());
        const std::array<std::byte, 4> expected_tail = is_fill
            ? std::array<std::byte, 4>{
                  std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}
            : std::array<std::byte, 4>{};
        CHECK(tail == expected_tail);
    };

    verify_huge_chunk(false);
    verify_huge_chunk(true);
}

void metadata_chunk_limit_is_enforced_before_reserve() {
    std::vector<std::byte> sparse;
    append_header(sparse, 4096, 0, kMaxSparseChunks + 1U);
    const auto parsed = open_bytes(std::move(sparse));
    CHECK(!parsed);
    CHECK(parsed.error().kind == SparseErrorKind::Unsupported);
}

void malformed_sizes_and_magic_are_rejected() {
    {
        std::vector<std::byte> bad_magic;
        append_header(bad_magic, 4, 0, 0, 0, 28, 12, 1, 0x12345678U);
        const auto parsed = open_bytes(std::move(bad_magic));
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
    {
        std::vector<std::byte> bad_block_size;
        append_header(bad_block_size, 3, 0, 0);
        const auto parsed = open_bytes(std::move(bad_block_size));
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
    {
        std::vector<std::byte> bad_total_size;
        append_header(bad_total_size, 4, 1, 1);
        append_chunk(bad_total_size, kSparseChunkRaw, 1, {}, 12, 8);
        const auto parsed = open_bytes(std::move(bad_total_size));
        CHECK(!parsed);
        CHECK(parsed.error().kind == SparseErrorKind::Malformed);
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"pre-cancelled entry points",
         pre_cancelled_entry_points_do_not_read_sources},
        {"short-read cancellation",
         short_header_reads_are_cancellable_at_the_exact_cursor},
        {"chunk-table cancellation",
         chunk_table_parsing_is_cancellable_at_the_exact_chunk},
        {"RAW checksum cancellation",
         raw_checksum_scans_are_cancellable_between_64k_windows},
        {"sparse flash plan preserves fitting payloads",
         sparse_flash_plan_preserves_payloads_that_fit},
        {"raw sparse split", raw_images_are_split_without_materializing_their_expansion},
        {"raw FILL and encoded boundary",
         raw_fill_detection_and_encoded_size_boundaries_match_aosp},
        {"AOSP 56-byte overhead boundary",
         aosp_56_byte_overhead_boundary_is_exact},
        {"libsparse backed-block merging",
         sparse_backed_blocks_merge_like_aosp_libsparse},
        {"AOSP seven-eighths heuristic",
         sparse_raw_chunks_obey_the_aosp_seven_eighths_heuristic},
        {"sparse repacking", sparse_chunks_are_repacked_with_partition_offsets_preserved},
        {"sparse split rejection", sparse_flash_plan_rejects_unsafe_split_inputs},
        {"sparse planner cancellation",
         sparse_flash_planning_is_cancellable_during_raw_scans},
        {"sparse planner source failures",
         sparse_flash_planning_surfaces_raw_source_failures},
        {"100k sparse planner scale",
         one_hundred_thousand_fragments_remain_near_linear},
        {"flash artifact raw classification",
         flash_artifact_classifies_raw_without_materializing_it},
        {"flash artifact sparse classification",
         flash_artifact_validates_sparse_but_preserves_encoded_source},
        {"flash artifact failures",
         flash_artifact_surfaces_sparse_and_source_failures},
        {"mixed chunks", mixed_chunks_expand_with_bounded_reads},
        {"cross-window reads", output_windows_cross_chunk_boundaries},
        {"zero blocks", zero_block_image_is_valid_but_zero_block_data_chunk_is_not},
        {"truncation", truncated_headers_and_payloads_are_distinct},
        {"overflow", aggregate_output_overflow_is_rejected},
        {"unsupported features", unknown_chunks_and_major_versions_are_unsupported},
        {"header extensions", extended_file_and_chunk_headers_are_skipped},
        {"declared totals", declared_totals_and_trailing_bytes_are_enforced},
        {"CRC validation", crc_chunks_and_header_checksum_are_verified},
        {"repeated CRC oracle", repeated_crc_matches_small_byte_oracle},
        {"huge virtual CRC", huge_virtual_chunks_have_logarithmic_crc_cost},
        {"metadata chunk limit", metadata_chunk_limit_is_enforced_before_reserve},
        {"malformed fields", malformed_sizes_and_magic_are_rejected},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " sparse image test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " sparse image tests passed\n";
    return 0;
}
