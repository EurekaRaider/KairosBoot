// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fastboot {

// The parser is intentionally internal. It owns every string in its result and
// never opens an artifact, queries a device, or performs a Fastboot operation.
enum class UpdateManifestKind : std::uint8_t {
    AndroidInfo,
    FastbootInfo,
};

struct UpdateSourceLocation final {
    UpdateManifestKind manifest{UpdateManifestKind::AndroidInfo};
    std::size_t line{1};
    std::size_t column{1};
    std::size_t byte_offset{0};

    friend bool operator==(const UpdateSourceLocation&,
                           const UpdateSourceLocation&) = default;
};

enum class UpdatePlanErrorCode : std::uint8_t {
    LimitExceeded,
    EmbeddedNul,
    Syntax,
    UnsupportedVersion,
    UnsafeArtifactPath,
    Duplicate,
};

struct UpdatePlanError final {
    UpdatePlanErrorCode code{UpdatePlanErrorCode::Syntax};
    UpdateSourceLocation location;
    std::string message;
};

struct UpdatePlanLimits final {
    std::size_t maximum_file_bytes{1024U * 1024U};
    std::size_t maximum_lines{65'536};
    std::size_t maximum_line_bytes{8U * 1024U};
    std::size_t maximum_field_bytes{1024};
    std::size_t maximum_tokens_per_line{16};
    std::size_t maximum_requirements{16'384};
    std::size_t maximum_options_per_requirement{256};
    std::size_t maximum_total_options{65'536};
    std::size_t maximum_tasks{16'384};
};

enum class RequirementAction : std::uint8_t {
    Require,
    Reject,
};

struct PlannedRequirement final {
    RequirementAction action{RequirementAction::Require};
    std::string variable;
    std::optional<std::string> product;
    std::vector<std::string> options;
    UpdateSourceLocation location;
};

enum class UpdateTaskKind : std::uint8_t {
    Flash,
    Reboot,
    UpdateSuper,
    Erase,
};

enum class PlannedSlot : std::uint8_t {
    Default,
    Other,
};

enum class PlannedRebootTarget : std::uint8_t {
    System,
    Bootloader,
    Recovery,
    Fastboot,
};

struct PlannedUpdateTask final {
    UpdateTaskKind kind{UpdateTaskKind::Flash};
    bool conditional_on_wipe{false};
    UpdateSourceLocation location;

    // Flash fields.
    std::string partition;
    std::string artifact;
    PlannedSlot slot{PlannedSlot::Default};
    bool apply_vbmeta{false};

    // Reboot field. System represents a bare `reboot` line.
    PlannedRebootTarget reboot_target{PlannedRebootTarget::System};
};

struct ParsedUpdateManifest final {
    std::vector<PlannedRequirement> requirements;
    std::vector<PlannedUpdateTask> tasks;
    std::optional<std::uint32_t> fastboot_info_version;
};

struct DeterministicUpdatePlan final {
    std::vector<PlannedRequirement> requirements;
    std::vector<PlannedUpdateTask> tasks;
    std::optional<std::uint32_t> fastboot_info_version;
};

// Parses the grammar used by frozen AOSP Platform-Tools 37.0.1. Security
// deviations are deliberate: malformed android-info lines are fatal, inactive
// if-wipe bodies are still validated, executable reboot targets are checked at
// planning time, artifact traversal is rejected, and ambiguous duplicates are
// rejected. The returned AST still contains conditional tasks.
[[nodiscard]] std::expected<ParsedUpdateManifest, UpdatePlanError>
parse_update_manifest(
    std::string_view android_info,
    std::string_view fastboot_info,
    const UpdatePlanLimits& limits = {});

// Applies the only device-independent condition in fastboot-info.txt while
// preserving AOSP source order. This function performs no I/O.
[[nodiscard]] DeterministicUpdatePlan make_update_plan(
    const ParsedUpdateManifest& manifest,
    bool wants_wipe);

}  // namespace kairosboot::fastboot
