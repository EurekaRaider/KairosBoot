// SPDX-License-Identifier: MIT
#include "update_package_preflight.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace kairosboot::fastboot {
namespace {

inline constexpr std::string_view kAndroidInfoName{"android-info.txt"};
inline constexpr std::string_view kFastbootInfoName{"fastboot-info.txt"};
inline constexpr std::string_view kSuperEmptyName{"super_empty.img"};

enum class HardcodedImageType : std::uint8_t {
    BootCritical,
    Normal,
    Extra,
};

struct HardcodedImage final {
    std::string_view nickname;
    std::string_view image_name;
    std::string_view partition;
    bool optional_if_missing;
    HardcodedImageType type;
};

// Frozen Platform-Tools 37.0.1 evidence:
//   commit a3b721a32242006b59cb12bd62c9133632af3a2d
//   blob   1c52da2382ad46759485c2ff096dd660a9c3c999
//   https://android.googlesource.com/platform/system/core/+/a3b721a32242006b59cb12bd62c9133632af3a2d/fastboot/fastboot.cpp
// AOSP selects BootCritical, then UpdateSuper, then Normal. Extra entries are
// excluded from flashall; boot.img and system.img are the only required files.
// The independent test fixture intentionally duplicates this oracle so a
// production-table edit cannot silently update its own expected values.
inline constexpr std::array kHardcodedImages{
    HardcodedImage{"boot", "boot.img", "boot", false,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"bootloader", "bootloader.img", "bootloader", true,
                   HardcodedImageType::Extra},
    HardcodedImage{"init_boot", "init_boot.img", "init_boot", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"", "boot_other.img", "boot", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"cache", "cache.img", "cache", true,
                   HardcodedImageType::Extra},
    HardcodedImage{"dtbo", "dtbo.img", "dtbo", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"dts", "dt.img", "dts", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"odm", "odm.img", "odm", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"odm_dlkm", "odm_dlkm.img", "odm_dlkm", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"product", "product.img", "product", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"pvmfw", "pvmfw.img", "pvmfw", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"radio", "radio.img", "radio", true,
                   HardcodedImageType::Extra},
    HardcodedImage{"recovery", "recovery.img", "recovery", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"super", "super.img", "super", true,
                   HardcodedImageType::Extra},
    HardcodedImage{"system", "system.img", "system", false,
                   HardcodedImageType::Normal},
    HardcodedImage{"system_dlkm", "system_dlkm.img", "system_dlkm", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"system_ext", "system_ext.img", "system_ext", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"", "system_other.img", "system", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"userdata", "userdata.img", "userdata", true,
                   HardcodedImageType::Extra},
    HardcodedImage{"vbmeta", "vbmeta.img", "vbmeta", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"vbmeta_system", "vbmeta_system.img", "vbmeta_system", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"vbmeta_vendor", "vbmeta_vendor.img", "vbmeta_vendor", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"vendor", "vendor.img", "vendor", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"vendor_boot", "vendor_boot.img", "vendor_boot", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"vendor_dlkm", "vendor_dlkm.img", "vendor_dlkm", true,
                   HardcodedImageType::Normal},
    HardcodedImage{"vendor_kernel_boot", "vendor_kernel_boot.img",
                   "vendor_kernel_boot", true,
                   HardcodedImageType::BootCritical},
    HardcodedImage{"", "vendor_other.img", "vendor", true,
                   HardcodedImageType::Normal},
};

[[nodiscard]] UpdatePackagePreflightError
failure(const UpdatePackagePreflightErrorKind kind, std::string message,
        std::string artifact = {}) {
    return {
        .kind = kind,
        .message = std::move(message),
        .artifact = std::move(artifact),
    };
}

[[nodiscard]] UpdatePackagePreflightError
artifact_failure(const std::string_view name, image::ArtifactSourceError error) {
    const auto kind = error.kind == image::ArtifactSourceErrorKind::Cancelled
                          ? UpdatePackagePreflightErrorKind::Cancelled
                          : UpdatePackagePreflightErrorKind::Artifact;
    auto result = failure(kind,
                          "unable to resolve update package entry " +
                              std::string(name) + ": " + error.message,
                          std::string(name));
    result.artifact_error = std::move(error);
    return result;
}

[[nodiscard]] std::expected<std::string, UpdatePackagePreflightError>
read_manifest(const image::ResolvedArtifact& resolved, const std::string_view name,
              const std::size_t maximum_size, const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        return std::unexpected(failure(UpdatePackagePreflightErrorKind::Cancelled,
                                       "update package preflight was cancelled",
                                       std::string(name)));
    }
    const auto source_size = resolved.source->size();
    if (source_size > maximum_size || !std::in_range<std::size_t>(source_size)) {
        return std::unexpected(
            failure(UpdatePackagePreflightErrorKind::LimitExceeded,
                    std::string(name) + " exceeds the configured manifest byte limit",
                    std::string(name)));
    }

