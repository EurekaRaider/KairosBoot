// SPDX-License-Identifier: MIT
#include "src/image/vendor_boot_repacker.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::VendorBootRepackErrorKind;
using kairosboot::image::VendorBootRepackOptions;
using kairosboot::image::repack_vendor_boot;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            throw std::runtime_error(                                            \
                std::string{"check failed at line "} +                          \
                std::to_string(__LINE__) + ": " #condition);                   \
        }                                                                       \
    } while (false)

inline constexpr std::uint32_t kPage = 4096U;
inline constexpr std::uint32_t kHeaderV3Size = 2112U;
inline constexpr std::uint32_t kHeaderV4Size = 2128U;

class MemorySource final : public IImageSource {
public:
    explicit MemorySource(std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)) {}

    explicit MemorySource(const std::string_view text) {
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
                    amount, destination.begin());
        return amount;
    }

private:
    std::vector<std::byte> bytes_;
};

void write_u32(std::vector<std::byte>& bytes,
               const std::size_t offset,
               const std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index])
                 << (index * 8U);
    }
    return value;
}

void write_text(std::vector<std::byte>& bytes,
                const std::size_t offset,
                const std::string_view text) {
    for (std::size_t index = 0U; index < text.size(); ++index) {
        bytes[offset + index] = static_cast<std::byte>(text[index]);
    }
}

[[nodiscard]] bool matches(const std::span<const std::byte> bytes,
                           const std::size_t offset,
                           const std::string_view text) {
    return offset <= bytes.size() && text.size() <= bytes.size() - offset &&
           std::equal(text.begin(), text.end(), bytes.begin() +
                                                    static_cast<std::ptrdiff_t>(offset),
                      [](const char left, const std::byte right) {
                          return static_cast<std::byte>(left) == right;
                      });
}

