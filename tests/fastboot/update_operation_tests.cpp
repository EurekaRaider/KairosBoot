// SPDX-License-Identifier: MIT
#include "src/fastboot/update_operation.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::IPreparedDeviceTask;
using kairosboot::fastboot::IUpdateDevice;
using kairosboot::fastboot::run_update_package_operation;
using kairosboot::fastboot::UpdateDeviceError;
using kairosboot::fastboot::UpdateDeviceErrorKind;
using kairosboot::fastboot::UpdateDeviceTaskInput;
using kairosboot::fastboot::UpdateExecutionEventKind;
using kairosboot::fastboot::UpdateOperationContext;
using kairosboot::fastboot::UpdatePackageOperationErrorKind;
using kairosboot::fastboot::UpdatePackageOperationOptions;
using kairosboot::fastboot::UpdatePackageOperationStage;
using kairosboot::fastboot::UpdatePackagePreflightErrorKind;
using kairosboot::image::ArtifactSourceErrorKind;
using kairosboot::image::ArtifactSourceLimits;
using kairosboot::image::ArtifactSourceResolver;
using kairosboot::protocol::ProtocolPhase;
using kairosboot::protocol::Response;
using kairosboot::protocol::ResponseKind;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransportStatus;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                               \
    do {                                                                               \
        if (!(condition)) {                                                            \
            throw CheckFailure(std::string("check failed at line ") +                  \
                               std::to_string(__LINE__) + ": " #condition);            \
        }                                                                              \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        const auto suffix = sequence.fetch_add(1U, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                ("kairosboot-update-operation-test-" + std::to_string(suffix));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_text(const std::filesystem::path& path,
                const std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw CheckFailure("unable to write update operation fixture");
    }
}

[[nodiscard]] std::filesystem::path create_package(
    const TemporaryDirectory& temporary,
    const std::string_view android_info,
    const std::string_view fastboot_info) {
    const auto package = temporary.path() / "package";
    std::filesystem::create_directory(package);
    write_text(package / "android-info.txt", android_info);
    write_text(package / "fastboot-info.txt", fastboot_info);
    return package;
}

struct DeviceState final {
    std::size_t expected_tasks{};
    std::size_t getvar_calls{};
    std::size_t prepare_calls{};
    std::size_t execute_calls{};
    bool executed_before_all_prepared{};
    std::vector<std::chrono::steady_clock::time_point> deadlines;
    std::optional<std::size_t> failing_task;
    std::optional<UpdateDeviceError> task_error;
};

class ScriptedPreparedTask final : public IPreparedDeviceTask {
public:
    ScriptedPreparedTask(std::shared_ptr<DeviceState> state,
                         const std::size_t index) noexcept
        : state_(std::move(state)), index_(index) {}

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    execute(const UpdateOperationContext& context) const override {
        if (state_->prepare_calls != state_->expected_tasks) {
            state_->executed_before_all_prepared = true;
        }
        ++state_->execute_calls;
        CHECK(context.deadline.has_value());
        state_->deadlines.push_back(*context.deadline);
        if (state_->failing_task == index_) {
            CHECK(state_->task_error.has_value());
            return std::unexpected(*state_->task_error);
        }
        return {};
    }

private:
    std::shared_ptr<DeviceState> state_;
    std::size_t index_{};
};

class ScriptedUpdateDevice final : public IUpdateDevice {
public:
    explicit ScriptedUpdateDevice(std::shared_ptr<DeviceState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] std::expected<std::string, UpdateDeviceError>
    getvar(const std::string_view name,
           const UpdateOperationContext& context) override {
        ++state_->getvar_calls;
        CHECK(context.deadline.has_value());
        state_->deadlines.push_back(*context.deadline);
        if (name == "product") {
            return std::string("atlas");
        }
        return std::string("yes");
    }

    [[nodiscard]] std::expected<std::unique_ptr<IPreparedDeviceTask>,
                                UpdateDeviceError>
    prepare_task(UpdateDeviceTaskInput,
                 const UpdateOperationContext& context) override {
        const auto index = state_->prepare_calls;
        ++state_->prepare_calls;
        CHECK(context.deadline.has_value());
        state_->deadlines.push_back(*context.deadline);
        std::unique_ptr<IPreparedDeviceTask> token =
            std::make_unique<ScriptedPreparedTask>(state_, index);
        return token;
    }

private:
    std::shared_ptr<DeviceState> state_;
};

[[nodiscard]] ScriptedUpdateDevice device_with_tasks(
    const std::shared_ptr<DeviceState>& state,
    const std::size_t expected_tasks) {
    state->expected_tasks = expected_tasks;
    return ScriptedUpdateDevice(state);
}

void one_finite_deadline_is_shared_and_all_tasks_prepare_first() {
    TemporaryDirectory temporary;
    const auto package = create_package(
        temporary, "require product=atlas\n",
        "erase cache\nreboot fastboot\n");
    ArtifactSourceResolver resolver;
    auto state = std::make_shared<DeviceState>();
    auto device = device_with_tasks(state, 2U);
    UpdatePackageOperationOptions options;
    options.timeout = std::chrono::hours(1);

    const auto before = std::chrono::steady_clock::now();
    auto result = run_update_package_operation(
        resolver, package, device, options);
    const auto after = std::chrono::steady_clock::now();

    CHECK(result.has_value());
    CHECK(result->deadline >= before + std::chrono::hours(1));
    CHECK(result->deadline <= after + std::chrono::hours(1));
    CHECK(result->execution.completed_tasks == 2U);
    CHECK(state->getvar_calls == 1U);
    CHECK(state->prepare_calls == 2U);
    CHECK(state->execute_calls == 2U);
    CHECK(!state->executed_before_all_prepared);
    CHECK(!state->deadlines.empty());
    for (const auto observed : state->deadlines) {
        CHECK(observed == result->deadline);
    }
}

void infinite_budget_is_an_explicit_shared_max_deadline() {
    TemporaryDirectory temporary;
    const auto package = create_package(temporary, "", "erase cache\n");
    ArtifactSourceResolver resolver;
    auto state = std::make_shared<DeviceState>();
    auto device = device_with_tasks(state, 1U);

    auto result = run_update_package_operation(resolver, package, device);

    CHECK(result.has_value());
    CHECK(result->deadline == std::chrono::steady_clock::time_point::max());
    CHECK(state->deadlines.size() == 2U);
    CHECK(state->deadlines[0] == result->deadline);
    CHECK(state->deadlines[1] == result->deadline);
}

void timeout_overflow_fails_before_package_or_device_work() {
    TemporaryDirectory temporary;
    ArtifactSourceLimits source_limits;
    std::size_t entries_seen = 0U;
    source_limits.package_entry_observer =
        [&](const std::string_view) { ++entries_seen; };
    ArtifactSourceResolver resolver(source_limits);
    auto state = std::make_shared<DeviceState>();
    auto device = device_with_tasks(state, 0U);
    UpdatePackageOperationOptions options;
    options.timeout = -std::chrono::steady_clock::duration{1};

    auto negative = run_update_package_operation(
        resolver, temporary.path() / "not-opened", device, options);

    CHECK(!negative.has_value());
    CHECK(negative.error().kind ==
          UpdatePackageOperationErrorKind::InvalidTimeout);
    CHECK(negative.error().stage == UpdatePackageOperationStage::Deadline);

    options.timeout = std::chrono::steady_clock::duration::max();

    auto result = run_update_package_operation(
        resolver, temporary.path() / "not-opened", device, options);

    CHECK(!result.has_value());
    CHECK(result.error().kind ==
          UpdatePackageOperationErrorKind::InvalidTimeout);
    CHECK(result.error().stage == UpdatePackageOperationStage::Deadline);
    CHECK(entries_seen == 0U);
    CHECK(state->getvar_calls == 0U);
    CHECK(state->prepare_calls == 0U);
    CHECK(state->execute_calls == 0U);
}

void simultaneous_initial_cancel_and_timeout_prefers_cancellation() {
    TemporaryDirectory temporary;
    ArtifactSourceResolver resolver;
    auto state = std::make_shared<DeviceState>();
    auto device = device_with_tasks(state, 0U);
    std::stop_source cancelled;
    cancelled.request_stop();
    UpdatePackageOperationOptions options;
    options.timeout = std::chrono::steady_clock::duration::zero();

    auto result = run_update_package_operation(
        resolver, temporary.path() / "not-opened", device, options,
        cancelled.get_token());

    CHECK(!result.has_value());
    CHECK(result.error().kind == UpdatePackageOperationErrorKind::Cancelled);
    CHECK(result.error().stage ==
          UpdatePackageOperationStage::PackagePreflight);
    CHECK(!result.error().preflight_error.has_value());
    CHECK(state->getvar_calls == 0U);
    CHECK(state->prepare_calls == 0U);
    CHECK(state->execute_calls == 0U);
}

void incomplete_preflight_preserves_metadata_and_sends_no_commands() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "missing-android-info";
    std::filesystem::create_directory(package);
    write_text(package / "fastboot-info.txt", "erase cache\n");
    ArtifactSourceResolver resolver;
    auto state = std::make_shared<DeviceState>();
    auto device = device_with_tasks(state, 0U);

    auto result = run_update_package_operation(resolver, package, device);

    CHECK(!result.has_value());
    CHECK(result.error().kind ==
          UpdatePackageOperationErrorKind::PreflightFailed);
    CHECK(result.error().stage ==
          UpdatePackageOperationStage::PackagePreflight);
    CHECK(result.error().preflight_error.has_value());
    const auto& original = *result.error().preflight_error;
    CHECK(original.kind ==
          UpdatePackagePreflightErrorKind::MissingAndroidInfo);
    CHECK(original.artifact == "android-info.txt");
    CHECK(!original.artifact_error.has_value());
    CHECK(!original.manifest_error.has_value());
    CHECK(state->getvar_calls == 0U);
    CHECK(state->prepare_calls == 0U);
    CHECK(state->execute_calls == 0U);
}

void cancellation_inside_preflight_preserves_original_error() {
    TemporaryDirectory temporary;
    const auto package = create_package(temporary, "", "erase cache\n");
    std::stop_source cancelled;
    ArtifactSourceLimits source_limits;
    source_limits.package_entry_observer = [&](const std::string_view) {
        cancelled.request_stop();
    };
    ArtifactSourceResolver resolver(source_limits);
    auto state = std::make_shared<DeviceState>();
    auto device = device_with_tasks(state, 1U);

    auto result = run_update_package_operation(
        resolver, package, device, {}, cancelled.get_token());

    CHECK(!result.has_value());
    CHECK(result.error().kind == UpdatePackageOperationErrorKind::Cancelled);
    CHECK(result.error().stage ==
          UpdatePackageOperationStage::PackagePreflight);
    CHECK(result.error().preflight_error.has_value());
    CHECK(result.error().preflight_error->kind ==
          UpdatePackagePreflightErrorKind::Cancelled);
    CHECK(result.error().preflight_error->artifact_error.has_value());
    CHECK(result.error().preflight_error->artifact_error->kind ==
          ArtifactSourceErrorKind::Cancelled);
    CHECK(state->getvar_calls == 0U);
    CHECK(state->prepare_calls == 0U);
    CHECK(state->execute_calls == 0U);
}

[[nodiscard]] UpdateDeviceError rich_device_failure() {
    UpdateDeviceError error;
    error.kind = UpdateDeviceErrorKind::TimedOut;
    error.phase = ProtocolPhase::FinalResponse;
    error.message = "wire deadline expired";
    error.device_message = "device detail";
    error.informational.push_back(Response{
        .kind = ResponseKind::Info,
        .payload = "still working",
        .data_size = std::nullopt,
    });
    error.transport_status = TransportStatus::Timeout;
    error.transport_certainty = TransferCertainty::PartialOrUnknown;
    error.outbound_certainty = TransferCertainty::FullyTransferred;
    error.inbound_expected = 4096U;
    error.inbound_transferred = 1024U;
    error.inbound_certainty = TransferCertainty::PartialOrUnknown;
    error.session_poisoned = true;
    error.session_closed = false;
    error.native_code = 73;
    error.task_certainty = TransferCertainty::PartialOrUnknown;
    error.completed_actions = 1U;
    error.total_actions = 3U;
    return error;
}

void device_error_metadata_and_task_progress_are_preserved() {
    TemporaryDirectory temporary;
    const auto package = create_package(
        temporary, "", "erase cache\nreboot fastboot\n");
    ArtifactSourceResolver resolver;
    auto state = std::make_shared<DeviceState>();
    state->failing_task = 1U;
    state->task_error = rich_device_failure();
    auto device = device_with_tasks(state, 2U);

    auto result = run_update_package_operation(resolver, package, device);

    CHECK(!result.has_value());
    const auto& operation = result.error();
    CHECK(operation.kind == UpdatePackageOperationErrorKind::TimedOut);
    CHECK(operation.stage == UpdatePackageOperationStage::TaskExecution);
    CHECK(operation.completed_tasks == 1U);
    CHECK(operation.total_tasks == 2U);
    CHECK(operation.completed_actions == 1U);
    CHECK(operation.total_actions == 3U);
    CHECK(operation.task_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(operation.execution_error.has_value());
    CHECK(operation.execution_error->device_error.has_value());
    const auto& original = *operation.execution_error->device_error;
    CHECK(original.kind == UpdateDeviceErrorKind::TimedOut);
    CHECK(original.phase == ProtocolPhase::FinalResponse);
    CHECK(original.message == "wire deadline expired");
    CHECK(original.device_message == "device detail");
    CHECK(original.informational.size() == 1U);
    CHECK(original.informational[0].kind == ResponseKind::Info);
    CHECK(original.informational[0].payload == "still working");
    CHECK(!original.informational[0].data_size.has_value());
    CHECK(original.transport_status == TransportStatus::Timeout);
    CHECK(original.transport_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(original.outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(original.inbound_expected == 4096U);
    CHECK(original.inbound_transferred == 1024U);
    CHECK(original.inbound_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(original.session_poisoned);
    CHECK(!original.session_closed);
    CHECK(original.native_code == 73);
    CHECK(original.task_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(original.completed_actions == 1U);
    CHECK(original.total_actions == 3U);
    CHECK(state->prepare_calls == 2U);
    CHECK(state->execute_calls == 2U);
    CHECK(!state->executed_before_all_prepared);
}

void task_started_cancellation_stops_before_destructive_execute() {
    TemporaryDirectory temporary;
    const auto package = create_package(temporary, "", "erase cache\n");
    ArtifactSourceResolver resolver;
    auto state = std::make_shared<DeviceState>();
    auto device = device_with_tasks(state, 1U);
    std::stop_source cancelled;
    UpdatePackageOperationOptions options;
    options.observer = [&](const auto& event) {
        if (event.kind == UpdateExecutionEventKind::TaskStarted) {
            cancelled.request_stop();
        }
    };

    auto result = run_update_package_operation(
        resolver, package, device, options, cancelled.get_token());

    CHECK(!result.has_value());
    CHECK(result.error().kind == UpdatePackageOperationErrorKind::Cancelled);
    CHECK(result.error().stage == UpdatePackageOperationStage::TaskExecution);
    CHECK(result.error().completed_tasks == 0U);
    CHECK(result.error().total_tasks == 1U);
    CHECK(result.error().completed_actions == 0U);
    CHECK(result.error().total_actions == 0U);
    CHECK(result.error().task_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(state->prepare_calls == 1U);
    CHECK(state->execute_calls == 0U);
}

void completion_observer_failure_reports_completed_device_prefix() {
    TemporaryDirectory temporary;
    const auto package = create_package(temporary, "", "erase cache\n");
    ArtifactSourceResolver resolver;
    auto state = std::make_shared<DeviceState>();
    auto device = device_with_tasks(state, 1U);
    UpdatePackageOperationOptions options;
    options.observer = [](const auto& event) {
        if (event.kind == UpdateExecutionEventKind::ExecutionCompleted) {
            throw std::runtime_error("completion observer failed");
        }
    };

    auto result = run_update_package_operation(
        resolver, package, device, options);

    CHECK(!result.has_value());
    CHECK(result.error().kind ==
          UpdatePackageOperationErrorKind::ExecutionFailed);
    CHECK(result.error().stage == UpdatePackageOperationStage::Completion);
    CHECK(result.error().completed_tasks == 1U);
    CHECK(result.error().total_tasks == 1U);
    CHECK(result.error().execution_error.has_value());
    CHECK(state->prepare_calls == 1U);
    CHECK(state->execute_calls == 1U);
}

struct Test final {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main() {
    const std::array tests{
        Test{"shared finite deadline and prepare barrier",
             one_finite_deadline_is_shared_and_all_tasks_prepare_first},
        Test{"infinite deadline", infinite_budget_is_an_explicit_shared_max_deadline},
        Test{"deadline overflow", timeout_overflow_fails_before_package_or_device_work},
        Test{"cancel timeout race",
             simultaneous_initial_cancel_and_timeout_prefers_cancellation},
        Test{"preflight fidelity",
             incomplete_preflight_preserves_metadata_and_sends_no_commands},
        Test{"preflight cancellation fidelity",
             cancellation_inside_preflight_preserves_original_error},
        Test{"device error fidelity",
             device_error_metadata_and_task_progress_are_preserved},
        Test{"TaskStarted cancellation barrier",
             task_started_cancellation_stops_before_destructive_execute},
        Test{"completion stage",
             completion_observer_failure_reports_completed_device_prefix},
    };

    std::size_t failures = 0U;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
