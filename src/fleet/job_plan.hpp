// SPDX-License-Identifier: MIT
#pragma once

#include "src/fleet/manifest.hpp"
#include "src/image/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace kairosboot::fleet {

enum class JobPlanErrorKind : std::uint8_t {
    InvalidManifest,
    InvalidUtf8,
    IntegerOutOfRange,
    ResourceExhausted,
    OutputTooLarge,
    UnexpectedFailure,
};

struct JobPlanError final {
    JobPlanErrorKind kind{JobPlanErrorKind::UnexpectedFailure};
    // InvalidUtf8 reports the offset within the rejected plan string.
    // Other errors do not have a byte offset and use zero.
    std::size_t input_byte_offset{};

    [[nodiscard]] bool operator==(const JobPlanError&) const = default;
};

enum class JobPlanFaultPoint : std::uint8_t {
    BeforeSerialization,
    BeforeSnapshotCommit,
};

using JobPlanFaultHook = void (*)(JobPlanFaultPoint, void*);

struct JobPlanBuildOptions final {
    // Deterministic exception seam for internal tests. Production leaves both
    // fields null; hooks are invoked before the input plan is consumed.
    JobPlanFaultHook fault_hook{};
    void* fault_context{};
};

class JobPlan final {
public:
    JobPlan(const JobPlan&) = delete;
    JobPlan& operator=(const JobPlan&) = delete;
    JobPlan(JobPlan&&) noexcept = default;
    JobPlan& operator=(JobPlan&&) noexcept = default;
    ~JobPlan() = default;

    [[nodiscard]] const FlashJobManifest& manifest() const noexcept;
    // Canonical UTF-8 JobPlan v1 JSON. SDK form never contains a trailing LF.
    [[nodiscard]] std::string_view canonical_json() const noexcept;
    [[nodiscard]] const image::Sha256Digest& sha256() const noexcept;
    [[nodiscard]] std::string_view sha256_hex() const noexcept;

private:
    JobPlan(FlashJobManifest&& manifest,
            std::string&& canonical_json,
            const image::Sha256Digest& sha256,
            std::string&& sha256_hex) noexcept;

    FlashJobManifest manifest_;
    std::string canonical_json_;
    image::Sha256Digest sha256_{};
    std::string sha256_hex_;

    friend std::expected<JobPlan, JobPlanError> make_job_plan(
        FlashJobManifest&&,
        const JobPlanBuildOptions&) noexcept;
};

// Planning defensively rejects unsafe encoding, shape, integer, and enum values
// that directly affect canonical serialization. The input is consumed only on
// success and no device enumeration or artifact access occurs. The returned
// snapshot owns the in-memory plan model, canonical JSON, and plan digest.
[[nodiscard]] std::expected<JobPlan, JobPlanError> make_job_plan(
    FlashJobManifest&& manifest,
    const JobPlanBuildOptions& options = {}) noexcept;

}  // namespace kairosboot::fleet
