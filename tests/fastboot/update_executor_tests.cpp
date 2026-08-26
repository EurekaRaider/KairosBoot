// SPDX-License-Identifier: MIT
#include "src/fastboot/update_executor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::execute_prepared_update;
using kairosboot::fastboot::IPreparedDeviceTask;
using kairosboot::fastboot::IUpdateDevice;
using kairosboot::fastboot::PlannedRebootTarget;
using kairosboot::fastboot::PlannedRequirement;
using kairosboot::fastboot::PlannedSlot;
using kairosboot::fastboot::PlannedUpdateTask;
using kairosboot::fastboot::PreparedUpdateArtifact;
using kairosboot::fastboot::PreparedUpdatePackage;
using kairosboot::fastboot::RequirementAction;
using kairosboot::fastboot::UpdateDeviceError;
using kairosboot::fastboot::UpdateDeviceErrorKind;
using kairosboot::fastboot::UpdateDeviceTaskInput;
using kairosboot::fastboot::UpdateExecutionErrorKind;
using kairosboot::fastboot::UpdateExecutionEventKind;
using kairosboot::fastboot::UpdateExecutorOptions;
using kairosboot::fastboot::UpdateOperationContext;
using kairosboot::fastboot::UpdateSuperArtifactInput;
using kairosboot::fastboot::UpdateManifestKind;
using kairosboot::fastboot::UpdateSourceLocation;
using kairosboot::fastboot::UpdateTaskKind;
using kairosboot::image::ArtifactSourceOrigin;
using kairosboot::image::FlashArtifact;
using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::ResolvedArtifact;
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

class MemorySource final : public IImageSource {
public:
    explicit MemorySource(std::string_view contents) {
        bytes_.resize(contents.size());
        std::memcpy(bytes_.data(), contents.data(), contents.size());
    }

    [[nodiscard]] std::uint64_t size() const noexcept override { return bytes_.size(); }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError>
    read_at(const std::uint64_t offset,
            const std::span<std::byte> destination) const override {
        if (offset >= bytes_.size()) {
            return std::size_t{0};
        }
        const auto copied = std::min<std::size_t>(
            destination.size(), bytes_.size() - static_cast<std::size_t>(offset));
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), copied,
                    destination.begin());
        return copied;
    }

private:
    std::vector<std::byte> bytes_;
};

[[nodiscard]] UpdateSourceLocation
location(const std::size_t line,
         const UpdateManifestKind manifest = UpdateManifestKind::AndroidInfo) {
    return {
        .manifest = manifest,
        .line = line,
        .column = 1U,
        .byte_offset = line * 10U,
    };
}

[[nodiscard]] PlannedRequirement
requirement(std::string variable, std::vector<std::string> options,
            const std::size_t line,
            const RequirementAction action = RequirementAction::Require,
            std::optional<std::string> product = std::nullopt) {
    return {
        .action = action,
        .variable = std::move(variable),
        .product = std::move(product),
        .options = std::move(options),
        .location = location(line),
    };
}

[[nodiscard]] PlannedUpdateTask
flash_task(std::string partition, std::string artifact, const std::size_t line,
           const PlannedSlot slot = PlannedSlot::Default,
           const bool apply_vbmeta = false) {
    return {
        .kind = UpdateTaskKind::Flash,
        .location = location(line, UpdateManifestKind::FastbootInfo),
        .partition = std::move(partition),
        .artifact = std::move(artifact),
        .slot = slot,
        .apply_vbmeta = apply_vbmeta,
    };
}

[[nodiscard]] PlannedUpdateTask erase_task(std::string partition,
                                           const std::size_t line) {
    return {
        .kind = UpdateTaskKind::Erase,
        .location = location(line, UpdateManifestKind::FastbootInfo),
        .partition = std::move(partition),
    };
}

[[nodiscard]] PlannedUpdateTask reboot_task(const PlannedRebootTarget target,
                                            const std::size_t line) {
    return {
        .kind = UpdateTaskKind::Reboot,
        .location = location(line, UpdateManifestKind::FastbootInfo),
        .reboot_target = target,
    };
}

[[nodiscard]] PlannedUpdateTask update_super_task(const std::size_t line) {
    return {
        .kind = UpdateTaskKind::UpdateSuper,
        .location = location(line, UpdateManifestKind::FastbootInfo),
    };
}

