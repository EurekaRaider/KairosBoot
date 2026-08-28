// SPDX-License-Identifier: MIT
#pragma once

#include "sparse_image.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>

namespace kairosboot::image {

enum class BootImageBuildErrorKind : std::uint8_t {
    InvalidArgument,
    SizeOverflow,
    Source,
    Truncated,
    Cancelled,
    Allocation,
};

struct BootImageBuildError final {
    BootImageBuildErrorKind kind{BootImageBuildErrorKind::InvalidArgument};
    std::string message;
};

// Address and layout defaults match the image produced by AOSP fastboot's
// `boot <kernel> [ramdisk [second]]` path. The historical type name is kept for
// source compatibility, but header_version selects Android boot header v0-v4.
// DTB is supported only by v2; v3/v4 use their fixed 4096-byte layout.
struct LegacyBootImageOptions final {
    std::string command_line;
    std::uint32_t base{0x10000000U};
    std::uint32_t page_size{2048U};
    std::uint32_t kernel_offset{0x00008000U};
    std::uint32_t ramdisk_offset{0x01000000U};
    std::uint32_t second_offset{0x00f00000U};
    std::uint32_t tags_offset{0x00000100U};
    std::uint32_t header_version{};
    std::string os_version;
    std::string os_patch_level;
    std::string dtb_path;
    std::uint64_t dtb_offset{0x01100000ULL};
    std::uint64_t maximum_output_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
};

// Immutable, bounded-memory composite image. Component bytes remain in their
// original random-access sources; only one header page and a small piece table
// are owned by the result.
class LegacyBootImageSource final : public IImageSource {
public:
    ~LegacyBootImageSource() override;

    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        std::uint64_t offset,
        std::span<std::byte> destination) const override;

private:
    struct Impl;
    explicit LegacyBootImageSource(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend std::expected<std::shared_ptr<const LegacyBootImageSource>,
                         BootImageBuildError>
    build_legacy_boot_image(
        std::shared_ptr<const IImageSource>,
        std::shared_ptr<const IImageSource>,
        std::shared_ptr<const IImageSource>,
        LegacyBootImageOptions,
        std::stop_token);

    friend std::expected<std::shared_ptr<const LegacyBootImageSource>,
                         BootImageBuildError>
    build_boot_image(
        std::shared_ptr<const IImageSource>,
        std::shared_ptr<const IImageSource>,
        std::shared_ptr<const IImageSource>,
        std::shared_ptr<const IImageSource>,
        LegacyBootImageOptions,
        std::stop_token);
};

[[nodiscard]] std::expected<std::shared_ptr<const LegacyBootImageSource>,
                            BootImageBuildError>
build_legacy_boot_image(
    std::shared_ptr<const IImageSource> kernel,
    std::shared_ptr<const IImageSource> ramdisk = {},
    std::shared_ptr<const IImageSource> second = {},
    LegacyBootImageOptions options = {},
    std::stop_token cancellation = {});

// General v0-v4 builder. Payloads remain random-access sources in the returned
// piece table, so even very large components are not copied into one buffer.
[[nodiscard]] std::expected<std::shared_ptr<const LegacyBootImageSource>,
                            BootImageBuildError>
build_boot_image(
    std::shared_ptr<const IImageSource> kernel,
    std::shared_ptr<const IImageSource> ramdisk,
    std::shared_ptr<const IImageSource> second,
    std::shared_ptr<const IImageSource> dtb,
    LegacyBootImageOptions options = {},
    std::stop_token cancellation = {});

}  // namespace kairosboot::image
