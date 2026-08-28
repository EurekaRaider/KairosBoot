// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/update_package_preflight.hpp"

#include <cstdint>
#include <expected>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::fastboot {

enum class SuperOptimizationErrorKind : std::uint8_t {
    InvalidMetadata,
    IncompatibleLayout,
    SizeMismatch,
    SlotMismatch,
    NoSpace,
    Source,
    Cancelled,
};

struct SuperOptimizationError final {
    SuperOptimizationErrorKind kind{SuperOptimizationErrorKind::InvalidMetadata};
    std::string message{};
};

struct SuperOptimizationDeviceInfo final {
    std::string super_partition{"super"};
    std::string current_slot{};
    std::uint64_t super_partition_size{};
};

struct SuperOptimizationReport final {
    bool optimized{};
    std::vector<std::string> absorbed_partitions{};
};

// This is only a structural check. It performs no reads, allocation or device
// queries and is useful for avoiding optional getvars when the frozen AOSP
// reboot-fastboot/update-super/dynamic-flash window is absent.
[[nodiscard]] bool has_super_optimization_candidate(
    const PreparedUpdatePackage& prepared) noexcept;

// Validates the immutable super_empty.img LP geometry, every metadata copy,
// table checksum, partition/group/block-device relationship and the reported
// target size. When the AOSP optimization window is present and all referenced
// logical images are raw, it replaces that window with one deterministic sparse
// flash of the physical super partition. The returned source is a piece table:
// partition payloads are never copied into a second full image.
[[nodiscard]] std::expected<SuperOptimizationReport, SuperOptimizationError>
optimize_prepared_super(
    PreparedUpdatePackage& prepared,
    const SuperOptimizationDeviceInfo& device,
    std::stop_token cancellation = {});

}  // namespace kairosboot::fastboot