[[nodiscard]] PreparedUpdateArtifact
make_artifact(std::string name, const std::string_view contents = "raw-image") {
    auto source = std::make_shared<MemorySource>(contents);
    auto inspected = FlashArtifact::inspect(source);
    CHECK(inspected);
    auto resolved = std::make_shared<ResolvedArtifact>(ResolvedArtifact{
        .source = source,
        .origin = ArtifactSourceOrigin::DirectoryEntry,
        .logical_name = name,
    });
    return {
        .name = std::move(name),
        .resolved = std::move(resolved),
        .artifact = std::move(*inspected),
    };
}

[[nodiscard]] PreparedUpdatePackage
make_package(std::vector<PlannedRequirement> requirements,
             std::vector<PlannedUpdateTask> tasks,
             std::vector<PreparedUpdateArtifact> artifacts) {
    const bool requires_device_validation = !requirements.empty();
    return {
        .plan =
            {
                .requirements = std::move(requirements),
                .tasks = std::move(tasks),
            },
        .artifacts = std::move(artifacts),
        .requires_device_validation = requires_device_validation,
    };
}

class ScriptedUpdateDevice final : public IUpdateDevice {
public:
    [[nodiscard]] std::expected<std::string, UpdateDeviceError>
    getvar(const std::string_view name,
           const UpdateOperationContext& context) override {
        getvar_calls.emplace_back(name);
        observed_deadlines.push_back(context.deadline);
        if (throw_getvar == name) {
            throw std::runtime_error("scripted getvar exception");
        }
        if (fail_getvar == name) {
            return std::unexpected(UpdateDeviceError{
                .message = "scripted getvar failure",
                .native_code = 17,
            });
        }
        const auto found = variables.find(name);
        if (found == variables.end()) {
            return std::unexpected(UpdateDeviceError{
                .message = "missing scripted variable",
                .native_code = 2,
            });
        }
        return found->second;
    }

    [[nodiscard]] std::expected<std::unique_ptr<IPreparedDeviceTask>,
                                UpdateDeviceError>
    prepare_task(UpdateDeviceTaskInput input,
                 const UpdateOperationContext& context) override {
        const auto index = prepare_calls.size();
        observed_deadlines.push_back(context.deadline);
        prepare_calls.push_back(call_name(input));
        if (input.task.kind == UpdateTaskKind::Flash) {
            CHECK(input.flash_artifact);
            CHECK(input.flash_artifact->logical_name == input.task.artifact);
            CHECK(input.flash_artifact->source);
            last_flash_source = input.flash_artifact->source;
        } else {
            CHECK(!input.flash_artifact);
        }
        if (input.task.kind == UpdateTaskKind::UpdateSuper && input.super_artifact) {
            last_super_source = input.super_artifact->source;
        }
        if (throw_prepare_index == index) {
            throw std::runtime_error("scripted task preparation exception");
        }
        if (cancel_prepare_index == index) {
            CHECK(cancellation_source != nullptr);
            cancellation_source->request_stop();
        }
        if (fail_prepare_index == index) {
            return std::unexpected(
                prepare_error.value_or(UpdateDeviceError{
                    .message = "scripted task preparation failure",
                    .native_code = 23,
                }));
        }
        return std::unique_ptr<IPreparedDeviceTask>(
            std::make_unique<Token>(*this, std::move(input)));
    }

    std::map<std::string, std::string, std::less<>> variables;
    std::vector<std::string> getvar_calls;
    std::vector<std::string> prepare_calls;
    std::vector<std::string> task_calls;
    std::vector<std::optional<std::chrono::steady_clock::time_point>>
        observed_deadlines;
    std::string fail_getvar;
    std::string throw_getvar;
    std::optional<std::size_t> fail_prepare_index;
    std::optional<std::size_t> cancel_prepare_index;
    std::optional<std::size_t> throw_prepare_index;
    std::optional<std::size_t> fail_task_index;
    std::optional<std::size_t> cancel_task_index;
    std::optional<std::size_t> throw_task_index;
    std::optional<UpdateDeviceError> prepare_error;
    std::optional<UpdateDeviceError> task_error;
    std::stop_source* cancellation_source{};
    std::shared_ptr<const IImageSource> last_flash_source;
    std::shared_ptr<const IImageSource> last_super_source;

private:
    class Token final : public IPreparedDeviceTask {
    public:
        Token(ScriptedUpdateDevice& owner, UpdateDeviceTaskInput input)
            : owner_(owner), input_(std::move(input)) {}

        [[nodiscard]] std::expected<void, UpdateDeviceError>
        execute(const UpdateOperationContext& context) const override {
            return owner_.destructive(input_, context);
        }

