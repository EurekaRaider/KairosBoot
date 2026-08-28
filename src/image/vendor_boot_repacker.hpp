// SPDX-License-Identifier: MIT
#pragma once

#include "sparse_image.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>

namespace kairosboot::image {

enum class VendorBootRepackErrorKind : std::uint8_t {
    InvalidArgument,
    Malformed,
    Unsupported,
    Source,
    SizeOverflow,
    Cancelled,
    Allocation,
};

struct VendorBootRepackError final {
    VendorBootRepackErrorKind kind{VendorBootRepackErrorKind::Malformed};
    std::string message;
};

struct VendorBootRepackOptions final {
    // "default" replaces the complete v3/v4 vendor ramdisk. Any other name
    // replaces one unique v4 table fragment.
    std::string ramdisk_name{"default"};
    std::uint64_t maximum_image_bytes{0xffff'ffffULL};
};

struct VendorBootRepackMetadata final {
    std::uint32_t header_version{};
    std::uint32_t page_size{};
    std::uint32_t vendor_ramdisk_size{};
    std::uint32_t dtb_size{};
    std::uint32_t table_entry_count{};
};

// Immutable bounded-memory view of a repacked vendor_boot partition image.
// Payloads remain in their original random-access sources; only the header and
// changed v4 table fields are owned by this object.
class VendorBootImageSource final : public IImageSource {
public:
    ~VendorBootImageSource() override;

    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        std::uint64_t offset,
        std::span<std::byte> destination) const override;
    [[nodiscard]] const VendorBootRepackMetadata& metadata() const noexcept;

private:
    struct Impl;
    explicit VendorBootImageSource(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend std::expected<std::shared_ptr<const VendorBootImageSource>,
                         VendorBootRepackError>
    repack_vendor_boot(
        std::shared_ptr<const IImageSource>,
        std::shared_ptr<const IImageSource>,
        std::shared_ptr<const IImageSource>,
        VendorBootRepackOptions,
        std::stop_token);
};

[[nodiscard]] std::expected<std::shared_ptr<const VendorBootImageSource>,
                            VendorBootRepackError>
repack_vendor_boot(
    std::shared_ptr<const IImageSource> vendor_boot,
    std::shared_ptr<const IImageSource> new_ramdisk,
    std::shared_ptr<const IImageSource> new_dtb = {},
    VendorBootRepackOptions options = {},
    std::stop_token cancellation = {});

}  // namespace kairosboot::image
