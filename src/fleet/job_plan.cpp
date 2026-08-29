// SPDX-License-Identifier: MIT
#include "src/fleet/job_plan.hpp"

#include "src/fleet/canonical_json.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace kairosboot::fleet {
namespace {

inline constexpr std::size_t kIdentifierBytes = 256U;
inline constexpr std::uint64_t kMinimumMemoryBudget = 1024U * 1024U;
inline constexpr std::uint64_t kMaximumMemoryBudget =
    2ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaximumParallelDevices = 256U;

[[nodiscard]] JobPlanError invalid_manifest() noexcept {
    return {JobPlanErrorKind::InvalidManifest, 0U};
}

[[nodiscard]] JobPlanError canonical_error(
    const CanonicalJsonError& error) noexcept {
    switch (error.kind) {
        case CanonicalJsonErrorKind::InvalidUtf8:
            return {JobPlanErrorKind::InvalidUtf8, error.input_byte_offset};
        case CanonicalJsonErrorKind::IntegerOutOfRange:
            return {JobPlanErrorKind::IntegerOutOfRange, 0U};
    }
    return {JobPlanErrorKind::UnexpectedFailure, 0U};
}

void invoke_fault(const JobPlanBuildOptions& options,
                  const JobPlanFaultPoint point) {
    if (options.fault_hook != nullptr) {
        options.fault_hook(point, options.fault_context);
    }
}

[[nodiscard]] bool bounded_nonempty(const std::string_view value,
                                    const std::size_t maximum) noexcept {
    return !value.empty() && value.size() <= maximum;
}

[[nodiscard]] bool canonical_sha256(const std::string_view value) noexcept {
    if (value.size() != 64U) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

void validate_string_encoding(const std::string_view value,
                              std::optional<JobPlanError>& error) noexcept {
    if (error.has_value()) {
        return;
    }
    const auto validated = validate_canonical_json_utf8(value);
    if (!validated) {
        error = canonical_error(validated.error());
    }
}

[[nodiscard]] std::expected<void, JobPlanError> validate_manifest_encoding(
    const FlashJobManifest& manifest) noexcept {
    std::optional<JobPlanError> error;
    for (const auto& artifact : manifest.artifacts) {
        validate_string_encoding(artifact.id.value, error);
        validate_string_encoding(artifact.path.value, error);
        validate_string_encoding(artifact.sha256.value, error);
    }
    for (const auto& target : manifest.targets) {
        validate_string_encoding(target.name.value, error);
        for (const auto& value : target.selector.serials) {
            validate_string_encoding(value.value, error);
        }
        for (const auto& value : target.selector.usb_paths) {
            validate_string_encoding(value.value, error);
        }
        validate_string_encoding(target.expected_product.value, error);
        for (const auto& step : target.steps) {
            if (const auto* flash =
                    std::get_if<ManifestFlashStep>(&step.payload)) {
                validate_string_encoding(flash->artifact.value, error);
                validate_string_encoding(flash->partition.value, error);
            } else if (const auto* erase =
                           std::get_if<ManifestEraseStep>(&step.payload)) {
                validate_string_encoding(erase->partition.value, error);
            } else if (const auto* oem =
                           std::get_if<ManifestOemStep>(&step.payload)) {
                validate_string_encoding(oem->command.value, error);
            }
        }
    }
    if (error.has_value()) {
        return std::unexpected(*error);
    }
    return {};
}

[[nodiscard]] std::optional<std::string_view> flash_slot_name(
    const ManifestFlashSlot value) noexcept {
    switch (value) {
        case ManifestFlashSlot::Current:
            return "current";
        case ManifestFlashSlot::Other:
            return "other";
        case ManifestFlashSlot::All:
            return "all";
        case ManifestFlashSlot::A:
            return "a";
        case ManifestFlashSlot::B:
            return "b";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> active_slot_name(
    const ManifestActiveSlot value) noexcept {
    switch (value) {
        case ManifestActiveSlot::A:
            return "a";
        case ManifestActiveSlot::B:
            return "b";
        case ManifestActiveSlot::Other:
            return "other";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string_view> reboot_target_name(
    const ManifestRebootTarget value) noexcept {
    switch (value) {
        case ManifestRebootTarget::System:
            return "system";
        case ManifestRebootTarget::Bootloader:
            return "bootloader";
        case ManifestRebootTarget::Recovery:
            return "recovery";
        case ManifestRebootTarget::Fastboot:
            return "fastboot";
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_step_shape(const ManifestStep& step) noexcept {
    if (step.payload.valueless_by_exception()) {
        return false;
    }
    if (const auto* flash = std::get_if<ManifestFlashStep>(&step.payload)) {
        return bounded_nonempty(flash->partition.value,
                                kMaximumManifestScalarBytes) &&
            bounded_nonempty(flash->artifact.value,
                             kMaximumManifestScalarBytes) &&
            (!flash->slot.has_value() ||
             flash_slot_name(*flash->slot).has_value());
    }
    if (const auto* erase = std::get_if<ManifestEraseStep>(&step.payload)) {
        return bounded_nonempty(erase->partition.value,
                                kMaximumManifestScalarBytes);
    }
    if (const auto* active =
            std::get_if<ManifestSetActiveStep>(&step.payload)) {
        return active_slot_name(active->slot).has_value();
    }
    if (const auto* reboot = std::get_if<ManifestRebootStep>(&step.payload)) {
        return reboot_target_name(reboot->target).has_value();
    }
    const auto* oem = std::get_if<ManifestOemStep>(&step.payload);
    return oem != nullptr &&
        bounded_nonempty(oem->command.value, kMaximumManifestScalarBytes);
}

[[nodiscard]] bool valid_manifest_shape(
    const FlashJobManifest& manifest) noexcept {
    if (manifest.artifacts.empty() ||
        manifest.artifacts.size() > kMaximumManifestArtifacts ||
        manifest.targets.empty() ||
        manifest.targets.size() > kMaximumManifestTargets) {
        return false;
    }
    for (const auto& artifact : manifest.artifacts) {
        if (!bounded_nonempty(artifact.id.value, kIdentifierBytes) ||
            !bounded_nonempty(artifact.path.value,
                              kMaximumManifestScalarBytes) ||
            !canonical_sha256(artifact.sha256.value)) {
            return false;
        }
    }
    for (const auto& target : manifest.targets) {
        if (!bounded_nonempty(target.name.value, kIdentifierBytes) ||
            !bounded_nonempty(target.expected_product.value,
                              kMaximumManifestScalarBytes) ||
            (target.selector.serials.empty() &&
             target.selector.usb_paths.empty()) ||
            target.selector.serials.size() >
                kMaximumManifestSelectorValues ||
            target.selector.usb_paths.size() >
                kMaximumManifestSelectorValues ||
            target.steps.empty() || target.steps.size() > kMaximumManifestSteps) {
            return false;
        }
        for (const auto& value : target.selector.serials) {
            if (!bounded_nonempty(value.value, kMaximumManifestScalarBytes)) {
                return false;
            }
        }
        for (const auto& value : target.selector.usb_paths) {
            if (!bounded_nonempty(value.value, kMaximumManifestScalarBytes)) {
                return false;
            }
        }
        for (const auto& step : target.steps) {
            if (!valid_step_shape(step)) {
                return false;
            }
        }
    }
    if (manifest.policy.max_parallel_devices == 0U ||
        manifest.policy.max_parallel_devices > kMaximumParallelDevices) {
        return false;
    }
    if (manifest.policy.memory_budget.automatic) {
        if (manifest.policy.memory_budget.bytes != 0U) {
            return false;
        }
    } else if (manifest.policy.memory_budget.bytes < kMinimumMemoryBudget ||
               manifest.policy.memory_budget.bytes > kMaximumMemoryBudget) {
        return false;
    }
    switch (manifest.policy.on_device_failure) {
        case ManifestDeviceFailurePolicy::Continue:
        case ManifestDeviceFailurePolicy::Stop:
            return true;
    }
    return false;
}

class PlanJsonWriter final {
public:
    void raw(const std::string_view value) {
        if (!error_.has_value()) {
            output_.append(value);
        }
    }

    void quoted(const std::string_view value) {
        if (error_.has_value()) {
            return;
        }
        auto appended = append_canonical_json_quoted_string(output_, value);
        if (!appended) {
            error_ = canonical_error(appended.error());
        }
    }

    void unsigned_integer(const std::uint64_t value) {
        if (error_.has_value()) {
            return;
        }
        auto appended = append_canonical_json_unsigned_integer(output_, value);
        if (!appended) {
            error_ = canonical_error(appended.error());
        }
    }

    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }

    [[nodiscard]] std::expected<std::string, JobPlanError> finish() && {
        if (error_.has_value()) {
            return std::unexpected(*error_);
        }
        return std::move(output_);
    }

private:
    std::string output_;
    std::optional<JobPlanError> error_;
};

void append_string_array(PlanJsonWriter& writer,
                         const std::vector<LocatedManifestString>& values) {
    writer.raw("[");
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            writer.raw(",");
        }
        writer.quoted(values[index].value);
    }
    writer.raw("]");
}

void append_nullable_string(PlanJsonWriter& writer,
                            const std::optional<std::string_view> value) {
    if (value.has_value()) {
        writer.quoted(*value);
    } else {
        writer.raw("null");
    }
}

[[nodiscard]] bool append_step(PlanJsonWriter& writer,
                               const ManifestStep& step,
                               const std::size_t index) {
    writer.raw("{\"artifact\":");
    if (const auto* flash = std::get_if<ManifestFlashStep>(&step.payload)) {
        writer.quoted(flash->artifact.value);
        writer.raw(",\"index\":");
        writer.unsigned_integer(index);
        writer.raw(",\"oemCommand\":null,\"operation\":\"flash\","
                   "\"partition\":");
        writer.quoted(flash->partition.value);
        writer.raw(",\"rebootTarget\":null,\"slot\":");
        append_nullable_string(
            writer,
            flash->slot.has_value() ? flash_slot_name(*flash->slot)
                                    : std::nullopt);
        writer.raw("}");
        return !writer.failed();
    }
    writer.raw("null,\"index\":");
    writer.unsigned_integer(index);
    writer.raw(",\"oemCommand\":");
    if (const auto* oem = std::get_if<ManifestOemStep>(&step.payload)) {
        writer.quoted(oem->command.value);
        writer.raw(",\"operation\":\"oem\",\"partition\":null,"
                   "\"rebootTarget\":null,\"slot\":null}");
        return !writer.failed();
    }
    writer.raw("null,\"operation\":");
    if (const auto* erase = std::get_if<ManifestEraseStep>(&step.payload)) {
        writer.raw("\"erase\",\"partition\":");
        writer.quoted(erase->partition.value);
        writer.raw(",\"rebootTarget\":null,\"slot\":null}");
        return !writer.failed();
    }
    if (const auto* active =
            std::get_if<ManifestSetActiveStep>(&step.payload)) {
        writer.raw("\"set_active\",\"partition\":null,"
                   "\"rebootTarget\":null,\"slot\":");
        append_nullable_string(writer, active_slot_name(active->slot));
        writer.raw("}");
        return !writer.failed();
    }
    if (const auto* reboot = std::get_if<ManifestRebootStep>(&step.payload)) {
        writer.raw("\"reboot\",\"partition\":null,\"rebootTarget\":");
        append_nullable_string(writer, reboot_target_name(reboot->target));
        writer.raw(",\"slot\":null}");
        return !writer.failed();
    }
    return false;
}

[[nodiscard]] std::expected<std::string, JobPlanError>
serialize_manifest(const FlashJobManifest& manifest) {
    if (auto encoded = validate_manifest_encoding(manifest); !encoded) {
        return std::unexpected(encoded.error());
    }
    if (!valid_manifest_shape(manifest)) {
        return std::unexpected(invalid_manifest());
    }

    PlanJsonWriter writer;
    writer.raw("{\"artifacts\":[");
    for (std::size_t index = 0U; index < manifest.artifacts.size(); ++index) {
        if (index != 0U) {
            writer.raw(",");
        }
        const auto& artifact = manifest.artifacts[index];
        writer.raw("{\"id\":");
        writer.quoted(artifact.id.value);
        writer.raw(",\"index\":");
        writer.unsigned_integer(index);
        writer.raw(",\"path\":");
        writer.quoted(artifact.path.value);
        writer.raw(",\"sha256\":");
        writer.quoted(artifact.sha256.value);
        writer.raw("}");
    }
    writer.raw("],\"policy\":{\"maxParallelDevices\":");
    writer.unsigned_integer(manifest.policy.max_parallel_devices);
    writer.raw(",\"memoryBudget\":");
    if (manifest.policy.memory_budget.automatic) {
        writer.raw("\"auto\"");
    } else {
        writer.unsigned_integer(manifest.policy.memory_budget.bytes);
    }
    writer.raw(",\"onDeviceFailure\":");
    switch (manifest.policy.on_device_failure) {
        case ManifestDeviceFailurePolicy::Continue:
            writer.raw("\"continue\"");
            break;
        case ManifestDeviceFailurePolicy::Stop:
            writer.raw("\"stop\"");
            break;
    }
    writer.raw("},\"schemaVersion\":1,\"targets\":[");
    for (std::size_t target_index = 0U;
         target_index < manifest.targets.size();
         ++target_index) {
        if (target_index != 0U) {
            writer.raw(",");
        }
        const auto& target = manifest.targets[target_index];
        writer.raw("{\"expectedProduct\":");
        writer.quoted(target.expected_product.value);
        writer.raw(",\"index\":");
        writer.unsigned_integer(target_index);
        writer.raw(",\"name\":");
        writer.quoted(target.name.value);
        writer.raw(",\"selector\":{\"serials\":");
        append_string_array(writer, target.selector.serials);
        writer.raw(",\"usbPaths\":");
        append_string_array(writer, target.selector.usb_paths);
        writer.raw("},\"steps\":[");
        for (std::size_t step_index = 0U;
             step_index < target.steps.size();
             ++step_index) {
            if (step_index != 0U) {
                writer.raw(",");
            }
            if (!append_step(writer, target.steps[step_index], step_index)) {
                if (writer.failed()) {
                    return std::move(writer).finish();
                }
                return std::unexpected(invalid_manifest());
            }
        }
        writer.raw("]}");
    }
    writer.raw("]}");
    return std::move(writer).finish();
}

[[nodiscard]] image::Sha256Digest hash_plan(const std::string& json) {
    image::Sha256Accumulator accumulator;
    accumulator.update(std::as_bytes(
        std::span<const char>{json.data(), json.size()}));
    return accumulator.finish();
}

}  // namespace

JobPlan::JobPlan(FlashJobManifest&& manifest,
                 std::string&& canonical_json,
                 const image::Sha256Digest& sha256,
                 std::string&& sha256_hex) noexcept
    : manifest_(std::move(manifest)),
      canonical_json_(std::move(canonical_json)),
      sha256_(sha256),
      sha256_hex_(std::move(sha256_hex)) {}

const FlashJobManifest& JobPlan::manifest() const noexcept { return manifest_; }

std::string_view JobPlan::canonical_json() const noexcept {
    return canonical_json_;
}

const image::Sha256Digest& JobPlan::sha256() const noexcept { return sha256_; }

std::string_view JobPlan::sha256_hex() const noexcept { return sha256_hex_; }

std::expected<JobPlan, JobPlanError> make_job_plan(
    FlashJobManifest&& manifest,
    const JobPlanBuildOptions& options) noexcept {
    try {
        invoke_fault(options, JobPlanFaultPoint::BeforeSerialization);
        auto canonical_json = serialize_manifest(manifest);
        if (!canonical_json) {
            return std::unexpected(canonical_json.error());
        }
        const auto plan_sha256 = hash_plan(*canonical_json);
        auto plan_sha256_hex = image::sha256_hex(plan_sha256);
        invoke_fault(options, JobPlanFaultPoint::BeforeSnapshotCommit);
        return JobPlan{std::move(manifest),
                       std::move(*canonical_json),
                       plan_sha256,
                       std::move(plan_sha256_hex)};
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            JobPlanError{JobPlanErrorKind::ResourceExhausted, 0U});
    } catch (const std::length_error&) {
        return std::unexpected(
            JobPlanError{JobPlanErrorKind::OutputTooLarge, 0U});
    } catch (...) {
        return std::unexpected(
            JobPlanError{JobPlanErrorKind::UnexpectedFailure, 0U});
    }
}

}  // namespace kairosboot::fleet
