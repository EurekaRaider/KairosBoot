// SPDX-License-Identifier: MIT
#include "sparse_image.hpp"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::SparseChunkKind;
using kairosboot::image::SparseErrorKind;
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