    private:
        ScriptedUpdateDevice& owner_;
        const UpdateDeviceTaskInput input_;
    };

    [[nodiscard]] static std::string call_name(const UpdateDeviceTaskInput& input) {
        const auto& task = input.task;
        switch (task.kind) {
        case UpdateTaskKind::Flash:
            return "flash:" + task.partition + ":" + task.artifact +
                   (task.slot == PlannedSlot::Other ? ":slot-other" : ":default") +
                   (task.apply_vbmeta ? ":apply-vbmeta" : ":no-vbmeta");
        case UpdateTaskKind::Erase:
            return "erase:" + task.partition;
        case UpdateTaskKind::Reboot:
            return "reboot:" +
                   std::to_string(static_cast<unsigned>(task.reboot_target));
        case UpdateTaskKind::UpdateSuper:
            return "update-super";
        default:
            return "unknown";
        }
    }

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    destructive(const UpdateDeviceTaskInput& input,
                const UpdateOperationContext& context) {
        const auto index = task_calls.size();
        observed_deadlines.push_back(context.deadline);
        task_calls.push_back(call_name(input));
        if (throw_task_index == index) {
            throw std::runtime_error("scripted task exception");
        }
        if (cancel_task_index == index) {
            CHECK(cancellation_source != nullptr);
            cancellation_source->request_stop();
        }
        if (fail_task_index == index) {
            if (task_error) {
                return std::unexpected(*task_error);
            }
            return std::unexpected(
                UpdateDeviceError{
                    .kind = context.cancellation.stop_requested()
                                ? UpdateDeviceErrorKind::Cancelled
                                : UpdateDeviceErrorKind::Failed,
                    .message = "scripted task failure",
                    .native_code = 31,
                });
        }
        return {};
    }
};

void requirements_are_checked_once_before_ordered_tasks() {
    std::vector<PlannedRequirement> requirements;
    requirements.push_back(requirement("version-bootloader", {"1*"}, 1U));
    requirements.push_back(
        requirement("secure", {"no*", "bad"}, 2U, RequirementAction::Reject));
    requirements.push_back(requirement("version-baseband", {"2*"}, 3U,
                                       RequirementAction::Require, "atlas"));
    requirements.push_back(requirement("ignored-variable", {"never"}, 4U,
                                       RequirementAction::Require, "boreal"));
    requirements.push_back(requirement("partition-exists",
                                       {"vendor", "ignored-second-option"}, 5U,
                                       RequirementAction::Reject, "boreal"));
    requirements.push_back(requirement("version-bootloader", {"1.2.3"}, 6U));
    requirements.push_back(requirement("product", {"atlas"}, 7U));

    std::vector<PlannedUpdateTask> tasks;
    tasks.push_back(flash_task("boot", "boot.img", 10U, PlannedSlot::Other, true));
    tasks.push_back(erase_task("userdata", 11U));
    tasks.push_back(reboot_task(PlannedRebootTarget::Fastboot, 12U));
    tasks.push_back(update_super_task(13U));

    std::vector<PreparedUpdateArtifact> artifacts;
    artifacts.push_back(make_artifact("boot.img"));
    auto prepared =
        make_package(std::move(requirements), std::move(tasks), std::move(artifacts));

    ScriptedUpdateDevice device;
    device.variables = {
        {"product", "atlas"},        {"version-bootloader", "1.2.3"}, {"secure", "yes"},
        {"version-baseband", "2.5"}, {"has-slot:vendor", "no"},
    };
    UpdateExecutorOptions options;
    options.known_partitions = {"vendor"};
    auto result = execute_prepared_update(prepared, device, options);
    CHECK(result);
    CHECK(result->validated_requirements == 7U);
    CHECK(result->completed_tasks == 4U);
    CHECK(device.getvar_calls == std::vector<std::string>({
                                     "product",
                                     "version-bootloader",
                                     "secure",
                                     "version-baseband",
                                     "has-slot:vendor",
                                 }));
    CHECK(device.task_calls == std::vector<std::string>({
                                   "flash:boot:boot.img:slot-other:apply-vbmeta",
                                   "erase:userdata",
                                   "reboot:3",
                                   "update-super",
                               }));
    CHECK(device.prepare_calls == device.task_calls);
    CHECK(device.last_flash_source ==
          prepared.artifacts[0].artifact.transfer_source());
    CHECK(result->trace.front().kind == UpdateExecutionEventKind::ValidationStarted);
    CHECK(result->trace.back().kind == UpdateExecutionEventKind::ExecutionCompleted);
    CHECK(
        std::count_if(result->trace.begin(), result->trace.end(), [](const auto& item) {
            return item.kind == UpdateExecutionEventKind::GetVarCacheHit;
        }) == 2);
}

