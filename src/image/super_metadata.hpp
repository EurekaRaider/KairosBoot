// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::image {

enum class SuperMetadataErrorKind : std::uint8_t {
    Malformed,
    Unsupported,
    PartitionNotFound,
    NoSpace,
    Overflow,
};

struct SuperMetadataError final {
    SuperMetadataErrorKind kind{SuperMetadataErrorKind::Malformed};
    std::string message;
};

// Rewrites one logical partition in the metadata fetched from a physical
// super partition and returns the metadata-only Android sparse image used by
// AOSP Fastboot's bootloader-mode resize-logical-partition path. The input is
// bounded to the leading 1 MiB fetched by that command; no logical-partition
// payload bytes are materialized.
[[nodiscard]] std::expected<std::vector<std::byte>, SuperMetadataError>
resize_fetched_super_metadata(std::span<const std::byte> fetched_super,
                              std::string_view partition,
                              std::uint64_t requested_size);

}  // namespace kairosboot::image