    std::string contents(static_cast<std::size_t>(source_size), '\0');
    std::size_t completed = 0;
    while (completed < contents.size()) {
        if (cancellation.stop_requested()) {
            return std::unexpected(failure(UpdatePackagePreflightErrorKind::Cancelled,
                                           "update package preflight was cancelled",
                                           std::string(name)));
        }
        auto destination =
            std::span(reinterpret_cast<std::byte*>(contents.data()), contents.size());
        auto read = resolved.source->read_at(completed, destination.subspan(completed));
        if (!read) {
            return std::unexpected(failure(
                UpdatePackagePreflightErrorKind::ManifestRead,
                "unable to read " + std::string(name) + ": " + read.error().message,
                std::string(name)));
        }
        if (*read == 0U || *read > contents.size() - completed) {
            return std::unexpected(failure(
                UpdatePackagePreflightErrorKind::ManifestRead,
                std::string(name) + " was truncated or returned an invalid byte count",
                std::string(name)));
        }
        completed += *read;
    }
    return contents;
}

[[nodiscard]] bool task_references_artifact(const PlannedUpdateTask& task) noexcept {
    return task.kind == UpdateTaskKind::Flash;
}

[[nodiscard]] bool is_vbmeta_partition(
    const std::string_view partition) noexcept {
    return partition == "vbmeta" || partition == "vbmeta_system" ||
           partition == "vbmeta_vendor";
}

[[nodiscard]] UpdateSourceLocation hardcoded_location(
    const std::size_t inventory_index) noexcept {
    return {
        .manifest = UpdateManifestKind::FastbootInfo,
        .line = inventory_index + 1U,
        .column = 1U,
        .byte_offset = inventory_index,
    };
}

[[nodiscard]] std::expected<DeterministicUpdatePlan,
                            UpdatePackagePreflightError>
make_hardcoded_update_plan(
    image::ArtifactPackageSnapshot& snapshot,
    std::vector<PlannedRequirement> requirements,
    const std::stop_token cancellation) {
    DeterministicUpdatePlan plan{.requirements = std::move(requirements)};

    const auto add_phase = [&](const HardcodedImageType phase)
        -> std::expected<void, UpdatePackagePreflightError> {
        for (std::size_t index = 0; index < kHardcodedImages.size(); ++index) {
            const auto& specification = kHardcodedImages[index];
            if (specification.type != phase) {
                continue;
            }
            if (cancellation.stop_requested()) {
                return std::unexpected(failure(
                    UpdatePackagePreflightErrorKind::Cancelled,
                    "update package preflight was cancelled",
                    std::string(specification.image_name)));
            }

            auto resolved = snapshot.resolve(specification.image_name);
            if (!resolved) {
                if (specification.optional_if_missing &&
                    resolved.error().kind ==
                        image::ArtifactSourceErrorKind::NotFound) {
                    continue;
                }
                return std::unexpected(artifact_failure(
                    specification.image_name, std::move(resolved.error())));
            }
            plan.tasks.push_back(PlannedUpdateTask{
                .kind = UpdateTaskKind::Flash,
                .conditional_on_wipe = false,
                .location = hardcoded_location(index),
                .partition = std::string(specification.partition),
                .artifact = std::string(specification.image_name),
                .slot = specification.nickname.empty() ? PlannedSlot::Other
                                                       : PlannedSlot::Default,
                .apply_vbmeta = is_vbmeta_partition(specification.partition),
            });
        }
        return {};
    };

    if (auto boot = add_phase(HardcodedImageType::BootCritical); !boot) {
        return std::unexpected(std::move(boot.error()));
    }
    plan.tasks.push_back(PlannedUpdateTask{
        .kind = UpdateTaskKind::UpdateSuper,
        .conditional_on_wipe = false,
        .location = hardcoded_location(kHardcodedImages.size()),
    });
    if (auto normal = add_phase(HardcodedImageType::Normal); !normal) {
        return std::unexpected(std::move(normal.error()));
    }
    return plan;
}