void requirement_and_product_failures_make_zero_destructive_calls() {
    std::vector<PlannedRequirement> requirements;
    requirements.push_back(
        requirement("version-bootloader", {"1*"}, 21U, RequirementAction::Reject));
    std::vector<PlannedUpdateTask> tasks;
    tasks.push_back(flash_task("boot", "boot.img", 30U));
    std::vector<PreparedUpdateArtifact> artifacts;
    artifacts.push_back(make_artifact("boot.img"));
    auto prepared =
        make_package(std::move(requirements), std::move(tasks), std::move(artifacts));

    ScriptedUpdateDevice rejected;
    rejected.variables = {
        {"product", "atlas"},
        {"version-bootloader", "1.9"},
    };
    auto mismatch = execute_prepared_update(prepared, rejected);
    CHECK(!mismatch);
    CHECK(mismatch.error().kind == UpdateExecutionErrorKind::RequirementNotMet);
    CHECK(mismatch.error().requirement_index == 0U);
    CHECK(mismatch.error().location == location(21U));
    CHECK(rejected.task_calls.empty());

    ScriptedUpdateDevice failed_product;
    failed_product.fail_getvar = "product";
    auto getvar_failure = execute_prepared_update(prepared, failed_product);
    CHECK(!getvar_failure);
    CHECK(getvar_failure.error().kind == UpdateExecutionErrorKind::GetVarFailed);
    CHECK(getvar_failure.error().requirement_index == 0U);
    CHECK(failed_product.task_calls.empty());
}

void partition_exists_uses_host_table_and_has_slot_getvar() {
    std::vector<PlannedRequirement> requirements;
    requirements.push_back(requirement("partition-exists", {"vendor"}, 40U));
    auto prepared = make_package(std::move(requirements), {}, {});

    ScriptedUpdateDevice unknown_host;
    unknown_host.variables = {
        {"product", "atlas"},
        {"has-slot:vendor", "yes"},
    };
    auto unknown = execute_prepared_update(prepared, unknown_host);
    CHECK(!unknown);
    CHECK(unknown.error().kind == UpdateExecutionErrorKind::InvalidPreparedPackage);
    CHECK(unknown_host.getvar_calls ==
          std::vector<std::string>({"product", "has-slot:vendor"}));
    CHECK(unknown_host.task_calls.empty());

    ScriptedUpdateDevice invalid_device;
    invalid_device.variables = {
        {"product", "atlas"},
        {"has-slot:vendor", "maybe"},
    };
    UpdateExecutorOptions options;
    options.known_partitions = {"vendor"};
    auto invalid = execute_prepared_update(prepared, invalid_device, options);
    CHECK(!invalid);
    CHECK(invalid.error().kind == UpdateExecutionErrorKind::RequirementNotMet);
    CHECK(invalid_device.getvar_calls ==
          std::vector<std::string>({"product", "has-slot:vendor"}));

    ScriptedUpdateDevice valid;
    valid.variables = {
        {"product", "atlas"},
        {"has-slot:vendor", "yes"},
    };
    auto accepted = execute_prepared_update(prepared, valid, options);
    CHECK(accepted);
    CHECK(accepted->validated_requirements == 1U);
}

