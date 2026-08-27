// SPDX-License-Identifier: MIT
#include "src/fleet/job_report.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using kairosboot::fleet::JobReport;
using kairosboot::fleet::JobReportBuilder;
using kairosboot::fleet::JobReportBuilderOptions;
using kairosboot::fleet::JobReportErrorKind;
using kairosboot::fleet::JobReportFaultPoint;
using kairosboot::fleet::ReportDeviceSpec;
using kairosboot::fleet::ReportError;
using kairosboot::fleet::ReportOperation;
using kairosboot::fleet::ReportRebootTarget;
using kairosboot::fleet::ReportSkipReason;
using kairosboot::fleet::ReportSlot;
using kairosboot::fleet::ReportState;
using kairosboot::fleet::ReportStepSpec;
using kairosboot::fleet::ReportWorkState;
using kairosboot::image::Sha256Digest;

static_assert(!std::is_copy_constructible_v<JobReport>);
static_assert(std::is_nothrow_move_constructible_v<JobReport>);
static_assert(!std::is_copy_constructible_v<JobReportBuilder>);
static_assert(std::is_nothrow_move_constructible_v<JobReportBuilder>);

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw CheckFailure(std::string("check failed at line ") +          \
                               std::to_string(__LINE__) + ": " #condition);     \
        }                                                                       \
    } while (false)

[[nodiscard]] unsigned int hex_nibble(const char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return 10U + static_cast<unsigned int>(value - 'a');
    }
    throw CheckFailure{"invalid test SHA-256"};
}

[[nodiscard]] Sha256Digest digest(const std::string_view text) {
    CHECK(text.size() == 64U);
    Sha256Digest result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(
            (hex_nibble(text[index * 2U]) << 4U) |
            hex_nibble(text[index * 2U + 1U]));
    }
    return result;
}

[[nodiscard]] const Sha256Digest& plan_digest() {
    static const auto value = digest(
        "992daa21b5ea246910fc5d9ffffafed3e36e883d6a407b70abe3b04def3823f4");
    return value;
}

[[nodiscard]] ReportStepSpec flash_step(const std::uint64_t bytes = 4096U) {
    return {
        .operation = ReportOperation::Flash,
        .partition = "system",
        .artifact = "system",
        .slot = std::nullopt,
        .reboot_target = std::nullopt,
        .oem_command = std::nullopt,
        .bytes_total = bytes,
    };
}

[[nodiscard]] ReportDeviceSpec device(std::string identifier,
                                      std::string serial,
                                      std::vector<ReportStepSpec> steps = {
                                          flash_step()}) {
    return {
        .identifier = std::move(identifier),
        .serial = std::move(serial),
        .usb_path = std::nullopt,
        .target = "product-a",
        .expected_product = "product_a",
        .observed_product = "product_a",
        .steps = std::move(steps),
    };
}

[[nodiscard]] ReportError device_failure(std::string message =
                                             "device rejected flash command") {
    return {
        .code = KB_E_DEVICE_FAIL,
        .message = std::move(message),
        .device_identifier = std::nullopt,
        .native_code = std::nullopt,
        .transfer_certainty = KB_TRANSFER_FULLY_TRANSFERRED,
    };
}

[[nodiscard]] ReportError cancellation() {
    return {
        .code = KB_E_CANCELLED,
        .message = "operation cancelled",
        .device_identifier = std::nullopt,
        .native_code = std::nullopt,
        .transfer_certainty = KB_TRANSFER_PARTIAL_OR_UNKNOWN,
    };
}

[[nodiscard]] ReportError preflight_failure() {
    return {
        .code = KB_E_INVALID_ARGUMENT,
        .message = "artifact hash mismatch",
        .device_identifier = std::nullopt,
        .native_code = std::nullopt,
        .transfer_certainty = KB_TRANSFER_NOT_SENT,
    };
}

[[nodiscard]] JobReportBuilder make_builder(
    std::vector<ReportDeviceSpec> devices,
    const JobReportBuilderOptions& options = {}) {
    auto result = JobReportBuilder::create("job-0001",
                                           plan_digest(),
                                           "2026-08-27T06:00:00Z",
                                           std::move(devices),
                                           options);
    CHECK(result);
    return std::move(*result);
}