[[nodiscard]] bool super_metadata_is_consistent(
    const image::ResolvedArtifact& resolved,
    const image::FlashArtifact& artifact) noexcept {
    if (!resolved.source || resolved.logical_name != kSuperEmptyName ||
        artifact.transfer_source() != resolved.source ||
        artifact.metadata().transfer_size != resolved.source->size()) {
        return false;
    }

    const auto& metadata = artifact.metadata();
    switch (metadata.kind) {
    case image::FlashArtifactKind::Raw:
        return artifact.sparse_image() == nullptr &&
               !metadata.sparse_header.has_value() &&
               metadata.expanded_size == metadata.transfer_size;
    case image::FlashArtifactKind::AndroidSparse:
        return artifact.sparse_image() != nullptr &&
               metadata.sparse_header.has_value() &&
               metadata.expanded_size == artifact.sparse_image()->output_size();
    default:
        return false;
    }
}

struct PreparedUpdateSuper final {
    UpdateSuperPreparationState state{UpdateSuperPreparationState::NotRequired};
    std::shared_ptr<const PreparedSuperArtifact> artifact{};
};

[[nodiscard]] bool plan_requires_update_super(
    const DeterministicUpdatePlan& plan) noexcept {
    return std::ranges::any_of(plan.tasks, [](const PlannedUpdateTask& task) {
        return task.kind == UpdateTaskKind::UpdateSuper;
    });
}

[[nodiscard]] std::expected<PreparedUpdateSuper, UpdatePackagePreflightError>
prepare_update_super(
    image::ArtifactPackageSnapshot& snapshot,
    DeterministicUpdatePlan* plan,
    const std::stop_token cancellation) {
    if (!plan_requires_update_super(*plan)) {
        return PreparedUpdateSuper{};
    }

    if (cancellation.stop_requested()) {
        return std::unexpected(failure(
            UpdatePackagePreflightErrorKind::Cancelled,
            "update package preflight was cancelled",
            std::string(kSuperEmptyName)));
    }
    auto preflight = image::preflight_flash_artifact(snapshot, kSuperEmptyName);
    if (!preflight) {
        if (preflight.error().kind == image::ArtifactSourceErrorKind::NotFound) {
            std::erase_if(plan->tasks, [](const PlannedUpdateTask& task) {
                return task.kind == UpdateTaskKind::UpdateSuper;
            });
            return PreparedUpdateSuper{
                .state = UpdateSuperPreparationState::SkippedNotFound,
            };
        }
        return std::unexpected(
            artifact_failure(kSuperEmptyName, std::move(preflight.error())));
    }
    if (!super_metadata_is_consistent(*preflight->resolved,
                                      preflight->artifact)) {
        return std::unexpected(artifact_failure(
            kSuperEmptyName,
            image::ArtifactSourceError{
                .kind = image::ArtifactSourceErrorKind::InvalidImage,
                .message =
                    "prepared super_empty.img metadata or source mapping is "
                    "inconsistent",
            }));
    }

    auto flash = std::make_shared<const image::FlashArtifact>(
        std::move(preflight->artifact));
    auto prepared = std::make_shared<const PreparedSuperArtifact>(
        std::move(preflight->resolved), std::move(flash));
    return PreparedUpdateSuper{
        .state = UpdateSuperPreparationState::Prepared,
        .artifact = std::move(prepared),
    };
}

[[nodiscard]] std::expected<void, UpdatePackagePreflightError>
add_to_aggregate_bytes(
    std::uint64_t* total_bytes,
    const std::uint64_t size,
    const UpdatePackagePreflightLimits& limits,
    const std::string_view name) {
    if (*total_bytes > limits.maximum_total_artifact_bytes ||
        size > limits.maximum_total_artifact_bytes - *total_bytes) {
        return std::unexpected(failure(
            UpdatePackagePreflightErrorKind::LimitExceeded,
            "update plan artifacts exceed the configured aggregate byte limit",
            std::string(name)));
    }
    *total_bytes += size;
    return {};
}

[[nodiscard]] std::string ascii_fold(std::string_view value) {
    std::string folded(value);
    for (auto& character : folded) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return folded;
}

}  // namespace

PreparedSuperArtifact::PreparedSuperArtifact(
    std::shared_ptr<const image::ResolvedArtifact> resolved,
    std::shared_ptr<const image::FlashArtifact> artifact) noexcept
    : resolved_(std::move(resolved)), artifact_(std::move(artifact)) {}

const std::shared_ptr<const image::ResolvedArtifact>&
PreparedSuperArtifact::resolved() const noexcept {
    return resolved_;
}