void missing_duplicate_and_extra_artifacts_fail_before_getvar() {
    std::vector<PlannedRequirement> requirements;
    requirements.push_back(requirement("product", {"atlas"}, 50U));
    std::vector<PlannedUpdateTask> tasks;
    tasks.push_back(flash_task("boot", "boot.img", 51U));

    auto missing = make_package(requirements, tasks, {});
    ScriptedUpdateDevice missing_device;
    auto missing_result = execute_prepared_update(missing, missing_device);
    CHECK(!missing_result);
    CHECK(missing_result.error().kind ==
          UpdateExecutionErrorKind::InvalidPreparedPackage);
    CHECK(missing_result.error().task_index == 0U);
    CHECK(missing_device.getvar_calls.empty());
    CHECK(missing_device.task_calls.empty());

    std::vector<PreparedUpdateArtifact> duplicates;
    duplicates.push_back(make_artifact("boot.img", "one"));
    duplicates.push_back(make_artifact("boot.img", "two"));
    auto duplicate = make_package(requirements, tasks, std::move(duplicates));
    ScriptedUpdateDevice duplicate_device;
    auto duplicate_result = execute_prepared_update(duplicate, duplicate_device);
    CHECK(!duplicate_result);
    CHECK(duplicate_result.error().kind ==
          UpdateExecutionErrorKind::InvalidPreparedPackage);
    CHECK(duplicate_device.getvar_calls.empty());
    CHECK(duplicate_device.task_calls.empty());

    std::vector<PreparedUpdateArtifact> extras;
    extras.push_back(make_artifact("boot.img"));
    extras.push_back(make_artifact("unused.img"));
    auto extra = make_package(requirements, tasks, std::move(extras));
    ScriptedUpdateDevice extra_device;
    auto extra_result = execute_prepared_update(extra, extra_device);
    CHECK(!extra_result);
    CHECK(extra_result.error().name == "unused.img");
    CHECK(extra_device.getvar_calls.empty());
    CHECK(extra_device.task_calls.empty());
}

void task_failure_stops_and_reports_completed_prefix() {
    std::vector<PlannedUpdateTask> tasks;
    tasks.push_back(erase_task("cache", 60U));
    tasks.push_back(reboot_task(PlannedRebootTarget::Bootloader, 61U));
    tasks.push_back(update_super_task(62U));
    auto prepared = make_package({}, std::move(tasks), {});
    ScriptedUpdateDevice device;
    device.fail_task_index = 1U;

    auto result = execute_prepared_update(prepared, device);
    CHECK(!result);
    CHECK(result.error().kind == UpdateExecutionErrorKind::DeviceTaskFailed);
    CHECK(result.error().task_index == 1U);
    CHECK(result.error().location == location(61U, UpdateManifestKind::FastbootInfo));
    CHECK(result.error().completed_tasks == 1U);
    CHECK(device.task_calls.size() == 2U);
    CHECK(result.error().trace.back().kind == UpdateExecutionEventKind::TaskFailed);
}

void cancellation_before_and_during_tasks_is_truthful() {
    std::vector<PlannedUpdateTask> tasks;
    tasks.push_back(erase_task("cache", 70U));
    tasks.push_back(update_super_task(71U));
    auto prepared = make_package({}, std::move(tasks), {});

    std::stop_source before;
    before.request_stop();
    ScriptedUpdateDevice untouched;
    auto cancelled_before =
        execute_prepared_update(prepared, untouched, {}, before.get_token());
    CHECK(!cancelled_before);
    CHECK(cancelled_before.error().kind == UpdateExecutionErrorKind::Cancelled);
    CHECK(cancelled_before.error().trace.empty());
    CHECK(untouched.task_calls.empty());

    std::stop_source during;
    ScriptedUpdateDevice device;
    device.cancel_task_index = 0U;
    device.cancellation_source = &during;
    auto cancelled_during =
        execute_prepared_update(prepared, device, {}, during.get_token());
    CHECK(!cancelled_during);
    CHECK(cancelled_during.error().kind == UpdateExecutionErrorKind::Cancelled);
    CHECK(cancelled_during.error().task_index == 0U);
    CHECK(cancelled_during.error().completed_tasks == 1U);
    CHECK(device.task_calls.size() == 1U);

    std::stop_source during_preparation;
    ScriptedUpdateDevice preparation_device;
    preparation_device.cancel_prepare_index = 1U;
    preparation_device.cancellation_source = &during_preparation;
    auto cancelled_preparation = execute_prepared_update(
        prepared, preparation_device, {}, during_preparation.get_token());
    CHECK(!cancelled_preparation);
    CHECK(cancelled_preparation.error().kind ==
          UpdateExecutionErrorKind::Cancelled);
    CHECK(cancelled_preparation.error().task_index == 1U);
    CHECK(preparation_device.prepare_calls.size() == 2U);
    CHECK(preparation_device.task_calls.empty());
}