[[nodiscard]] std::vector<std::byte> materialize(const IImageSource& source) {
    std::vector<std::byte> result(static_cast<std::size_t>(source.size()));
    std::size_t completed = 0U;
    while (completed < result.size()) {
        auto read = source.read_at(completed, std::span(result).subspan(completed));
        CHECK(read.has_value());
        CHECK(*read != 0U);
        completed += *read;
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> header(
    const std::uint32_t version,
    const std::uint32_t ramdisk_size,
    const std::uint32_t dtb_size) {
    std::vector<std::byte> bytes(kPage, std::byte{0xa5});
    write_text(bytes, 0U, "VNDRBOOT");
    write_u32(bytes, 8U, version);
    write_u32(bytes, 12U, kPage);
    write_u32(bytes, 24U, ramdisk_size);
    write_u32(bytes, 2096U, version == 3U ? kHeaderV3Size : kHeaderV4Size);
    write_u32(bytes, 2100U, dtb_size);
    return bytes;
}

[[nodiscard]] std::vector<std::byte> v3_image() {
    std::vector<std::byte> bytes(6U * kPage, std::byte{0x7b});
    auto hdr = header(3U, 3U, 3U);
    std::copy(hdr.begin(), hdr.end(), bytes.begin());
    write_text(bytes, kPage, "old");
    std::fill(bytes.begin() + kPage + 3U, bytes.begin() + 2U * kPage,
              std::byte{0});
    write_text(bytes, 2U * kPage, "dtb");
    std::fill(bytes.begin() + 2U * kPage + 3U,
              bytes.begin() + 3U * kPage, std::byte{0});
    write_text(bytes, 5U * kPage + 17U, "TAIL");
    return bytes;
}

[[nodiscard]] std::vector<std::byte> v4_image() {
    constexpr std::uint32_t entry_size = 112U;
    constexpr std::uint32_t table_size = 2U * entry_size;
    std::vector<std::byte> bytes(7U * kPage, std::byte{0x6c});
    auto hdr = header(4U, 7U, 3U);
    write_u32(hdr, 2112U, table_size);
    write_u32(hdr, 2116U, 2U);
    write_u32(hdr, 2120U, entry_size);
    write_u32(hdr, 2124U, 4U);
    std::copy(hdr.begin(), hdr.end(), bytes.begin());

    write_text(bytes, kPage, "oneTWO2");
    std::fill(bytes.begin() + kPage + 7U, bytes.begin() + 2U * kPage,
              std::byte{0});
    write_text(bytes, 2U * kPage, "dtb");
    std::fill(bytes.begin() + 2U * kPage + 3U,
              bytes.begin() + 3U * kPage, std::byte{0});

    const auto table = 3U * kPage;
    write_u32(bytes, table, 3U);
    write_u32(bytes, table + 4U, 0U);
    write_u32(bytes, table + 8U, 1U);
    std::fill(bytes.begin() + table + 12U, bytes.begin() + table + 44U,
              std::byte{0});
    write_text(bytes, table + 12U, "alpha");
    bytes[table + 108U] = std::byte{0xd1};
    bytes[table + 109U] = std::byte{0xd2};
    const auto second = table + entry_size;
    write_u32(bytes, second, 4U);
    write_u32(bytes, second + 4U, 3U);
    write_u32(bytes, second + 8U, 3U);
    std::fill(bytes.begin() + second + 12U, bytes.begin() + second + 44U,
              std::byte{0});
    write_text(bytes, second + 12U, "beta");
    bytes[second + 44U] = std::byte{0xb7};
    bytes[second + 108U] = std::byte{0xe1};
    std::fill(bytes.begin() + table + table_size,
              bytes.begin() + 4U * kPage, std::byte{0});

    write_text(bytes, 4U * kPage, "boot");
    std::fill(bytes.begin() + 4U * kPage + 4U,
              bytes.begin() + 5U * kPage, std::byte{0});
    write_text(bytes, 6U * kPage + 11U, "TAIL");
    return bytes;
}

void v3_default_replacement_and_dtb_are_bounded_and_exact() {
    const auto built = repack_vendor_boot(
        std::make_shared<MemorySource>(v3_image()),
        std::make_shared<MemorySource>("new-ramdisk"),
        std::make_shared<MemorySource>("new-dtb"));
    CHECK(built.has_value());
    CHECK((*built)->size() == 6U * kPage);
    CHECK((*built)->metadata().header_version == 3U);
    CHECK((*built)->metadata().vendor_ramdisk_size == 11U);
    CHECK((*built)->metadata().dtb_size == 7U);
    const auto bytes = materialize(**built);
    CHECK(read_u32(bytes, 24U) == 11U);
    CHECK(read_u32(bytes, 2100U) == 7U);
    CHECK(bytes[3000U] == std::byte{0xa5});
    CHECK(matches(bytes, kPage, "new-ramdisk"));
    CHECK(matches(bytes, 2U * kPage, "new-dtb"));
    CHECK(matches(bytes, 5U * kPage + 17U, "TAIL"));
}

void v4_named_fragment_preserves_table_extensions_and_other_sections() {
    VendorBootRepackOptions options;
    options.ramdisk_name = "alpha";
    const auto built = repack_vendor_boot(
        std::make_shared<MemorySource>(v4_image()),
        std::make_shared<MemorySource>("FIRST"), {}, options);
    if (!built) {
        throw std::runtime_error(built.error().message);
    }
    CHECK(built.has_value());
    CHECK((*built)->metadata().header_version == 4U);
    CHECK((*built)->metadata().vendor_ramdisk_size == 9U);
    CHECK((*built)->metadata().table_entry_count == 2U);
    const auto bytes = materialize(**built);
    CHECK(matches(bytes, kPage, "FIRSTTWO2"));
    CHECK(matches(bytes, 2U * kPage, "dtb"));
    const auto table = 3U * kPage;
    CHECK(read_u32(bytes, table) == 5U);
    CHECK(read_u32(bytes, table + 4U) == 0U);
    CHECK(read_u32(bytes, table + 112U) == 4U);
    CHECK(read_u32(bytes, table + 116U) == 5U);
    CHECK(bytes[table + 108U] == std::byte{0xd1});
    CHECK(bytes[table + 112U + 44U] == std::byte{0xb7});
    CHECK(bytes[table + 112U + 108U] == std::byte{0xe1});
    CHECK(matches(bytes, 4U * kPage, "boot"));
    CHECK(matches(bytes, 6U * kPage + 11U, "TAIL"));
}

void v4_default_replacement_collapses_the_fragment_table() {
    const auto built = repack_vendor_boot(
        std::make_shared<MemorySource>(v4_image()),
        std::make_shared<MemorySource>("all"));
    CHECK(built.has_value());
    const auto bytes = materialize(**built);
    CHECK(read_u32(bytes, 24U) == 3U);
    CHECK(read_u32(bytes, 2112U) == 108U);
    CHECK(read_u32(bytes, 2116U) == 1U);
    CHECK(read_u32(bytes, 2120U) == 108U);
    CHECK(read_u32(bytes, 3U * kPage) == 3U);
    CHECK(read_u32(bytes, 3U * kPage + 4U) == 0U);
    CHECK(std::all_of(bytes.begin() + 3U * kPage + 8U,
                      bytes.begin() + 3U * kPage + 108U,
                      [](const std::byte value) { return value == std::byte{0}; }));
}

void malformed_unsupported_and_cancelled_images_fail_closed() {
    {
        auto bytes = v3_image();
        bytes[0] = std::byte{'X'};
        const auto result = repack_vendor_boot(
            std::make_shared<MemorySource>(std::move(bytes)),
            std::make_shared<MemorySource>("new"));
        CHECK(!result.has_value());
        CHECK(result.error().kind == VendorBootRepackErrorKind::Malformed);
    }
    {
        VendorBootRepackOptions options;
        options.ramdisk_name = "alpha";
        const auto result = repack_vendor_boot(
            std::make_shared<MemorySource>(v3_image()),
            std::make_shared<MemorySource>("new"), {}, options);
        CHECK(!result.has_value());
        CHECK(result.error().kind == VendorBootRepackErrorKind::Unsupported);
    }
    {
        auto bytes = v4_image();
        write_u32(bytes, 3U * kPage + 112U + 4U, 99U);
        const auto result = repack_vendor_boot(
            std::make_shared<MemorySource>(std::move(bytes)),
            std::make_shared<MemorySource>("new"));
        CHECK(!result.has_value());
        CHECK(result.error().kind == VendorBootRepackErrorKind::Malformed);
    }
    {
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto result = repack_vendor_boot(
            std::make_shared<MemorySource>(v4_image()),
            std::make_shared<MemorySource>("new"), {}, {},
            cancellation.get_token());
        CHECK(!result.has_value());
        CHECK(result.error().kind == VendorBootRepackErrorKind::Cancelled);
    }
}

}  // namespace

int main() {
    const std::array tests{
        std::pair{"v3 default and dtb",
                  &v3_default_replacement_and_dtb_are_bounded_and_exact},
        std::pair{"v4 named fragment",
                  &v4_named_fragment_preserves_table_extensions_and_other_sections},
        std::pair{"v4 default",
                  &v4_default_replacement_collapses_the_fragment_table},
        std::pair{"fail closed",
                  &malformed_unsupported_and_cancelled_images_fail_closed},
    };
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& exception) {
            std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
            return 1;
        }
    }
    return 0;
}
