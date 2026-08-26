// SPDX-License-Identifier: MIT
#include "update_package_preflight.hpp"

#include <cstddef>
#include <limits>
#include <new>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace kairosboot::fastboot {
namespace {

inline constexpr std::string_view kAndroidInfoName{"android-info.txt"};
inline constexpr std::string_view kFastbootInfoName{"fastboot-info.txt"};

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

}  // namespace

std::expected<PreparedUpdatePackage, UpdatePackagePreflightError>
preflight_update_package(image::ArtifactSourceResolver& resolver,
                         const std::filesystem::path& package_directory_or_zip,
                         const bool wants_wipe,
                         const UpdatePackagePreflightLimits& limits,
                         const std::stop_token cancellation) {
    try {
        if (cancellation.stop_requested()) {
            return std::unexpected(failure(UpdatePackagePreflightErrorKind::Cancelled,
                                           "update package preflight was cancelled"));
        }

        auto android =
            resolver.resolve(package_directory_or_zip, kAndroidInfoName, cancellation);
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
        auto fastboot =
            resolver.resolve(package_directory_or_zip, kFastbootInfoName, cancellation);
        if (fastboot) {
            auto contents =
                read_manifest(**fastboot, kFastbootInfoName,
                              limits.manifest.maximum_file_bytes, cancellation);
            if (!contents) {
                return std::unexpected(std::move(contents.error()));
            }
            fastboot_text = std::move(*contents);
        } else if (fastboot.error().kind != image::ArtifactSourceErrorKind::NotFound) {
            return std::unexpected(
                artifact_failure(kFastbootInfoName, std::move(fastboot.error())));
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
        auto plan = make_update_plan(*parsed, wants_wipe);

        std::vector<std::string> artifact_names;
        artifact_names.reserve(plan.tasks.size());
        std::set<std::string> unique_names;
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
            unique_names.insert(task.artifact);
            artifact_names.push_back(task.artifact);
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
            auto preflight = image::preflight_flash_artifact(
                resolver, package_directory_or_zip, name, cancellation);
            if (!preflight) {
                return std::unexpected(
                    artifact_failure(name, std::move(preflight.error())));
            }
            const auto size = preflight->resolved->source->size();
            if (total_bytes > limits.maximum_total_artifact_bytes ||
                size > limits.maximum_total_artifact_bytes - total_bytes) {
                return std::unexpected(failure(
                    UpdatePackagePreflightErrorKind::LimitExceeded,
                    "update plan artifacts exceed the configured aggregate byte limit",
                    name));
            }
            total_bytes += size;
            artifacts.push_back(PreparedUpdateArtifact{
                .name = name,
                .resolved = std::move(preflight->resolved),
                .artifact = std::move(preflight->artifact),
            });
        }

        return PreparedUpdatePackage{
            .plan = std::move(plan),
            .artifacts = std::move(artifacts),
            .requires_device_validation = !parsed->requirements.empty(),
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            failure(UpdatePackagePreflightErrorKind::LimitExceeded,
                    "memory allocation failed during update package preflight"));
    }
}

}  // namespace kairosboot::fastboot