void observer_exceptions_stop_before_the_next_destructive_call() {
    std::vector<PlannedUpdateTask> tasks;
    tasks.push_back(erase_task("cache", 80U));
    tasks.push_back(update_super_task(81U));
    auto prepared = make_package({}, std::move(tasks), {});

    ScriptedUpdateDevice validation_device;
    UpdateExecutorOptions validation_options;
    validation_options.observer = [](const auto& item) {
        if (item.kind == UpdateExecutionEventKind::ValidationCompleted) {
            throw std::runtime_error("observer validation failure");
        }
    };
    auto validation =
        execute_prepared_update(prepared, validation_device, validation_options);
    CHECK(!validation);
    CHECK(validation.error().kind == UpdateExecutionErrorKind::ObserverFailed);
    CHECK(validation_device.task_calls.empty());

    ScriptedUpdateDevice task_device;
    UpdateExecutorOptions task_options;
    task_options.observer = [](const auto& item) {
        if (item.kind == UpdateExecutionEventKind::TaskCompleted) {
            throw std::runtime_error("observer task failure");
        }
    };
    auto task = execute_prepared_update(prepared, task_device, task_options);
    CHECK(!task);
    CHECK(task.error().kind == UpdateExecutionErrorKind::ObserverFailed);
    CHECK(task.error().task_index == 0U);
    CHECK(task.error().completed_tasks == 1U);
    CHECK(task_device.task_calls.size() == 1U);
}

void later_task_preparation_failure_keeps_every_task_non_destructive() {
    std::vector<PlannedUpdateTask> tasks;
    tasks.push_back(erase_task("cache", 85U));
    tasks.push_back(reboot_task(PlannedRebootTarget::Fastboot, 86U));
    tasks.push_back(update_super_task(87U));
    auto prepared = make_package({}, std::move(tasks), {});

    ScriptedUpdateDevice device;
    device.fail_prepare_index = 1U;
    device.prepare_error = UpdateDeviceError{
        .phase = ProtocolPhase::Validation,
        .message = "device does not support reboot-fastboot",
        .device_message = "unsupported capability",
        .native_code = 95,
    };
    auto result = execute_prepared_update(prepared, device);
    CHECK(!result);
    CHECK(result.error().kind == UpdateExecutionErrorKind::DeviceTaskFailed);
    CHECK(result.error().task_index == 1U);
    CHECK(result.error().completed_tasks == 0U);
    CHECK(result.error().device_error);
    CHECK(result.error().device_error->device_message == "unsupported capability");
    CHECK(device.prepare_calls.size() == 2U);
    CHECK(device.task_calls.empty());
}

void invalid_task_enums_fail_before_device_preparation() {
    auto invalid_kind_task = erase_task("cache", 88U);
    invalid_kind_task.kind = static_cast<UpdateTaskKind>(0xffU);
    auto invalid_kind = make_package({}, {invalid_kind_task}, {});
    ScriptedUpdateDevice kind_device;
    auto kind_result = execute_prepared_update(invalid_kind, kind_device);
    CHECK(!kind_result);
    CHECK(kind_result.error().kind ==
          UpdateExecutionErrorKind::InvalidPreparedPackage);
    CHECK(kind_device.prepare_calls.empty());
    CHECK(kind_device.task_calls.empty());

    auto invalid_slot_task = flash_task("boot", "boot.img", 89U);
    invalid_slot_task.slot = static_cast<PlannedSlot>(0xffU);
    std::vector<PreparedUpdateArtifact> slot_artifacts;
    slot_artifacts.push_back(make_artifact("boot.img"));
    auto invalid_slot =
        make_package({}, {invalid_slot_task}, std::move(slot_artifacts));
    ScriptedUpdateDevice slot_device;
    auto slot_result = execute_prepared_update(invalid_slot, slot_device);
    CHECK(!slot_result);
    CHECK(slot_result.error().kind ==
          UpdateExecutionErrorKind::InvalidPreparedPackage);
    CHECK(slot_device.prepare_calls.empty());
    CHECK(slot_device.task_calls.empty());

    auto invalid_reboot_task = reboot_task(PlannedRebootTarget::System, 90U);
    invalid_reboot_task.reboot_target =
        static_cast<PlannedRebootTarget>(0xffU);
    auto invalid_reboot = make_package({}, {invalid_reboot_task}, {});
    ScriptedUpdateDevice reboot_device;
    auto reboot_result = execute_prepared_update(invalid_reboot, reboot_device);
    CHECK(!reboot_result);
    CHECK(reboot_result.error().kind ==
          UpdateExecutionErrorKind::InvalidPreparedPackage);
    CHECK(reboot_device.prepare_calls.empty());
    CHECK(reboot_device.task_calls.empty());
}

