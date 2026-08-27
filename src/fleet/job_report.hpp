// SPDX-License-Identifier: MIT
#pragma once

#include "src/image/sha256.hpp"

#include <kairosboot/kairosboot.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kairosboot::fleet {

enum class ReportOperation : std::uint8_t {
    Flash,
    Erase,
    SetActive,
    Reboot,
    Oem,
};

enum class ReportSlot : std::uint8_t {
    Current,
    Other,
    All,
    A,
    B,
};

enum class ReportRebootTarget : std::uint8_t {
    System,
    Bootloader,
    Recovery,
    Fastboot,
};

enum class ReportState : std::uint8_t {
    Running,
    Succeeded,
    PartiallyFailed,
    Failed,
    Cancelled,
};

enum class ReportWorkState : std::uint8_t {
    Pending,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    Skipped,
};

// The frozen JSON schema deliberately carries no skippedReason field. This
// typed value is retained only by the immutable in-process snapshot so every
// state-machine transition still records why work was not attempted.
enum class ReportSkipReason : std::uint8_t {
    FollowingStepFailure,
    FollowingStepCancellation,
    ProductMismatch,
    DevicePreflightFailure,
    PolicyStopped,
    JobPreflightFailure,
    JobCancellation,
};

struct ReportStepSpec final {
    ReportOperation operation{ReportOperation::Flash};
    std::optional<std::string> partition;
    std::optional<std::string> artifact;
    std::optional<ReportSlot> slot;
    std::optional<ReportRebootTarget> reboot_target;
    std::optional<std::string> oem_command;
    // Required only for Flash. It is the aggregate Fastboot DATA byte count,
    // not the logical partition size.
    std::optional<std::uint64_t> bytes_total;

    [[nodiscard]] bool operator==(const ReportStepSpec&) const = default;
};

// Adapter seam for the future JobPlan integration. The caller supplies only
// the already-resolved target binding and immutable plan step identity; this
// layer intentionally does not duplicate selector or manifest normalization.
struct ReportDeviceSpec final {
    std::string identifier;
    std::optional<std::string> serial;
    std::optional<std::string> usb_path;
    std::string target;
    std::string expected_product;
    // May be absent while device product preflight is still in progress. No
    // snapshot or destructive transition is publishable until it is verified
    // or the device is failed at preflight scope.
    std::optional<std::string> observed_product;
    std::vector<ReportStepSpec> steps;

    [[nodiscard]] bool operator==(const ReportDeviceSpec&) const = default;
};

struct ReportError final {
    kb_status_t code{KB_E_INTERNAL};
    std::string message;
    std::optional<std::string> device_identifier;
    std::optional<std::int32_t> native_code;
    std::optional<kb_transfer_state_t> transfer_certainty;

    [[nodiscard]] bool operator==(const ReportError&) const = default;
};

struct ReportStepSnapshot final {
    std::size_t index{};
    ReportStepSpec spec;
    ReportWorkState state{ReportWorkState::Pending};
    std::optional<std::string> started_at;
    std::optional<std::string> finished_at;
    std::optional<std::uint64_t> bytes_transferred;
    std::optional<ReportError> error;
    std::optional<ReportSkipReason> skip_reason;

    [[nodiscard]] bool operator==(const ReportStepSnapshot&) const = default;
};

struct ReportDeviceSnapshot final {
    std::string identifier;
    std::optional<std::string> serial;
    std::optional<std::string> usb_path;
    std::string target;
    std::string expected_product;
    std::optional<std::string> observed_product;
    ReportWorkState state{ReportWorkState::Pending};
    std::vector<ReportStepSnapshot> steps;
    std::optional<ReportError> error;

    [[nodiscard]] bool operator==(const ReportDeviceSnapshot&) const = default;
};

struct ReportSummary final {
    std::uint64_t total{};
    std::uint64_t pending{};
    std::uint64_t running{};
    std::uint64_t succeeded{};
    std::uint64_t failed{};
    std::uint64_t cancelled{};
    std::uint64_t skipped{};

    [[nodiscard]] bool operator==(const ReportSummary&) const = default;
};

enum class JobReportErrorKind : std::uint8_t {
    InvalidArgument,
    InvalidUtf8,
    InvalidTimestamp,
    TimestampOrder,
    IntegerOutOfRange,
    InvalidTransition,
    CancellationLatched,
    AlreadyTerminal,
    NotTerminal,
    UnexpectedFailure,
};

struct JobReportError final {
    JobReportErrorKind kind{JobReportErrorKind::UnexpectedFailure};
    std::string message;

    [[nodiscard]] bool operator==(const JobReportError&) const = default;
};

enum class JobReportFaultPoint : std::uint8_t {
    BeforeCommit,
    BeforeSnapshotPublish,
};

using JobReportFaultHook = void (*)(JobReportFaultPoint, void*);

