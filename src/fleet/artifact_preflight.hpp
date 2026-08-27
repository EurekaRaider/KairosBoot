// SPDX-License-Identifier: MIT
#pragma once

#include "src/fleet/job_plan.hpp"
#include "src/image/artifact_source.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fleet {

using ArtifactPreflightClock = std::chrono::steady_clock;
using ArtifactPreflightTimePoint = ArtifactPreflightClock::time_point;

enum class ArtifactPreflightErrorKind : std::uint8_t {
    InvalidArgument,
    InvalidPlan,
    DuplicateArtifactId,
    ConflictingDeclaredDigest,
    NotFound,
    UnsafePath,
    LimitExceeded,
    Integrity,
    HashMismatch,
    InvalidImage,
    Io,
    Cancelled,
    TimedOut,
    ResourceExhausted,
    UnexpectedFailure,
};

struct ArtifactPreflightError final {
    ArtifactPreflightErrorKind kind{ArtifactPreflightErrorKind::UnexpectedFailure};
    std::size_t artifact_index{};
    std::string artifact_id;
    // Absolute, lexically normalized Host path when path construction succeeded.
    std::filesystem::path path;
    image::ArtifactSourceErrorKind source_kind{image::ArtifactSourceErrorKind::Io};
    int native_code{};
    std::string message;
};

enum class ArtifactPreflightFaultPoint : std::uint8_t {
    BeforeArtifactResolve,
    BeforePublish,
};

using ArtifactPreflightFaultHook =
    void (*)(ArtifactPreflightFaultPoint, std::size_t, void*);

struct ArtifactPreflightOptions final {
    ArtifactPreflightTimePoint deadline{ArtifactPreflightTimePoint::max()};
    std::stop_token cancellation;
    // max_spool_bytes is the immutable batch byte budget. The remaining source
    // limits retain their ArtifactSourceResolver meanings.
    image::ArtifactSourceLimits source_limits{};
    // Internal deterministic exception/observation seam. Production leaves
    // both fields null.
    ArtifactPreflightFaultHook fault_hook{};
    void* fault_context{};
};

class PreparedArtifactSource final {
public:
    PreparedArtifactSource(const PreparedArtifactSource&) = delete;
    PreparedArtifactSource& operator=(const PreparedArtifactSource&) = delete;
    PreparedArtifactSource(PreparedArtifactSource&&) = delete;
    PreparedArtifactSource& operator=(PreparedArtifactSource&&) = delete;
    ~PreparedArtifactSource() = default;

    [[nodiscard]] const std::shared_ptr<const image::ResolvedArtifact>& resolved()
        const noexcept;
    [[nodiscard]] const image::FlashArtifact& flash_artifact() const noexcept;
    [[nodiscard]] std::string_view sha256_hex() const noexcept;

private:
    PreparedArtifactSource(
        std::shared_ptr<const image::ResolvedArtifact> resolved,
        image::FlashArtifact&& flash_artifact,
        std::string&& sha256_hex) noexcept;

    std::shared_ptr<const image::ResolvedArtifact> resolved_;
    image::FlashArtifact flash_artifact_;
    std::string sha256_hex_;

    friend class PreparedFleetArtifacts;
    friend std::expected<class PreparedFleetArtifacts, ArtifactPreflightError>
    preflight_fleet_artifacts(const JobPlan&,
                              const std::filesystem::path&,
                              const ArtifactPreflightOptions&) noexcept;
};

class PreparedFleetArtifact final {
public:
    PreparedFleetArtifact(const PreparedFleetArtifact&) = delete;
    PreparedFleetArtifact& operator=(const PreparedFleetArtifact&) = delete;
    PreparedFleetArtifact(PreparedFleetArtifact&&) noexcept = default;
    PreparedFleetArtifact& operator=(PreparedFleetArtifact&&) noexcept = default;
    ~PreparedFleetArtifact() = default;

    [[nodiscard]] std::size_t index() const noexcept;
    [[nodiscard]] std::string_view id() const noexcept;
    [[nodiscard]] std::string_view declared_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& source_path() const noexcept;
    [[nodiscard]] std::string_view declared_sha256() const noexcept;
    [[nodiscard]] const std::shared_ptr<const PreparedArtifactSource>& source()
        const noexcept;

private:
    PreparedFleetArtifact(
        std::size_t index,
        std::string&& id,
        std::string&& declared_path,
        std::filesystem::path&& source_path,
        std::string&& declared_sha256,
        std::shared_ptr<const PreparedArtifactSource> source) noexcept;

    std::size_t index_{};
    std::string id_;
    std::string declared_path_;
    std::filesystem::path source_path_;
    std::string declared_sha256_;
    std::shared_ptr<const PreparedArtifactSource> source_;

    friend class PreparedFleetArtifacts;
    friend std::expected<class PreparedFleetArtifacts, ArtifactPreflightError>
    preflight_fleet_artifacts(const JobPlan&,
                              const std::filesystem::path&,
                              const ArtifactPreflightOptions&) noexcept;
};

// Capability token proving that the complete JobPlan artifact set was resolved,
// hashed, inspected, and atomically published. It cannot be default-constructed
// or forged from individual sources. The referenced immutable JobPlan must
// outlive the token and any execution that consumes it.
class PreparedFleetArtifacts final {
public:
    PreparedFleetArtifacts(const PreparedFleetArtifacts&) = delete;
    PreparedFleetArtifacts& operator=(const PreparedFleetArtifacts&) = delete;
    PreparedFleetArtifacts(PreparedFleetArtifacts&&) noexcept = default;
    PreparedFleetArtifacts& operator=(PreparedFleetArtifacts&&) noexcept = default;
    ~PreparedFleetArtifacts() = default;

    [[nodiscard]] const JobPlan& plan() const noexcept;
    [[nodiscard]] const std::filesystem::path& artifact_root() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const PreparedFleetArtifact& at(std::size_t index) const;
    [[nodiscard]] const PreparedFleetArtifact* find(std::string_view id) const noexcept;

private:
    PreparedFleetArtifacts(const JobPlan& plan,
                           std::filesystem::path&& artifact_root,
                           std::vector<PreparedFleetArtifact>&& artifacts) noexcept;

    const JobPlan* plan_{};
    std::filesystem::path artifact_root_;
    std::vector<PreparedFleetArtifact> artifacts_;

    friend std::expected<PreparedFleetArtifacts, ArtifactPreflightError>
    preflight_fleet_artifacts(const JobPlan&,
                              const std::filesystem::path&,
                              const ArtifactPreflightOptions&) noexcept;
};

// Performs no device enumeration, transport construction, or session access.
// Every source is a sealed private spool whose SHA-256 was accumulated from the
// same bytes during materialization. No result is published until the entire
// ordered artifact set succeeds.
[[nodiscard]] std::expected<PreparedFleetArtifacts, ArtifactPreflightError>
preflight_fleet_artifacts(
    const JobPlan& plan,
    const std::filesystem::path& artifact_root,
    const ArtifactPreflightOptions& options = {}) noexcept;

}  // namespace kairosboot::fleet