void prepared_super_binding_reaches_only_the_update_super_token() {
    auto prepared = make_package({}, {erase_task("cache", 91U),
                                      update_super_task(92U)}, {});
    auto source = std::make_shared<MemorySource>("dedicated-super");
    UpdateExecutorOptions options;
    options.super_artifact = UpdateSuperArtifactInput{
        .logical_name = "super.img",
        .source = source,
    };
    ScriptedUpdateDevice device;
    auto result = execute_prepared_update(prepared, device, options);
    CHECK(result);
    CHECK(result->completed_tasks == 2U);
    CHECK(device.last_super_source == source);
    CHECK(device.prepare_calls == device.task_calls);
}

void one_absolute_deadline_covers_getvar_prepare_and_execute() {
    auto prepared = make_package(
        {requirement("product", {"atlas"}, 93U)},
        {erase_task("cache", 94U)}, {});
    ScriptedUpdateDevice device;
    device.variables = {{"product", "atlas"}};
    UpdateExecutorOptions options;
    options.deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
    auto result = execute_prepared_update(prepared, device, options);
    CHECK(result);
    CHECK(device.observed_deadlines.size() == 3U);
    CHECK(std::all_of(device.observed_deadlines.begin(),
                      device.observed_deadlines.end(), [&](const auto& observed) {
                          return observed == options.deadline;
                      }));

    ScriptedUpdateDevice expired_device;
    UpdateExecutorOptions expired_options;
    expired_options.deadline = std::chrono::steady_clock::now();
    auto expired = execute_prepared_update(prepared, expired_device,
                                           expired_options);
    CHECK(!expired);
    CHECK(expired.error().kind == UpdateExecutionErrorKind::TimedOut);
    CHECK(expired_device.getvar_calls.empty());
    CHECK(expired_device.prepare_calls.empty());
    CHECK(expired_device.task_calls.empty());
}

void task_started_observer_cancel_and_deadline_are_rechecked() {
    auto prepared = make_package({}, {erase_task("cache", 95U)}, {});

    std::stop_source cancellation;
    ScriptedUpdateDevice cancelled_device;
    UpdateExecutorOptions cancel_options;
    cancel_options.observer = [&](const auto& item) {
        if (item.kind == UpdateExecutionEventKind::TaskStarted) {
            cancellation.request_stop();
        }
    };
    auto cancelled = execute_prepared_update(
        prepared, cancelled_device, cancel_options, cancellation.get_token());
    CHECK(!cancelled);
    CHECK(cancelled.error().kind == UpdateExecutionErrorKind::Cancelled);
    CHECK(cancelled.error().task_index == 0U);
    CHECK(cancelled_device.prepare_calls.size() == 1U);
    CHECK(cancelled_device.task_calls.empty());

    ScriptedUpdateDevice timed_device;
    UpdateExecutorOptions timeout_options;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
    timeout_options.deadline = deadline;
    timeout_options.observer = [deadline](const auto& item) {
        if (item.kind == UpdateExecutionEventKind::TaskStarted) {
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        }
    };
    auto timed = execute_prepared_update(prepared, timed_device, timeout_options);
    CHECK(!timed);
    CHECK(timed.error().kind == UpdateExecutionErrorKind::TimedOut);
    CHECK(timed.error().task_index == 0U);
    CHECK(timed_device.prepare_calls.size() == 1U);
    CHECK(timed_device.task_calls.empty());
}

