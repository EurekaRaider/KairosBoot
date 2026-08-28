// SPDX-License-Identifier: MIT
#include "src/image/boot_image_builder.hpp"
#include "src/image/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::image::BootImageBuildErrorKind;
using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::LegacyBootImageOptions;
using kairosboot::image::build_legacy_boot_image;
using kairosboot::image::compute_sha256;
using kairosboot::image::sha256_hex;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            throw std::runtime_error(                                           \
                std::string{"check failed at line "} + std::to_string(__LINE__) + \
                ": " #condition);                                              \
        }                                                                      \
    } while (false)

class MemorySource final : public IImageSource {
public:
    explicit MemorySource(std::string_view text) {
        bytes_.reserve(text.size());
        for (const char value : text) {
            bytes_.push_back(static_cast<std::byte>(value));
        }
    }

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset >= bytes_.size()) {
            return std::size_t{0};
        }
        const auto amount = std::min<std::size_t>(
            destination.size(), bytes_.size() - static_cast<std::size_t>(offset));
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                    amount,
                    destination.begin());
        return amount;
    }

private:
    std::vector<std::byte> bytes_;
};

class DeclaredSource final : public IImageSource {
public:
    explicit DeclaredSource(const std::uint64_t size) : size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        std::uint64_t,
        std::span<std::byte>) const override {
        return std::size_t{0};
    }

private:
    std::uint64_t size_{};
};

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::vector<std::byte> materialize(const IImageSource& source) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(source.size()));
    std::size_t completed = 0U;
    while (completed < bytes.size()) {
        auto read = source.read_at(completed, std::span(bytes).subspan(completed));
        CHECK(read.has_value());
        CHECK(*read != 0U);
        completed += *read;
    }
    return bytes;
}

[[nodiscard]] bool matches(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::string_view expected) {
    if (offset > bytes.size() || expected.size() > bytes.size() - offset) {
        return false;
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::byte>(expected[index])) {
            return false;
        }
    }
    return true;
}

void canonical_v0_layout_addresses_hash_and_padding() {
    const auto kernel = std::make_shared<MemorySource>("KERNEL");
    const auto ramdisk = std::make_shared<MemorySource>("RAM");
    const auto second = std::make_shared<MemorySource>("S");
    LegacyBootImageOptions options;
    options.command_line = "console=ttyS0";

    const auto built = build_legacy_boot_image(kernel, ramdisk, second, options);
    CHECK(built.has_value());
    CHECK((*built)->size() == 8192U);
    const auto bytes = materialize(**built);
    const std::array magic{
        std::byte{'A'}, std::byte{'N'}, std::byte{'D'}, std::byte{'R'},
        std::byte{'O'}, std::byte{'I'}, std::byte{'D'}, std::byte{'!'},
    };
    CHECK(std::equal(magic.begin(), magic.end(), bytes.begin()));
    CHECK(read_u32(bytes, 8U) == 6U);
    CHECK(read_u32(bytes, 12U) == 0x10008000U);
    CHECK(read_u32(bytes, 16U) == 3U);
    CHECK(read_u32(bytes, 20U) == 0x11000000U);
    CHECK(read_u32(bytes, 24U) == 1U);
    CHECK(read_u32(bytes, 28U) == 0x10f00000U);
    CHECK(read_u32(bytes, 32U) == 0x10000100U);
    CHECK(read_u32(bytes, 36U) == 2048U);
    CHECK(read_u32(bytes, 40U) == 0U);
    CHECK(matches(bytes, 64U, "console=ttyS0"));
    CHECK(matches(bytes, 2048U, "KERNEL"));
    CHECK(matches(bytes, 4096U, "RAM"));
    CHECK(bytes[6144] == std::byte{'S'});
    CHECK(std::all_of(bytes.begin() + 2054, bytes.begin() + 4096,
                      [](const std::byte value) { return value == std::byte{0}; }));

    const auto digest = compute_sha256(**built);
    CHECK(digest.has_value());
    CHECK(sha256_hex(*digest) ==
          "38d9454587604cecfda25439bca1e1a93fc13a9cc77490c68c1ca1bb6b7f8d55");
}

