// SPDX-License-Identifier: MIT
#pragma once

#include "src/fastboot/update_plan.hpp"
#include "src/image/artifact_source.hpp"

#include <chrono>
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
    std::string message{};
    std::string artifact{};
    std::optional<image::ArtifactSourceError> artifact_error{};
    std::optional<UpdatePlanError> manifest_error{};
};

struct UpdatePackagePreflightLimits final {
    UpdatePlanLimits manifest{};
    std::size_t maximum_unique_artifacts{16'384U};
    std::uint64_t maximum_total_artifact_bytes{512ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct PreparedUpdateArtifact final {
    std::string name{};
    std::shared_ptr<const image::ResolvedArtifact> resolved{};
    // Own the complete immutable preflight result. Device execute tokens can
    // retain this object without copying metadata or reparsing sparse images.
    std::shared_ptr<const image::FlashArtifact> artifact{};
};

enum class UpdateSuperPreparationState : std::uint8_t {
    NotRequired,
    SkippedNotFound,
    Prepared,
};

// The prepared dynamic-partition metadata is immutable and package-scoped.
// Execution adapters must consume this snapshot instead of reopening the
// directory or ZIP after preflight.
class PreparedSuperArtifact final {
public:
    PreparedSuperArtifact(
        std::shared_ptr<const image::ResolvedArtifact> resolved,
        std::shared_ptr<const image::FlashArtifact> artifact,
        bool wants_wipe = false) noexcept;

    [[nodiscard]] const std::shared_ptr<const image::ResolvedArtifact>& resolved()
        const noexcept;
    [[nodiscard]] const std::shared_ptr<const image::FlashArtifact>& artifact()
        const noexcept;
    [[nodiscard]] bool wants_wipe() const noexcept;

private:
    std::shared_ptr<const image::ResolvedArtifact> resolved_;
    std::shared_ptr<const image::FlashArtifact> artifact_;
    bool wants_wipe_{};
};

struct PreparedUpdatePackage final {
    DeterministicUpdatePlan plan{};
    std::vector<PreparedUpdateArtifact> artifacts{};

    // Invariants:
    //  * NotRequired: no update-super task and no prepared_super_artifact.
    //  * SkippedNotFound: AOSP-compatible no-op; update-super tasks were
    //    removed before execution and no prepared_super_artifact exists.
    //  * Prepared: update-super tasks remain and prepared_super_artifact is a
    //    fully materialized, validated super_empty.img snapshot.
    UpdateSuperPreparationState update_super_state{
        UpdateSuperPreparationState::NotRequired};
    std::shared_ptr<const PreparedSuperArtifact> prepared_super_artifact{};

    // Requirements are intentionally not queried during transport-free
    // preflight. The execution layer must validate them against one uniquely
    // selected device before any destructive task.
    bool requires_device_validation{};
};

// Returns the de-duplicated partition column from the frozen Platform-Tools
// 37.0.1 host image table used by partition-exists requirements. The public
// update operation passes this exact table to the executor instead of
// guessing partitions from the connected device or the package contents.
[[nodiscard]] std::vector<std::string> frozen_update_known_partitions();

// Resolves android-info.txt and, when present, fastboot-info.txt. A missing
// fastboot-info.txt selects the frozen Platform-Tools 37.0.1 image inventory;
// a present empty file intentionally remains an empty plan. The function
// applies the wipe condition and materializes every referenced flash artifact,
// including super_empty.img when update-super needs it. It has no transport
// dependency and returns no partial plan on failure.
[[nodiscard]] std::expected<PreparedUpdatePackage, UpdatePackagePreflightError>
preflight_update_package(image::ArtifactSourceResolver& resolver,
                         const std::filesystem::path& package_directory_or_zip,
                         bool wants_wipe,
                         const UpdatePackagePreflightLimits& limits = {},
                         std::stop_token cancellation = {});

// Uses the same caller-owned absolute deadline for package inventory and every
// artifact materialization. Callers must pass this unchanged to the executor.
[[nodiscard]] std::expected<PreparedUpdatePackage, UpdatePackagePreflightError>
preflight_update_package(
    image::ArtifactSourceResolver& resolver,
    const std::filesystem::path& package_directory_or_zip,
    bool wants_wipe,
    const UpdatePackagePreflightLimits& limits,
    std::chrono::steady_clock::time_point deadline,
    std::stop_token cancellation = {});

// Materializes one explicit super_empty image into the same immutable
// update-super transaction used by package updates. The returned plan contains
// exactly one wipe-enabled UpdateSuper task and performs no transport I/O.
[[nodiscard]] std::expected<PreparedUpdatePackage, UpdatePackagePreflightError>
preflight_wipe_super(image::ArtifactSourceResolver& resolver,
                     const std::filesystem::path& super_empty_image,
                     std::stop_token cancellation = {});

}  // namespace kairosboot::fastboot