void device_error_fidelity_survives_task_failed_observer_exception() {
    auto prepared = make_package({}, {erase_task("cache", 96U)}, {});
    ScriptedUpdateDevice device;
    device.fail_task_index = 0U;
    device.task_error = UpdateDeviceError{
        .kind = UpdateDeviceErrorKind::TimedOut,
        .phase = ProtocolPhase::DataWrite,
        .message = "transport deadline expired",
        .device_message = "remote terminal detail",
        .informational = {
            Response{.kind = ResponseKind::Info, .payload = "progress"},
            Response{.kind = ResponseKind::Text, .payload = "diagnostic"},
        },
        .transport_status = TransportStatus::Timeout,
        .transport_certainty = TransferCertainty::PartialOrUnknown,
        .outbound_certainty = TransferCertainty::PartialOrUnknown,
        .inbound_expected = 4096U,
        .inbound_transferred = 17U,
        .inbound_certainty = TransferCertainty::PartialOrUnknown,
        .session_poisoned = true,
        .session_closed = true,
        .native_code = 110,
    };
    UpdateExecutorOptions options;
    options.observer = [](const auto& item) {
        if (item.kind == UpdateExecutionEventKind::TaskFailed) {
            throw std::runtime_error("observer secondary failure");
        }
    };
    auto result = execute_prepared_update(prepared, device, options);
    CHECK(!result);
    CHECK(result.error().kind == UpdateExecutionErrorKind::TimedOut);
    CHECK(result.error().task_index == 0U);
    CHECK(result.error().device_error);
    const auto& error = *result.error().device_error;
    CHECK(error.kind == UpdateDeviceErrorKind::TimedOut);
    CHECK(error.phase == ProtocolPhase::DataWrite);
    CHECK(error.message == "transport deadline expired");
    CHECK(error.device_message == "remote terminal detail");
    CHECK(error.informational.size() == 2U);
    CHECK(error.informational[0].kind == ResponseKind::Info);
    CHECK(error.informational[1].kind == ResponseKind::Text);
    CHECK(error.transport_status == TransportStatus::Timeout);
    CHECK(error.transport_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(error.outbound_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(error.inbound_expected == 4096U);
    CHECK(error.inbound_transferred == 17U);
    CHECK(error.inbound_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(error.session_poisoned);
    CHECK(error.session_closed);
    CHECK(error.native_code == 110);
    CHECK(result.error().secondary_observer_error);
    CHECK(result.error().secondary_observer_error->find("observer secondary failure") !=
          std::string::npos);
    CHECK(result.error().trace.back().kind ==
          UpdateExecutionEventKind::TaskFailed);
}

void actor_exceptions_and_empty_plans_are_structured() {
    std::vector<PlannedUpdateTask> tasks;
    tasks.push_back(erase_task("cache", 90U));
    tasks.push_back(update_super_task(91U));
    auto prepared = make_package({}, std::move(tasks), {});
    ScriptedUpdateDevice throwing;
    throwing.throw_task_index = 0U;
    auto exception = execute_prepared_update(prepared, throwing);
    CHECK(!exception);
    CHECK(exception.error().kind == UpdateExecutionErrorKind::ActorException);
    CHECK(exception.error().task_index == 0U);
    CHECK(exception.error().completed_tasks == 0U);
    CHECK(throwing.task_calls.size() == 1U);

    auto empty = make_package({}, {}, {});
    ScriptedUpdateDevice empty_device;
    auto empty_result = execute_prepared_update(empty, empty_device);
    CHECK(empty_result);
    CHECK(empty_result->validated_requirements == 0U);
    CHECK(empty_result->completed_tasks == 0U);
    CHECK(empty_device.getvar_calls.empty());
    CHECK(empty_device.task_calls.empty());
    CHECK(empty_result->trace.size() == 4U);
    CHECK(empty_result->trace[0].kind == UpdateExecutionEventKind::ValidationStarted);
    CHECK(empty_result->trace[1].kind ==
          UpdateExecutionEventKind::PreparedPackageValidated);
    CHECK(empty_result->trace[2].kind == UpdateExecutionEventKind::ValidationCompleted);
    CHECK(empty_result->trace[3].kind == UpdateExecutionEventKind::ExecutionCompleted);
}

struct Test final {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main() {
    const std::array tests{
        Test{"requirements then ordered tasks",
             requirements_are_checked_once_before_ordered_tasks},
        Test{"requirement failures are non-destructive",
             requirement_and_product_failures_make_zero_destructive_calls},
        Test{"partition-exists semantics",
             partition_exists_uses_host_table_and_has_slot_getvar},
        Test{"prepared artifact mapping",
             missing_duplicate_and_extra_artifacts_fail_before_getvar},
        Test{"task failure stops the suffix",
             task_failure_stops_and_reports_completed_prefix},
        Test{"task cancellation", cancellation_before_and_during_tasks_is_truthful},
        Test{"observer exceptions",
             observer_exceptions_stop_before_the_next_destructive_call},
        Test{"prepare all before execute",
             later_task_preparation_failure_keeps_every_task_non_destructive},
        Test{"invalid task enums",
             invalid_task_enums_fail_before_device_preparation},
        Test{"dedicated super binding",
             prepared_super_binding_reaches_only_the_update_super_token},
        Test{"absolute deadline propagation",
             one_absolute_deadline_covers_getvar_prepare_and_execute},
        Test{"TaskStarted cancellation and deadline",
             task_started_observer_cancel_and_deadline_are_rechecked},
        Test{"device error fidelity and observer secondary",
             device_error_fidelity_survives_task_failed_observer_exception},
        Test{"actor exceptions and empty plan",
             actor_exceptions_and_empty_plans_are_structured},
    };

    std::size_t failures = 0;
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