void command_line_split_and_custom_layout_are_exact() {
    LegacyBootImageOptions options;
    options.command_line = std::string(700U, 'x');
    options.base = 0x20000000U;
    options.page_size = 4096U;
    options.kernel_offset = 0x1000U;
    options.ramdisk_offset = 0x2000U;
    options.second_offset = 0x3000U;
    options.tags_offset = 0x4000U;
    const auto built = build_legacy_boot_image(
        std::make_shared<MemorySource>("k"), {}, {}, options);
    CHECK(built.has_value());
    const auto bytes = materialize(**built);
    CHECK(bytes.size() == 8192U);
    CHECK(read_u32(bytes, 12U) == 0x20001000U);
    CHECK(read_u32(bytes, 20U) == 0x20002000U);
    CHECK(read_u32(bytes, 28U) == 0x20003000U);
    CHECK(read_u32(bytes, 32U) == 0x20004000U);
    CHECK(std::all_of(bytes.begin() + 64, bytes.begin() + 575,
                      [](const std::byte value) { return value == std::byte{'x'}; }));
    CHECK(bytes[575] == std::byte{0});
    CHECK(std::all_of(bytes.begin() + 608, bytes.begin() + 797,
                      [](const std::byte value) { return value == std::byte{'x'}; }));
    CHECK(bytes[797] == std::byte{0});
}

void reads_cross_piece_boundaries_without_copying_payloads() {
    const auto built = build_legacy_boot_image(
        std::make_shared<MemorySource>("abcdef"),
        std::make_shared<MemorySource>("ghi"));
    CHECK(built.has_value());
    std::array<std::byte, 10> boundary{};
    const auto read = (*built)->read_at(2045U, boundary);
    CHECK(read.has_value());
    CHECK(*read == boundary.size());
    CHECK(boundary[0] == std::byte{0});
    CHECK(boundary[2] == std::byte{0});
    CHECK(boundary[3] == std::byte{'a'});
    CHECK(boundary[8] == std::byte{'f'});
    CHECK(boundary[9] == std::byte{0});
}

void rejects_unsupported_or_unrepresentable_inputs_before_publication() {
    const auto kernel = std::make_shared<MemorySource>("k");
    {
        auto options = LegacyBootImageOptions{};
        options.page_size = 1024U;
        const auto result = build_legacy_boot_image(kernel, {}, {}, options);
        CHECK(!result.has_value());
        CHECK(result.error().kind == BootImageBuildErrorKind::InvalidArgument);
    }
    {
        auto options = LegacyBootImageOptions{};
        options.command_line = std::string(1535U, 'x');
        const auto result = build_legacy_boot_image(kernel, {}, {}, options);
        CHECK(!result.has_value());
        CHECK(result.error().kind == BootImageBuildErrorKind::InvalidArgument);
    }
    {
        auto options = LegacyBootImageOptions{};
        options.base = std::numeric_limits<std::uint32_t>::max();
        const auto result = build_legacy_boot_image(kernel, {}, {}, options);
        CHECK(!result.has_value());
        CHECK(result.error().kind == BootImageBuildErrorKind::SizeOverflow);
    }
    {
        const auto huge = std::make_shared<DeclaredSource>(
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
            1U);
        const auto result = build_legacy_boot_image(huge);
        CHECK(!result.has_value());
        CHECK(result.error().kind == BootImageBuildErrorKind::SizeOverflow);
    }
    {
        auto options = LegacyBootImageOptions{};
        options.maximum_output_bytes = 2048U;
        const auto result = build_legacy_boot_image(kernel, {}, {}, options);
        CHECK(!result.has_value());
        CHECK(result.error().kind == BootImageBuildErrorKind::SizeOverflow);
    }
    CHECK(!build_legacy_boot_image(nullptr).has_value());
}

void source_contract_and_cancellation_are_reported() {
    {
        const auto result = build_legacy_boot_image(
            std::make_shared<DeclaredSource>(1U));
        CHECK(!result.has_value());
        CHECK(result.error().kind == BootImageBuildErrorKind::Truncated);
    }
    {
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto result = build_legacy_boot_image(
            std::make_shared<MemorySource>("kernel"),
            {},
            {},
            {},
            cancellation.get_token());
        CHECK(!result.has_value());
        CHECK(result.error().kind == BootImageBuildErrorKind::Cancelled);
    }
}

}  // namespace

int main() {
    using TestCase = std::pair<std::string_view, void (*)()>;
    const std::array<TestCase, 5> tests{{
        {"canonical v0", canonical_v0_layout_addresses_hash_and_padding},
        {"cmdline and custom layout", command_line_split_and_custom_layout_are_exact},
        {"cross-piece reads", reads_cross_piece_boundaries_without_copying_payloads},
        {"invalid boundaries", rejects_unsupported_or_unrepresentable_inputs_before_publication},
        {"source and cancellation", source_contract_and_cancellation_are_reported},
    }};
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