[[nodiscard]] std::string read_golden(const std::string_view name) {
    const auto path = std::filesystem::path{__FILE__}.parent_path().parent_path() /
                      "contracts" / name;
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    std::string result{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
    CHECK(!input.bad());
    CHECK(result.ends_with('\n'));
    result.pop_back();
    return result;
}

void exact_golden_and_partial_failure() {
    auto first = device("usb:1-2", "SERIAL-01");
    first.usb_path = "usb:1-2";
    auto second = device("usb:1-3", "SERIAL-02");
    second.usb_path = "usb:1-3";
    auto builder = make_builder({std::move(first), std::move(second)});

    CHECK(builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    CHECK(builder.update_flash_progress(0U, 0U, 4096U));
    CHECK(builder.complete_device(0U, 0U, "2026-08-27T06:00:02Z"));
    CHECK(builder.begin_first_step(1U, "2026-08-27T06:00:01Z"));
    CHECK(builder.update_flash_progress(1U, 0U, 4096U));
    CHECK(builder.fail_step(1U,
                            0U,
                            "2026-08-27T06:00:03Z",
                            device_failure(),
                            ReportSkipReason::FollowingStepFailure));
    CHECK(builder.finish("2026-08-27T06:00:03Z"));
    auto report = builder.terminal_snapshot();
    CHECK(report);
    CHECK(report->state() == ReportState::PartiallyFailed);
    CHECK(report->summary().succeeded == 1U);
    CHECK(report->summary().failed == 1U);
    CHECK(report->canonical_json() ==
          read_golden("job-report-v1.golden.json"));
    CHECK(report->canonical_json().find('\n') == std::string_view::npos);
}

void running_snapshots_are_owned_and_immutable() {
    auto builder = make_builder({device("usb:1-2", "SERIAL-01")});
    auto pending = builder.running_snapshot();
    CHECK(pending);
    CHECK(pending->devices()[0].state == ReportWorkState::Pending);
    CHECK(pending->summary().pending == 1U);

    CHECK(builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    CHECK(builder.update_flash_progress(0U, 0U, 1024U));
    auto active = builder.running_snapshot();
    CHECK(active);
    CHECK(active->devices()[0].state == ReportWorkState::Running);
    CHECK(active->devices()[0].steps[0].bytes_transferred == 1024U);
    CHECK(pending->devices()[0].state == ReportWorkState::Pending);
    CHECK(pending->devices()[0].steps[0].bytes_transferred == 0U);

    CHECK(builder.update_flash_progress(0U, 0U, 4096U));
    CHECK(builder.complete_device(0U, 0U, "2026-08-27T06:00:02Z"));
    CHECK(builder.finish("2026-08-27T06:00:03Z"));
    CHECK(!builder.running_snapshot());
    CHECK(active->devices()[0].steps[0].bytes_transferred == 1024U);
}

void every_operation_and_success_state_are_canonical() {
    std::vector<ReportStepSpec> steps{
        flash_step(4U),
        ReportStepSpec{.operation = ReportOperation::Erase,
                       .partition = "userdata"},
        ReportStepSpec{.operation = ReportOperation::SetActive,
                       .slot = ReportSlot::A},
        ReportStepSpec{.operation = ReportOperation::Reboot,
                       .reboot_target = ReportRebootTarget::Bootloader},
        ReportStepSpec{.operation = ReportOperation::Oem,
                       .oem_command = "device-info"},
    };
    auto builder = make_builder(
        {device("usb:2-1", "SERIAL-OPS", std::move(steps))});
    CHECK(builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    CHECK(builder.update_flash_progress(0U, 0U, 4U));
    CHECK(builder.advance_step(0U,
                               0U,
                               "2026-08-27T06:00:02Z",
                               "2026-08-27T06:00:02.1Z"));
    CHECK(builder.advance_step(0U,
                               1U,
                               "2026-08-27T06:00:03Z",
                               "2026-08-27T06:00:03Z"));
    CHECK(builder.advance_step(0U,
                               2U,
                               "2026-08-27T06:00:04Z",
                               "2026-08-27T06:00:04.01Z"));
    CHECK(builder.advance_step(0U,
                               3U,
                               "2026-08-27T06:00:05Z",
                               "2026-08-27T06:00:05.0Z"));
    CHECK(builder.complete_device(0U, 4U, "2026-08-27T06:00:06Z"));
    CHECK(builder.finish("2026-08-27T06:00:07Z"));
    auto report = builder.terminal_snapshot();
    CHECK(report);
    CHECK(report->state() == ReportState::Succeeded);
    CHECK(report->summary().total == 1U);
    CHECK(report->summary().succeeded == 1U);
    const auto json = report->canonical_json();
    CHECK(json.find("\"operation\":\"erase\"") != std::string_view::npos);
    CHECK(json.find("\"operation\":\"set_active\"") !=
          std::string_view::npos);
    CHECK(json.find("\"operation\":\"reboot\"") !=
          std::string_view::npos);
    CHECK(json.find("\"operation\":\"oem\"") != std::string_view::npos);
}

void device_and_job_failure_states_are_complete() {
    auto failed = make_builder({device("usb:1-3", "SERIAL-02")});
    CHECK(failed.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    CHECK(failed.fail_step(0U,
                           0U,
                           "2026-08-27T06:00:02Z",
                           device_failure(),
                           ReportSkipReason::FollowingStepFailure));
    CHECK(failed.finish("2026-08-27T06:00:03Z"));
    auto device_report = failed.terminal_snapshot();
    CHECK(device_report);
    CHECK(device_report->state() == ReportState::Failed);
    CHECK(!device_report->error());

    auto preflight = make_builder({});
    CHECK(preflight.finish_failed("2026-08-27T06:00:01Z",
                                  preflight_failure()));
    auto job_report = preflight.terminal_snapshot();
    CHECK(job_report);
    CHECK(job_report->state() == ReportState::Failed);
    CHECK(job_report->devices().empty());
    CHECK(job_report->error()->code == KB_E_INVALID_ARGUMENT);
    CHECK(job_report->summary().total == 0U);
}

void product_mismatch_is_all_skipped() {
    auto mismatch = device("usb:3-1", "SERIAL-MISMATCH",
                           {flash_step(),
                            ReportStepSpec{.operation = ReportOperation::Reboot,
                                           .reboot_target =
                                               ReportRebootTarget::System}});
    mismatch.observed_product = "product_b";
    auto builder = make_builder({std::move(mismatch)});
    CHECK(!builder.running_snapshot());
    CHECK(builder.fail_product_preflight(
        0U,
        std::optional<std::string>{"product_b"},
        "2026-08-27T06:00:01Z",
        ReportError{.code = KB_E_DEVICE_FAIL,
                    .message = "device product does not match target",
                    .transfer_certainty = KB_TRANSFER_NOT_SENT}));
    CHECK(builder.finish("2026-08-27T06:00:02Z"));
    auto report = builder.terminal_snapshot();
    CHECK(report);
    CHECK(report->devices()[0].state == ReportWorkState::Failed);
    for (const auto& step : report->devices()[0].steps) {
        CHECK(step.state == ReportWorkState::Skipped);
        CHECK(step.skip_reason == ReportSkipReason::ProductMismatch);
        CHECK(!step.error);
        CHECK(!step.started_at);
        CHECK(step.finished_at == "2026-08-27T06:00:01Z");
    }
    CHECK(report->canonical_json().find("skippedReason") ==
          std::string_view::npos);
}

void products_can_be_verified_incrementally_before_publication() {
    auto first = device("usb:3-2", "SERIAL-PRE-1");
    auto second = device("usb:3-3", "SERIAL-PRE-2");
    first.observed_product.reset();
    second.observed_product.reset();
    auto builder = make_builder({std::move(first), std::move(second)});
    CHECK(!builder.running_snapshot());
    CHECK(builder.verify_product(0U, "product_a"));
    CHECK(!builder.running_snapshot());
    CHECK(builder.verify_product(1U, "product_a"));
    auto snapshot = builder.running_snapshot();
    CHECK(snapshot);
    CHECK(snapshot->summary().pending == 2U);
    CHECK(snapshot->devices()[0].observed_product == "product_a");
    CHECK(snapshot->devices()[1].observed_product == "product_a");
}

void policy_stop_requires_typed_skip_reason() {
    auto builder = make_builder({device("usb:4-1", "SERIAL-FAIL"),
                                 device("usb:4-2", "SERIAL-SKIP")});
    CHECK(builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    CHECK(builder.fail_step(0U,
                            0U,
                            "2026-08-27T06:00:02Z",
                            device_failure(),
                            ReportSkipReason::FollowingStepFailure));
    CHECK(builder.skip_pending_device(1U,
                                      "2026-08-27T06:00:02Z",
                                      ReportSkipReason::PolicyStopped));
    CHECK(builder.finish("2026-08-27T06:00:03Z"));
    auto report = builder.terminal_snapshot();
    CHECK(report);
    CHECK(report->state() == ReportState::Failed);
    CHECK(report->summary().failed == 1U);
    CHECK(report->summary().skipped == 1U);
    CHECK(report->devices()[1].steps[0].skip_reason ==
          ReportSkipReason::PolicyStopped);
}

void cancellation_latch_wins_after_success_and_failure() {
    auto builder = make_builder({device("usb:5-1", "SERIAL-OK"),
                                 device("usb:5-2", "SERIAL-FAIL"),
                                 device("usb:5-3", "SERIAL-PENDING")});
    CHECK(builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    CHECK(builder.update_flash_progress(0U, 0U, 4096U));
    CHECK(builder.complete_device(0U, 0U, "2026-08-27T06:00:02Z"));
    CHECK(builder.begin_first_step(1U, "2026-08-27T06:00:01Z"));
    CHECK(builder.fail_step(1U,
                            0U,
                            "2026-08-27T06:00:02Z",
                            device_failure(),
                            ReportSkipReason::FollowingStepFailure));
    CHECK(builder.request_cancellation(cancellation()));

    const auto normal = builder.finish("2026-08-27T06:00:03Z");
    CHECK(!normal);
    CHECK(normal.error().kind == JobReportErrorKind::CancellationLatched);
    CHECK(builder.finish_cancelled("2026-08-27T06:00:04Z"));
    auto report = builder.terminal_snapshot();
    CHECK(report);
    CHECK(report->state() == ReportState::Cancelled);
    CHECK(report->error()->code == KB_E_CANCELLED);
    CHECK(report->summary().succeeded == 1U);
    CHECK(report->summary().failed == 1U);
    CHECK(report->summary().cancelled == 1U);
    CHECK(report->devices()[2].steps[0].state ==
          ReportWorkState::Cancelled);
}

void transport_drain_cancellation_blocks_racing_failure() {
    auto builder = make_builder({device("usb:6-1", "SERIAL-RACE",
                                        {flash_step(), flash_step()})});
    CHECK(builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    CHECK(builder.update_flash_progress(0U, 0U, 1024U));

    std::latch cancellation_latched{1};
    std::atomic<bool> cancellation_ok{false};
    std::atomic<JobReportErrorKind> failure_kind{
        JobReportErrorKind::UnexpectedFailure};
    std::thread canceller([&] {
        cancellation_ok = static_cast<bool>(
            builder.request_cancellation(cancellation()));
        cancellation_latched.count_down();
    });
    std::thread transport_drain([&] {
        cancellation_latched.wait();
        const auto failure = builder.fail_step(
            0U,
            0U,
            "2026-08-27T06:00:02Z",
            device_failure("late transport failure"),
            ReportSkipReason::FollowingStepFailure);
        CHECK(!failure);
        failure_kind = failure.error().kind;
    });
    canceller.join();
    transport_drain.join();
    CHECK(cancellation_ok);
    CHECK(failure_kind == JobReportErrorKind::CancellationLatched);
    CHECK(builder.finish_cancelled("2026-08-27T06:00:03Z",
                                   ReportSkipReason::FollowingStepCancellation));
    auto report = builder.terminal_snapshot();
    CHECK(report);
    CHECK(report->devices()[0].steps[0].state ==
          ReportWorkState::Cancelled);
    CHECK(report->devices()[0].steps[1].skip_reason ==
          ReportSkipReason::FollowingStepCancellation);
}

void illegal_transitions_and_time_order_fail_closed() {
    auto builder = make_builder({device("usb:7-1", "SERIAL-ILLEGAL",
                                        {flash_step(), flash_step()})});
    CHECK(!builder.terminal_snapshot());
    CHECK(!builder.update_flash_progress(0U, 0U, 1U));
    CHECK(!builder.finish("2026-08-27T06:00:01Z"));
    CHECK(!builder.begin_first_step(0U, "2026-08-27T05:59:59Z"));
    CHECK(builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    CHECK(!builder.update_flash_progress(0U, 0U, 4097U));
    CHECK(!builder.advance_step(0U,
                                0U,
                                "2026-08-27T06:00:02Z",
                                "2026-08-27T06:00:01.9Z"));
    CHECK(!builder.fail_step(0U,
                             0U,
                             "2026-08-27T06:00:02Z",
                             cancellation(),
                             ReportSkipReason::FollowingStepFailure));
    auto snapshot = builder.running_snapshot();
    CHECK(snapshot);
    CHECK(snapshot->devices()[0].steps[0].state == ReportWorkState::Running);
    CHECK(snapshot->devices()[0].steps[0].bytes_transferred == 0U);

    auto invalid_date = JobReportBuilder::create(
        "job", plan_digest(), "2026-02-30T06:00:00Z", {});
    CHECK(!invalid_date);
    CHECK(invalid_date.error().kind == JobReportErrorKind::InvalidTimestamp);
    auto unsafe = flash_step(9'007'199'254'740'992ULL);
    CHECK(!JobReportBuilder::create(
        "job",
        plan_digest(),
        "2026-08-27T06:00:00Z",
        {device("usb:7-2", "SERIAL-UNSAFE", {std::move(unsafe)})}));
    CHECK(!JobReportBuilder::create(
        "job",
        plan_digest(),
        "2026-08-27T06:00:00Z",
        {device("short-id", "SERIAL-A"),
         device("short-id", "SERIAL-B")}));
    auto unknown_operation = flash_step();
    unknown_operation.operation = static_cast<ReportOperation>(255U);
    CHECK(!JobReportBuilder::create(
        "job",
        plan_digest(),
        "2026-08-27T06:00:00Z",
        {device("usb:7-3", "SERIAL-ENUM", {unknown_operation})}));
}

void every_stable_kb_error_code_has_the_frozen_status_name() {
    using Case = std::pair<kb_status_t, std::string_view>;
    constexpr std::array<Case, 11U> cases{
        Case{KB_E_INVALID_ARGUMENT, "invalid_argument"},
        Case{KB_E_OUT_OF_MEMORY, "out_of_memory"},
        Case{KB_E_NOT_SUPPORTED, "not_supported"},
        Case{KB_E_NO_DEVICE, "no_device"},
        Case{KB_E_AMBIGUOUS_DEVICE, "ambiguous_device"},
        Case{KB_E_BUSY, "busy"},
        Case{KB_E_TIMEOUT, "timeout"},
        Case{KB_E_IO, "io"},
        Case{KB_E_INTERNAL, "internal"},
        Case{KB_E_PROTOCOL, "protocol"},
        Case{KB_E_DEVICE_FAIL, "device_fail"},
    };
    for (const auto& [code, status] : cases) {
        auto builder = make_builder({});
        CHECK(builder.finish_failed(
            "2026-08-27T06:00:01Z",
            ReportError{.code = code,
                        .message = "failure",
                        .native_code = std::numeric_limits<std::int32_t>::min(),
                        .transfer_certainty = std::nullopt}));
        auto report = builder.terminal_snapshot();
        CHECK(report);
        CHECK(report->canonical_json().find(
                  std::string{"\"status\":\""} + std::string{status} +
                  "\"") != std::string_view::npos);
    }
    auto cancelled = make_builder({});
    CHECK(cancelled.request_cancellation(cancellation()));
    CHECK(cancelled.finish_cancelled("2026-08-27T06:00:01Z"));
    auto report = cancelled.terminal_snapshot();
    CHECK(report);
    CHECK(report->canonical_json().find("\"status\":\"cancelled\"") !=
          std::string_view::npos);
}

struct FaultState final {
    std::atomic<bool> fail_commit{false};
    std::atomic<bool> fail_snapshot{false};
};

void fault_hook(const JobReportFaultPoint point, void* context) {
    auto& state = *static_cast<FaultState*>(context);
    if (point == JobReportFaultPoint::BeforeCommit &&
        state.fail_commit.exchange(false)) {
        throw std::bad_alloc{};
    }
    if (point == JobReportFaultPoint::BeforeSnapshotPublish &&
        state.fail_snapshot.exchange(false)) {
        throw std::bad_alloc{};
    }
}

void commit_and_snapshot_oom_have_strong_guarantee() {
    FaultState faults;
    auto builder = make_builder(
        {device("usb:8-1", "SERIAL-OOM")},
        JobReportBuilderOptions{.fault_hook = &fault_hook,
                                .fault_context = &faults});
    auto before = builder.running_snapshot();
    CHECK(before);

    faults.fail_commit = true;
    try {
        static_cast<void>(
            builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
        throw CheckFailure{"expected commit allocation failure"};
    } catch (const std::bad_alloc&) {
    }
    auto unchanged = builder.running_snapshot();
    CHECK(unchanged);
    CHECK(unchanged->canonical_json() == before->canonical_json());

    faults.fail_snapshot = true;
    try {
        static_cast<void>(builder.running_snapshot());
        throw CheckFailure{"expected snapshot allocation failure"};
    } catch (const std::bad_alloc&) {
    }
    auto after = builder.running_snapshot();
    CHECK(after);
    CHECK(after->canonical_json() == before->canonical_json());
}

void concurrent_progress_and_snapshots_remain_consistent() {
    constexpr std::uint64_t kBytes = 1024U;
    auto builder = make_builder(
        {device("usb:9-1", "SERIAL-THREAD", {flash_step(kBytes)})});
    CHECK(builder.begin_first_step(0U, "2026-08-27T06:00:01Z"));
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (std::uint64_t value = 1U; value <= kBytes; ++value) {
            CHECK(builder.update_flash_progress(0U, 0U, value));
        }
        done = true;
    });
    std::uint64_t observed = 0U;
    while (!done.load()) {
        auto snapshot = builder.running_snapshot();
        CHECK(snapshot);
        const auto value = *snapshot->devices()[0].steps[0].bytes_transferred;
        CHECK(value >= observed);
        CHECK(value <= kBytes);
        observed = value;
    }
    writer.join();
    CHECK(builder.complete_device(0U, 0U, "2026-08-27T06:00:02Z"));
    CHECK(builder.finish("2026-08-27T06:00:03Z"));
    auto terminal = builder.terminal_snapshot();
    CHECK(terminal);
    CHECK(terminal->devices()[0].steps[0].bytes_transferred == kBytes);
}

void utf8_and_fractional_timestamp_profile_is_frozen() {
    auto builder = JobReportBuilder::create(
        std::string{"job-\"-"} + "\xE9\x9B\xAA",
        plan_digest(),
        "2026-08-27T06:00:00.0100Z",
        {device("usb:10-1", "SERIAL-UTF8")});
    CHECK(builder);
    CHECK(builder->begin_first_step(0U, "2026-08-27T06:00:00.1Z"));
    CHECK(builder->update_flash_progress(0U, 0U, 4096U));
    CHECK(builder->complete_device(0U, 0U, "2026-08-27T06:00:00.10Z"));
    CHECK(builder->finish("2026-08-27T06:00:00.1000Z"));
    auto report = builder->terminal_snapshot();
    CHECK(report);
    CHECK(report->canonical_json().find("job-\\\"") != std::string_view::npos);
    CHECK(report->canonical_json().find("\xE9\x9B\xAA") !=
          std::string_view::npos);

    auto invalid_utf8 = device("usb:10-2", "SERIAL-BAD");
    invalid_utf8.target = std::string{"bad"} + static_cast<char>(0x80U);
    const auto rejected = JobReportBuilder::create(
        "job", plan_digest(), "2026-08-27T06:00:00Z", {invalid_utf8});
    CHECK(!rejected);
    CHECK(rejected.error().kind == JobReportErrorKind::InvalidUtf8);
}

}  // namespace

int main() {
    using Test = std::pair<std::string_view, void (*)()>;
    const std::array<Test, 15U> tests{
        Test{"exact golden", &exact_golden_and_partial_failure},
        Test{"running snapshots", &running_snapshots_are_owned_and_immutable},
        Test{"all operations", &every_operation_and_success_state_are_canonical},
        Test{"failure states", &device_and_job_failure_states_are_complete},
        Test{"product mismatch", &product_mismatch_is_all_skipped},
        Test{"incremental product verification",
             &products_can_be_verified_incrementally_before_publication},
        Test{"policy skip reason", &policy_stop_requires_typed_skip_reason},
        Test{"cancellation wins", &cancellation_latch_wins_after_success_and_failure},
        Test{"transport drain cancellation",
             &transport_drain_cancellation_blocks_racing_failure},
        Test{"illegal transitions", &illegal_transitions_and_time_order_fail_closed},
        Test{"stable KB error mapping",
             &every_stable_kb_error_code_has_the_frozen_status_name},
        Test{"strong exception guarantee",
             &commit_and_snapshot_oom_have_strong_guarantee},
        Test{"concurrent snapshots", &concurrent_progress_and_snapshots_remain_consistent},
        Test{"UTF-8 and timestamp profile",
             &utf8_and_fractional_timestamp_profile_is_frozen},
        Test{"repeat exact golden", &exact_golden_and_partial_failure},
    };

    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
