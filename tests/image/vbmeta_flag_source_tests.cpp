// SPDX-License-Identifier: MIT
#include "src/image/vbmeta_flag_source.hpp"

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
#include <utility>
#include <vector>

namespace {

using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::VbmetaFlagErrorKind;
using kairosboot::image::VbmetaFlags;
using kairosboot::image::apply_vbmeta_flags;
using kairosboot::image::is_vbmeta_partition;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw std::runtime_error(                                          \
                std::string{"check failed at line "} +                         \
                std::to_string(__LINE__) + ": " #condition);                  \
        }                                                                      \
    } while (false)

class MemorySource final : public IImageSource {
public:
    explicit MemorySource(std::vector<std::byte> bytes,
                          const std::size_t maximum_read = 4096U)
        : bytes_(std::move(bytes)), maximum_read_(maximum_read) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        ++read_count_;
        if (offset >= bytes_.size()) {
            return std::size_t{0};
        }
        const auto amount = std::min(
            {destination.size(), maximum_read_,
             bytes_.size() - static_cast<std::size_t>(offset)});
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                    amount, destination.begin());
        return amount;
    }

    [[nodiscard]] std::size_t read_count() const noexcept { return read_count_; }

private:
    std::vector<std::byte> bytes_;
    std::size_t maximum_read_{};
    mutable std::size_t read_count_{};
};

class FailingSource final : public IImageSource {
public:
    [[nodiscard]] std::uint64_t size() const noexcept override { return 256U; }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        std::uint64_t, std::span<std::byte>) const override {
        return std::unexpected(ImageSourceError{"injected source failure"});
    }
};

[[nodiscard]] std::vector<std::byte> valid_image(const std::size_t size = 4096U) {
    std::vector<std::byte> bytes(size, std::byte{0x5aU});
    bytes[0] = std::byte{'A'};
    bytes[1] = std::byte{'V'};
    bytes[2] = std::byte{'B'};
    bytes[3] = std::byte{'0'};
    bytes[123] = std::byte{0x40U};
    return bytes;
}

void partition_predicate_matches_aosp() {
    CHECK(is_vbmeta_partition("vbmeta"));
    CHECK(is_vbmeta_partition("vbmeta_a"));
    CHECK(is_vbmeta_partition("guest_vbmeta_b"));
    CHECK(!is_vbmeta_partition("vbmeta_system"));
    CHECK(!is_vbmeta_partition("boot"));
}

void flags_are_overlaid_without_copying_or_mutating_source() {
    auto source = std::make_shared<MemorySource>(valid_image(), 37U);
    auto transformed = apply_vbmeta_flags(
        source, VbmetaFlags{.disable_verity = true,
                            .disable_verification = true});
    CHECK(transformed.has_value());
    CHECK(*transformed != source);
    CHECK(source->read_count() == 7U);

    std::array<std::byte, 5> window{};
    auto read = (*transformed)->read_at(121U, window);
    CHECK(read.has_value());
    CHECK(*read == window.size());
    CHECK(window[2] == std::byte{0x43U});

    std::array<std::byte, 1> original{};
    read = source->read_at(123U, original);
    CHECK(read.has_value());
    CHECK(original[0] == std::byte{0x40U});
    CHECK((*transformed)->size() == source->size());
}

void no_op_and_short_inputs_match_aosp() {
    auto valid = std::make_shared<MemorySource>(valid_image());
    auto no_flags = apply_vbmeta_flags(valid, {});
    CHECK(no_flags.has_value());
    CHECK(*no_flags == valid);
    CHECK(valid->read_count() == 0U);

    auto short_source = std::make_shared<MemorySource>(
        std::vector<std::byte>(255U, std::byte{0xffU}));
    auto short_result = apply_vbmeta_flags(
        short_source, VbmetaFlags{.disable_verity = true});
    CHECK(short_result.has_value());
    CHECK(*short_result == short_source);
    CHECK(short_source->read_count() == 0U);

    auto already_set = valid_image();
    already_set[123] = std::byte{0x43U};
    auto set_source = std::make_shared<MemorySource>(std::move(already_set));
    auto set_result = apply_vbmeta_flags(
        set_source, VbmetaFlags{.disable_verity = true,
                                .disable_verification = true});
    CHECK(set_result.has_value());
    CHECK(*set_result == set_source);
}

void malformed_source_and_cancel_are_bounded_failures() {
    auto corrupt = valid_image(256U);
    corrupt[0] = std::byte{'X'};
    auto corrupt_result = apply_vbmeta_flags(
        std::make_shared<MemorySource>(std::move(corrupt)),
        VbmetaFlags{.disable_verification = true});
    CHECK(!corrupt_result.has_value());
    CHECK(corrupt_result.error().kind == VbmetaFlagErrorKind::Malformed);
    CHECK(corrupt_result.error().input_offset == 0U);

    auto failed = apply_vbmeta_flags(
        std::make_shared<FailingSource>(),
        VbmetaFlags{.disable_verity = true});
    CHECK(!failed.has_value());
    CHECK(failed.error().kind == VbmetaFlagErrorKind::Source);

    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = apply_vbmeta_flags(
        std::make_shared<MemorySource>(valid_image()),
        VbmetaFlags{.disable_verity = true}, cancellation.get_token());
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().kind == VbmetaFlagErrorKind::Cancelled);
}

}  // namespace

int main() {
    try {
        partition_predicate_matches_aosp();
        flags_are_overlaid_without_copying_or_mutating_source();
        no_op_and_short_inputs_match_aosp();
        malformed_source_and_cancel_are_bounded_failures();
        std::cout << "vbmeta flag source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