struct JobReportBuilderOptions final {
    // Deterministic exception/OOM seam for internal tests. Production leaves
    // both fields null. A throwing hook must leave the prior state observable.
    JobReportFaultHook fault_hook{};
    void* fault_context{};
};

class JobReport final {
public:
    ~JobReport();
    JobReport(JobReport&&) noexcept;
    JobReport& operator=(JobReport&&) noexcept;

    JobReport(const JobReport&) = delete;
    JobReport& operator=(const JobReport&) = delete;

    [[nodiscard]] std::string_view job_id() const noexcept;
    [[nodiscard]] std::string_view plan_sha256() const noexcept;
    [[nodiscard]] ReportState state() const noexcept;
    [[nodiscard]] std::string_view started_at() const noexcept;
    [[nodiscard]] const std::optional<std::string>& finished_at() const noexcept;
    [[nodiscard]] const std::vector<ReportDeviceSnapshot>& devices() const noexcept;
    [[nodiscard]] const ReportSummary& summary() const noexcept;
    [[nodiscard]] const std::optional<ReportError>& error() const noexcept;
    [[nodiscard]] std::string_view canonical_json() const noexcept;

private:
    struct Implementation;
    explicit JobReport(std::unique_ptr<Implementation> implementation) noexcept;

    friend class JobReportBuilder;
    std::unique_ptr<Implementation> implementation_;
};

class JobReportBuilder final {
public:
    ~JobReportBuilder();
    JobReportBuilder(JobReportBuilder&&) noexcept;
    JobReportBuilder& operator=(JobReportBuilder&&) noexcept;

    JobReportBuilder(const JobReportBuilder&) = delete;
    JobReportBuilder& operator=(const JobReportBuilder&) = delete;

    [[nodiscard]] static std::expected<JobReportBuilder, JobReportError> create(
        std::string job_id,
        const image::Sha256Digest& plan_sha256,
        std::string started_at,
        std::vector<ReportDeviceSpec> devices,
        const JobReportBuilderOptions& options = {});

    // Product verification is separate from protocol execution. A mismatch
    // must use fail_product_preflight(), which guarantees an all-skipped device.
    [[nodiscard]] std::expected<void, JobReportError> verify_product(
        std::size_t device_index,
        std::string observed_product);

    [[nodiscard]] std::expected<void, JobReportError> begin_first_step(
        std::size_t device_index,
        std::string started_at);
    [[nodiscard]] std::expected<void, JobReportError> update_flash_progress(
        std::size_t device_index,
        std::size_t step_index,
        std::uint64_t bytes_transferred);
    [[nodiscard]] std::expected<void, JobReportError> advance_step(
        std::size_t device_index,
        std::size_t completed_step_index,
        std::string completed_at,
        std::string next_started_at);
    [[nodiscard]] std::expected<void, JobReportError> complete_device(
        std::size_t device_index,
        std::size_t final_step_index,
        std::string finished_at);
    [[nodiscard]] std::expected<void, JobReportError> fail_step(
        std::size_t device_index,
        std::size_t step_index,
        std::string finished_at,
        ReportError error,
        ReportSkipReason suffix_reason);
    [[nodiscard]] std::expected<void, JobReportError> fail_device_preflight(
        std::size_t device_index,
        std::optional<std::string> observed_product,
        std::string finished_at,
        ReportError error,
        ReportSkipReason reason = ReportSkipReason::DevicePreflightFailure);
    [[nodiscard]] std::expected<void, JobReportError> fail_product_preflight(
        std::size_t device_index,
        std::optional<std::string> observed_product,
        std::string finished_at,
        ReportError error);
    [[nodiscard]] std::expected<void, JobReportError> skip_pending_device(
        std::size_t device_index,
        std::string finished_at,
        ReportSkipReason reason);

    // Once latched, non-cancellation transitions and normal publication fail
    // closed. finish_cancelled() may be called after transport drain and wins
    // over any device successes or failures committed before the latch.
    [[nodiscard]] std::expected<void, JobReportError> request_cancellation(
        ReportError error);
    [[nodiscard]] std::expected<void, JobReportError> finish_cancelled(
        std::string finished_at,
        ReportSkipReason suffix_reason = ReportSkipReason::JobCancellation);

    // Normal terminal derivation is based only on device states. A job-level
    // preflight/integrity failure instead uses finish_failed().
    [[nodiscard]] std::expected<void, JobReportError> finish(
        std::string finished_at);
    [[nodiscard]] std::expected<void, JobReportError> finish_failed(
        std::string finished_at,
        ReportError error,
        ReportSkipReason pending_reason =
            ReportSkipReason::JobPreflightFailure);

    [[nodiscard]] std::expected<JobReport, JobReportError> running_snapshot()
        const;
    [[nodiscard]] std::expected<JobReport, JobReportError> terminal_snapshot()
        const;

private:
    struct Implementation;
    explicit JobReportBuilder(std::unique_ptr<Implementation> implementation)
        noexcept;

    std::unique_ptr<Implementation> implementation_;
};

}  // namespace kairosboot::fleet
