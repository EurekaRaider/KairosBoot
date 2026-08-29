// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace kairosboot::fleet {

inline constexpr std::size_t kMaximumManifestScalarBytes = 4096U;
inline constexpr std::size_t kMaximumManifestArtifacts = 16384U;
inline constexpr std::size_t kMaximumManifestTargets = 256U;
inline constexpr std::size_t kMaximumManifestSelectorValues = 256U;
inline constexpr std::size_t kMaximumManifestSteps = 16384U;

struct ManifestSourceLocation final {
    std::uint32_t line{1U};
    std::uint32_t column{1U};

    [[nodiscard]] bool operator==(const ManifestSourceLocation&) const = default;
};

struct LocatedManifestString final {
    std::string value;
    ManifestSourceLocation location;

    [[nodiscard]] bool operator==(const LocatedManifestString&) const = default;
};

struct ManifestArtifact final {
    ManifestSourceLocation location;
    LocatedManifestString id;
    LocatedManifestString path;
    LocatedManifestString sha256;

    [[nodiscard]] bool operator==(const ManifestArtifact&) const = default;
};

enum class ManifestFlashSlot : std::uint8_t {
    Current,
    Other,
    All,
    A,
    B,
};

enum class ManifestActiveSlot : std::uint8_t {
    A,
    B,
    Other,
};

enum class ManifestRebootTarget : std::uint8_t {
    System,
    Bootloader,
    Recovery,
    Fastboot,
};

struct ManifestFlashStep final {
    LocatedManifestString partition;
    LocatedManifestString artifact;
    std::optional<ManifestFlashSlot> slot;
    std::optional<ManifestSourceLocation> slot_location;

    [[nodiscard]] bool operator==(const ManifestFlashStep&) const = default;
};

struct ManifestEraseStep final {
    LocatedManifestString partition;

    [[nodiscard]] bool operator==(const ManifestEraseStep&) const = default;
};

struct ManifestSetActiveStep final {
    ManifestActiveSlot slot{ManifestActiveSlot::A};
    ManifestSourceLocation slot_location;

    [[nodiscard]] bool operator==(const ManifestSetActiveStep&) const = default;
};

struct ManifestRebootStep final {
    ManifestRebootTarget target{ManifestRebootTarget::System};
    // Empty when the schema default `system` was applied.
    std::optional<ManifestSourceLocation> target_location;

    [[nodiscard]] bool operator==(const ManifestRebootStep&) const = default;
};

struct ManifestOemStep final {
    LocatedManifestString command;

    [[nodiscard]] bool operator==(const ManifestOemStep&) const = default;
};

using ManifestStepPayload =
    std::variant<ManifestFlashStep,
                 ManifestEraseStep,
                 ManifestSetActiveStep,
                 ManifestRebootStep,
                 ManifestOemStep>;

struct ManifestStep final {
    ManifestSourceLocation location;
    ManifestStepPayload payload;

    [[nodiscard]] bool operator==(const ManifestStep&) const = default;
};

struct ManifestSelector final {
    ManifestSourceLocation location;
    std::vector<LocatedManifestString> serials;
    std::vector<LocatedManifestString> usb_paths;

    [[nodiscard]] bool operator==(const ManifestSelector&) const = default;
};

struct ManifestTarget final {
    ManifestSourceLocation location;
    LocatedManifestString name;
    ManifestSelector selector;
    LocatedManifestString expected_product;
    std::vector<ManifestStep> steps;

    [[nodiscard]] bool operator==(const ManifestTarget&) const = default;
};

enum class ManifestDeviceFailurePolicy : std::uint8_t {
    Continue,
    Stop,
};

struct ManifestMemoryBudget final {
    bool automatic{true};
    std::uint64_t bytes{};
    std::optional<ManifestSourceLocation> location;

    [[nodiscard]] bool operator==(const ManifestMemoryBudget&) const = default;
};

struct ManifestPolicy final {
    ManifestDeviceFailurePolicy on_device_failure{
        ManifestDeviceFailurePolicy::Continue};
    std::uint32_t max_parallel_devices{32U};
    ManifestMemoryBudget memory_budget;
    std::optional<ManifestSourceLocation> location;
    std::optional<ManifestSourceLocation> on_device_failure_location;
    std::optional<ManifestSourceLocation> max_parallel_devices_location;

    [[nodiscard]] bool operator==(const ManifestPolicy&) const = default;
};

struct FlashJobManifest final {
    ManifestSourceLocation location;
    std::vector<ManifestArtifact> artifacts;
    std::vector<ManifestTarget> targets;
    ManifestPolicy policy;

    [[nodiscard]] bool operator==(const FlashJobManifest&) const = default;
};

}  // namespace kairosboot::fleet
