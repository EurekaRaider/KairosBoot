// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/update_plan.hpp"
#include "src/image/artifact_source.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::fastboot {

enum class UpdatePackagePreflightErrorKind : std::uint8_t {
    MissingAndroidInfo,
    Artifact,
    ManifestRead,
    Manifest,
    LimitExceeded,
    Cancelled,
};

struct UpdatePackagePreflightError final {
    UpdatePackagePreflightErrorKind kind{UpdatePackagePreflightErrorKind::Artifact};
    std::string message;
    std::string artifact;
    std::optional<image::ArtifactSourceError> artifact_error;
    std::optional<UpdatePlanError> manifest_error;
};

struct UpdatePackagePreflightLimits final {
    UpdatePlanLimits manifest{};
    std::size_t maximum_unique_artifacts{16'384U};
    std::uint64_t maximum_total_artifact_bytes{512ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct PreparedUpdateArtifact final {
    std::string name;
    std::shared_ptr<const image::ResolvedArtifact> resolved;
    image::FlashArtifact artifact;
};

struct PreparedUpdatePackage final {
    DeterministicUpdatePlan plan;
    std::vector<PreparedUpdateArtifact> artifacts;

    // Requirements are intentionally not queried during transport-free
    // preflight. The execution layer must validate them against one uniquely
    // selected device before any destructive task.
    bool requires_device_validation{};
};

// Resolves both manifests, parses the frozen AOSP grammar, applies the wipe
// condition, and materializes every artifact referenced by the resulting plan.
// It has no transport dependency and returns no partial plan on failure.
[[nodiscard]] std::expected<PreparedUpdatePackage, UpdatePackagePreflightError>
preflight_update_package(image::ArtifactSourceResolver& resolver,
                         const std::filesystem::path& package_directory_or_zip,
                         bool wants_wipe,
                         const UpdatePackagePreflightLimits& limits = {},
                         std::stop_token cancellation = {});

}  // namespace kairosboot::fastboot
