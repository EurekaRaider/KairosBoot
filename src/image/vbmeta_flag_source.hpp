// SPDX-License-Identifier: MIT
#pragma once

#include "sparse_image.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace kairosboot::image {

struct VbmetaFlags final {
    bool disable_verity{};
    bool disable_verification{};

    [[nodiscard]] bool any() const noexcept {
        return disable_verity || disable_verification;
    }
};

enum class VbmetaFlagErrorKind : std::uint8_t {
    Malformed,
    Source,
    Cancelled,
};

struct VbmetaFlagError final {
    VbmetaFlagErrorKind kind{VbmetaFlagErrorKind::Source};
    std::uint64_t input_offset{};
    std::string message{};
};

// Matches Platform-Tools 37.0.1's partition predicate, including prefixed
// virtual-machine names such as guest_vbmeta_a.
[[nodiscard]] bool is_vbmeta_partition(std::string_view partition) noexcept;

// Produces an immutable overlay that changes only the big-endian AVB flags
// byte at offset 123. Creation validates the complete 256-byte vbmeta header
// before publishing the overlay. As in Platform-Tools 37.0.1, inputs shorter
// than 256 bytes are passed through unchanged, while a full-size input without
// AVB0 magic is rejected. The original source is never modified or copied.
[[nodiscard]] std::expected<std::shared_ptr<const IImageSource>, VbmetaFlagError>
apply_vbmeta_flags(std::shared_ptr<const IImageSource> source,
                   VbmetaFlags flags,
                   std::stop_token cancellation = {});

}  // namespace kairosboot::image
