// SPDX-License-Identifier: MIT
#include "src/image/sha256.hpp"
#include "src/image/super_metadata.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw std::runtime_error("check failed: " #condition);             \
        }                                                                       \
    } while (false)

template <typename Integer>
void write_le(const std::span<std::byte> bytes, const std::size_t offset,
              const Integer value) {
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(index * 8U)) &
            static_cast<Integer>(0xffU));
    }
}

template <typename Integer>
[[nodiscard]] Integer read_le(const std::span<const std::byte> bytes,
                              const std::size_t offset) {
    Integer value{};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(std::to_integer<unsigned int>(
                     bytes[offset + index]))
                 << static_cast<unsigned int>(index * 8U);
    }
    return value;
}

[[nodiscard]] kairosboot::image::Sha256Digest sha256(
    const std::span<const std::byte> bytes) {
    kairosboot::image::Sha256Accumulator accumulator;
    accumulator.update(bytes);
    return accumulator.finish();
}

void set_checksum(std::vector<std::byte>& bytes, const std::size_t offset) {
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 32U,
                std::byte{});
    const auto digest = sha256(bytes);
    std::copy(digest.begin(), digest.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] std::vector<std::byte> metadata_area() {
    constexpr std::size_t header_size = 128U;
    constexpr std::size_t table_size = 188U;
    std::vector<std::byte> tables(table_size);

    const std::string_view partition{"system_a"};
    std::transform(partition.begin(), partition.end(), tables.begin(),
                   [](const char value) {
                       return static_cast<std::byte>(value);
                   });
    write_le<std::uint32_t>(tables, 36U, 1U);
    write_le<std::uint32_t>(tables, 40U, 0U);
    write_le<std::uint32_t>(tables, 44U, 1U);
    write_le<std::uint32_t>(tables, 48U, 0U);

    write_le<std::uint64_t>(tables, 52U, 16U);
    write_le<std::uint32_t>(tables, 60U, 0U);
    write_le<std::uint64_t>(tables, 64U, 96U);
    write_le<std::uint32_t>(tables, 72U, 0U);

    const std::string_view group{"default"};
    std::transform(group.begin(), group.end(), tables.begin() + 76,
                   [](const char value) {
                       return static_cast<std::byte>(value);
                   });

    write_le<std::uint64_t>(tables, 124U, 96U);
    write_le<std::uint32_t>(tables, 132U, 16'384U);
    write_le<std::uint32_t>(tables, 136U, 0U);
    write_le<std::uint64_t>(tables, 140U, 65'536U);
    const std::string_view device{"super"};
    std::transform(device.begin(), device.end(), tables.begin() + 148,
                   [](const char value) {
                       return static_cast<std::byte>(value);
                   });

    std::vector<std::byte> header(header_size);
    write_le<std::uint32_t>(header, 0U, 0x414c5030U);
    write_le<std::uint16_t>(header, 4U, 10U);
    write_le<std::uint16_t>(header, 6U, 0U);
    write_le<std::uint32_t>(header, 8U, header_size);
    write_le<std::uint32_t>(header, 44U, table_size);
    const auto tables_digest = sha256(tables);
    std::copy(tables_digest.begin(), tables_digest.end(), header.begin() + 48);

    const std::array descriptors{
        std::array<std::uint32_t, 3>{0U, 1U, 52U},
        std::array<std::uint32_t, 3>{52U, 1U, 24U},
        std::array<std::uint32_t, 3>{76U, 1U, 48U},
        std::array<std::uint32_t, 3>{124U, 1U, 64U},
    };
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        const auto offset = 80U + index * 12U;
        write_le<std::uint32_t>(header, offset, descriptors[index][0]);
        write_le<std::uint32_t>(header, offset + 4U, descriptors[index][1]);
        write_le<std::uint32_t>(header, offset + 8U, descriptors[index][2]);
    }
    set_checksum(header, 12U);

    std::vector<std::byte> area(4096U);
    std::copy(header.begin(), header.end(), area.begin());
    std::copy(tables.begin(), tables.end(), area.begin() + header_size);
    return area;
}

[[nodiscard]] std::vector<std::byte> fetched_super_fixture() {
    std::vector<std::byte> geometry(52U);
    write_le<std::uint32_t>(geometry, 0U, 0x616c4467U);
    write_le<std::uint32_t>(geometry, 4U, 52U);
    write_le<std::uint32_t>(geometry, 40U, 4096U);
    write_le<std::uint32_t>(geometry, 44U, 3U);
    write_le<std::uint32_t>(geometry, 48U, 4096U);
    set_checksum(geometry, 8U);

    const auto area = metadata_area();
    std::vector<std::byte> image(65'536U);
    for (const auto offset : {4096U, 8192U}) {
        std::copy(geometry.begin(), geometry.end(), image.begin() + offset);
    }
    for (std::size_t copy = 0; copy < 6U; ++copy) {
        std::copy(area.begin(), area.end(),
                  image.begin() + static_cast<std::ptrdiff_t>(12'288U +
                                                              copy * 4096U));
    }
    return image;
}

[[nodiscard]] std::span<const std::byte> sparse_raw_area(
    const std::vector<std::byte>& sparse) {
    CHECK(sparse.size() == 4148U);
    CHECK(read_le<std::uint32_t>(sparse, 0U) == 0xed26ff3aU);
    CHECK(read_le<std::uint32_t>(sparse, 16U) == 4U);
    CHECK(read_le<std::uint32_t>(sparse, 20U) == 2U);
    CHECK(read_le<std::uint16_t>(sparse, 28U) == 0xcac3U);
    CHECK(read_le<std::uint32_t>(sparse, 32U) == 3U);
    CHECK(read_le<std::uint16_t>(sparse, 40U) == 0xcac1U);
    return std::span<const std::byte>{sparse}.subspan(52U, 4096U);
}

void shrink_matches_official_fastboot_bytes() {
    const auto input = fetched_super_fixture();
    auto resized = kairosboot::image::resize_fetched_super_metadata(
        input, "system_a", 4096U);
    CHECK(resized.has_value());
    CHECK(kairosboot::image::sha256_hex(sha256(*resized)) ==
          "cfbff82e159066d192121db4a0988bd6fd091792dade519b1f80f23d2ce4b324");
    const auto raw = sparse_raw_area(*resized);
    CHECK(read_le<std::uint64_t>(raw, 180U) == 8U);
}

void backup_geometry_and_metadata_are_accepted() {
    auto input = fetched_super_fixture();
    input[4096U] = std::byte{};
    input[12'288U] = std::byte{};
    auto resized = kairosboot::image::resize_fetched_super_metadata(
        input, "system_a", 4096U);
    CHECK(resized.has_value());
    CHECK(kairosboot::image::sha256_hex(sha256(*resized)) ==
          "cfbff82e159066d192121db4a0988bd6fd091792dade519b1f80f23d2ce4b324");
}

void malformed_and_unknown_metadata_fail_closed() {
    auto malformed = fetched_super_fixture();
    malformed[4096U + 8U] ^= std::byte{1U};
    malformed[8192U + 8U] ^= std::byte{1U};
    auto invalid = kairosboot::image::resize_fetched_super_metadata(
        malformed, "system_a", 4096U);
    CHECK(!invalid.has_value());
    CHECK(invalid.error().kind ==
          kairosboot::image::SuperMetadataErrorKind::Malformed);

    const auto valid = fetched_super_fixture();
    auto unknown = kairosboot::image::resize_fetched_super_metadata(
        valid, "vendor_a", 4096U);
    CHECK(!unknown.has_value());
    CHECK(unknown.error().kind ==
          kairosboot::image::SuperMetadataErrorKind::PartitionNotFound);

    auto unsupported_slot = kairosboot::image::resize_fetched_super_metadata(
        valid, "system_z", 4096U);
    CHECK(!unsupported_slot.has_value());
    CHECK(unsupported_slot.error().kind ==
          kairosboot::image::SuperMetadataErrorKind::Unsupported);
}

void sizes_are_aligned_and_capacity_is_enforced() {
    const auto input = fetched_super_fixture();
    auto aligned = kairosboot::image::resize_fetched_super_metadata(
        input, "system_a", 1U);
    CHECK(aligned.has_value());
    CHECK(read_le<std::uint64_t>(sparse_raw_area(*aligned), 180U) == 8U);

    auto grown = kairosboot::image::resize_fetched_super_metadata(
        input, "system_a", 12'288U);
    CHECK(grown.has_value());
    const auto raw = sparse_raw_area(*grown);
    CHECK(read_le<std::uint32_t>(raw, 96U) == 1U);
    CHECK(read_le<std::uint64_t>(raw, 180U) == 24U);

    auto too_large = kairosboot::image::resize_fetched_super_metadata(
        input, "system_a", 65'536U);
    CHECK(!too_large.has_value());
    CHECK(too_large.error().kind ==
          kairosboot::image::SuperMetadataErrorKind::NoSpace);
}

using Test = std::pair<std::string_view, void (*)()>;

}  // namespace

int main() {
    const std::array tests{
        Test{"shrink matches official Fastboot bytes",
             shrink_matches_official_fastboot_bytes},
        Test{"backup geometry and metadata", backup_geometry_and_metadata_are_accepted},
        Test{"malformed and unknown metadata", malformed_and_unknown_metadata_fail_closed},
        Test{"alignment and capacity", sizes_are_aligned_and_capacity_is_enforced},
    };
    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