const std::shared_ptr<const image::FlashArtifact>&
PreparedSuperArtifact::artifact() const noexcept {
    return artifact_;
}

std::expected<PreparedUpdatePackage, UpdatePackagePreflightError>
preflight_update_package(image::ArtifactSourceResolver& resolver,
                         const std::filesystem::path& package_directory_or_zip,
                         const bool wants_wipe,
                         const UpdatePackagePreflightLimits& limits,
                         const std::stop_token cancellation) {
    return preflight_update_package(
        resolver, package_directory_or_zip, wants_wipe, limits,
        std::chrono::steady_clock::time_point::max(), cancellation);
}

std::expected<PreparedUpdatePackage, UpdatePackagePreflightError>
preflight_update_package(
    image::ArtifactSourceResolver& resolver,
    const std::filesystem::path& package_directory_or_zip,
    const bool wants_wipe,
    const UpdatePackagePreflightLimits& limits,
    const std::chrono::steady_clock::time_point deadline,
    const std::stop_token cancellation) {
    try {
        if (cancellation.stop_requested()) {
            return std::unexpected(failure(UpdatePackagePreflightErrorKind::Cancelled,
                                           "update package preflight was cancelled"));
        }

        auto opened_snapshot = resolver.open_package_snapshot(
            package_directory_or_zip, deadline, cancellation);
        if (!opened_snapshot) {
            return std::unexpected(artifact_failure(
                "update package", std::move(opened_snapshot.error())));
        }
        auto snapshot = std::move(*opened_snapshot);

        auto android = snapshot.resolve(kAndroidInfoName);
        if (!android) {
            if (android.error().kind == image::ArtifactSourceErrorKind::NotFound) {
                return std::unexpected(
                    failure(UpdatePackagePreflightErrorKind::MissingAndroidInfo,
                            "update package is missing required android-info.txt",
                            std::string(kAndroidInfoName)));
            }
            return std::unexpected(
                artifact_failure(kAndroidInfoName, std::move(android.error())));
        }
        auto android_text =
            read_manifest(**android, kAndroidInfoName,
                          limits.manifest.maximum_file_bytes, cancellation);
        if (!android_text) {
            return std::unexpected(std::move(android_text.error()));
        }

        std::string fastboot_text;
        bool use_hardcoded_fallback = false;
        auto fastboot = snapshot.resolve(kFastbootInfoName);
        if (fastboot) {
            auto contents =
                read_manifest(**fastboot, kFastbootInfoName,
                              limits.manifest.maximum_file_bytes, cancellation);
            if (!contents) {
                return std::unexpected(std::move(contents.error()));
            }
            fastboot_text = std::move(*contents);
        } else {
            if (fastboot.error().kind != image::ArtifactSourceErrorKind::NotFound) {
                return std::unexpected(
                    artifact_failure(kFastbootInfoName, std::move(fastboot.error())));
            }
            use_hardcoded_fallback = true;
        }

        auto parsed =
            parse_update_manifest(*android_text, fastboot_text, limits.manifest);
        if (!parsed) {
            auto error =
                failure(UpdatePackagePreflightErrorKind::Manifest,
                        "update manifest validation failed: " + parsed.error().message);
            error.manifest_error = std::move(parsed.error());
            return std::unexpected(std::move(error));
        }
        DeterministicUpdatePlan plan;
        if (use_hardcoded_fallback) {
            auto fallback = make_hardcoded_update_plan(
                snapshot, std::move(parsed->requirements), cancellation);
            if (!fallback) {
                return std::unexpected(std::move(fallback.error()));
            }
            plan = std::move(*fallback);
        } else {
            plan = make_update_plan(*parsed, wants_wipe);
        }

        auto prepared_super = prepare_update_super(
            snapshot, &plan, cancellation);
        if (!prepared_super) {
            return std::unexpected(std::move(prepared_super.error()));
        }
        if (use_hardcoded_fallback &&
            plan.tasks.size() > limits.manifest.maximum_tasks) {
            return std::unexpected(failure(
                UpdatePackagePreflightErrorKind::LimitExceeded,
                "hardcoded update plan exceeds the configured task limit"));
        }

        std::vector<std::string> artifact_names;
        artifact_names.reserve(plan.tasks.size());
        std::set<std::string, std::less<>> unique_names;
        std::map<std::string, std::string, std::less<>> folded_names;
        for (const auto& task : plan.tasks) {
            if (!task_references_artifact(task) ||
                unique_names.contains(task.artifact)) {
                continue;
            }
            if (artifact_names.size() >= limits.maximum_unique_artifacts) {
                return std::unexpected(failure(
                    UpdatePackagePreflightErrorKind::LimitExceeded,
                    "update plan exceeds the configured unique artifact limit"));
            }
            const auto folded = ascii_fold(task.artifact);
            if (const auto [position, inserted] =
                    folded_names.emplace(folded, task.artifact);
                !inserted) {
                auto error = failure(
                    UpdatePackagePreflightErrorKind::Artifact,
                    "update plan contains cross-platform case-folding artifact "
                    "aliases: " +
                        position->second + " and " + task.artifact,
                    task.artifact);
                error.artifact_error = image::ArtifactSourceError{
                    .kind = image::ArtifactSourceErrorKind::UnsafePath,
                    .message =
                        "artifact names collide on case-insensitive filesystems",
                };
                return std::unexpected(std::move(error));
            }
            unique_names.insert(task.artifact);
            artifact_names.push_back(task.artifact);
        }
        const bool super_is_also_a_flash_artifact =
            unique_names.contains(kSuperEmptyName);
        if (prepared_super->state == UpdateSuperPreparationState::Prepared &&
            !super_is_also_a_flash_artifact &&
            artifact_names.size() >= limits.maximum_unique_artifacts) {
            return std::unexpected(failure(
                UpdatePackagePreflightErrorKind::LimitExceeded,
                "update plan exceeds the configured unique artifact limit",
                std::string(kSuperEmptyName)));
        }

        std::vector<PreparedUpdateArtifact> artifacts;
        artifacts.reserve(artifact_names.size());
        std::uint64_t total_bytes = 0;
        for (const auto& name : artifact_names) {
            if (cancellation.stop_requested()) {
                return std::unexpected(
                    failure(UpdatePackagePreflightErrorKind::Cancelled,
                            "update package preflight was cancelled", name));
            }
            std::shared_ptr<const image::ResolvedArtifact> resolved;
            std::shared_ptr<const image::FlashArtifact> artifact;
            if (name == kSuperEmptyName && prepared_super->artifact) {
                // update-super already materialized and inspected this exact
                // package entry. Reuse both immutable objects for an ordinary
                // flash task instead of resolving or parsing sparse data twice.
                resolved = prepared_super->artifact->resolved();
                artifact = prepared_super->artifact->artifact();
            } else {
                auto preflight = image::preflight_flash_artifact(snapshot, name);
                if (!preflight) {
                    return std::unexpected(
                        artifact_failure(name, std::move(preflight.error())));
                }
                resolved = std::move(preflight->resolved);
                artifact = std::make_shared<const image::FlashArtifact>(
                    std::move(preflight->artifact));
            }
            if (!resolved || !resolved->source || !artifact ||
                artifact->transfer_source() != resolved->source) {
                return std::unexpected(artifact_failure(
                    name,
                    image::ArtifactSourceError{
                        .kind = image::ArtifactSourceErrorKind::InvalidImage,
                        .message =
                            "prepared flash artifact mapping is incomplete or "
                            "inconsistent",
                    }));
            }
            if (auto counted = add_to_aggregate_bytes(
                    &total_bytes, resolved->source->size(), limits, name);
                !counted) {
                return std::unexpected(std::move(counted.error()));
            }
            artifacts.push_back(PreparedUpdateArtifact{
                .name = name,
                .resolved = std::move(resolved),
                .artifact = std::move(artifact),
            });
        }

        if (prepared_super->artifact && !super_is_also_a_flash_artifact) {
            if (auto counted = add_to_aggregate_bytes(
                    &total_bytes,
                    prepared_super->artifact->resolved()->source->size(), limits,
                    kSuperEmptyName);
                !counted) {
                return std::unexpected(std::move(counted.error()));
            }
        }

        if (auto unchanged = snapshot.verify_unchanged(); !unchanged) {
            return std::unexpected(artifact_failure(
                "update package", std::move(unchanged.error())));
        }

        const bool requires_device_validation = !plan.requirements.empty();
        return PreparedUpdatePackage{
            .plan = std::move(plan),
            .artifacts = std::move(artifacts),
            .update_super_state = prepared_super->state,
            .prepared_super_artifact = std::move(prepared_super->artifact),
            .requires_device_validation = requires_device_validation,
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            failure(UpdatePackagePreflightErrorKind::LimitExceeded,
                    "memory allocation failed during update package preflight"));
    }
}

}  // namespace kairosboot::fastboot
