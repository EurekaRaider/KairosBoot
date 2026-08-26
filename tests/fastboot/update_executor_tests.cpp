// SPDX-License-Identifier: MIT
#include "src/fastboot/update_executor.hpp"

#include <algorithm>
#include <array>
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
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::execute_prepared_update;
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
using kairosboot::fastboot::UpdateExecutionErrorKind;
using kairosboot::fastboot::UpdateExecutionEventKind;
using kairosboot::fastboot::UpdateExecutorOptions;
using kairosboot::fastboot::UpdateManifestKind;
using kairosboot::fastboot::UpdateSourceLocation;
using kairosboot::fastboot::UpdateTaskKind;
using kairosboot::image::ArtifactSourceOrigin;
using kairosboot::image::FlashArtifact;
using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::ResolvedArtifact;

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
    getvar(const std::string_view name, const std::stop_token) override {
        getvar_calls.emplace_back(name);
        if (throw_getvar == name) {
            throw std::runtime_error("scripted getvar exception");
        }
        if (fail_getvar == name) {
            return std::unexpected(UpdateDeviceError{
                .native_code = 17,
                .message = "scripted getvar failure",
            });
        }
        const auto found = variables.find(name);
        if (found == variables.end()) {
            return std::unexpected(UpdateDeviceError{
                .native_code = 2,
                .message = "missing scripted variable",
            });
        }
        return found->second;
    }

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    flash(const PlannedUpdateTask& task, const PreparedUpdateArtifact& artifact,
          const std::stop_token cancellation) override {
        CHECK(task.kind == UpdateTaskKind::Flash);
        CHECK(task.artifact == artifact.name);
        last_flash_artifact = &artifact;
        return destructive(
            "flash:" + task.partition + ":" + task.artifact +
                (task.slot == PlannedSlot::Other ? ":slot-other" : ":default") +
                (task.apply_vbmeta ? ":apply-vbmeta" : ":no-vbmeta"),
            cancellation);
    }

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    erase(const PlannedUpdateTask& task, const std::stop_token cancellation) override {
        CHECK(task.kind == UpdateTaskKind::Erase);
        return destructive("erase:" + task.partition, cancellation);
    }

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    reboot(const PlannedUpdateTask& task, const std::stop_token cancellation) override {
        CHECK(task.kind == UpdateTaskKind::Reboot);
        return destructive(
            "reboot:" + std::to_string(static_cast<unsigned>(task.reboot_target)),
            cancellation);
    }

    [[nodiscard]] std::expected<void, UpdateDeviceError>
    update_super(const PlannedUpdateTask& task,
                 const std::stop_token cancellation) override {
        CHECK(task.kind == UpdateTaskKind::UpdateSuper);
        return destructive("update-super", cancellation);
    }

    std::map<std::string, std::string, std::less<>> variables;
    std::vector<std::string> getvar_calls;
    std::vector<std::string> task_calls;
    std::string fail_getvar;
    std::string throw_getvar;
    std::optional<std::size_t> fail_task_index;
    std::optional<std::size_t> cancel_task_index;
    std::optional<std::size_t> throw_task_index;
    std::stop_source* cancellation_source{};
    const PreparedUpdateArtifact* last_flash_artifact{};

private:
    [[nodiscard]] std::expected<void, UpdateDeviceError>
    destructive(std::string call, const std::stop_token cancellation) {
        const auto index = task_calls.size();
        task_calls.push_back(std::move(call));
        if (throw_task_index == index) {
            throw std::runtime_error("scripted task exception");
        }
        if (cancel_task_index == index) {
            CHECK(cancellation_source != nullptr);
            cancellation_source->request_stop();
        }
        if (fail_task_index == index) {
            return std::unexpected(UpdateDeviceError{
                .kind = cancellation.stop_requested() ? UpdateDeviceErrorKind::Cancelled
                                                      : UpdateDeviceErrorKind::Failed,
                .native_code = 31,
                .message = "scripted task failure",
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
    CHECK(device.last_flash_artifact == &prepared.artifacts[0]);
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
    CHECK(cancelled_during.error().task_index == 1U);
    CHECK(cancelled_during.error().completed_tasks == 1U);
    CHECK(device.task_calls.size() == 1U);
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
