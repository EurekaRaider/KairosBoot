// SPDX-License-Identifier: MIT
#include "src/fleet/job_report.hpp"

#include "src/fleet/canonical_json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace kairosboot::fleet {
namespace {

struct ReportData final {
    std::string job_id;
    std::string plan_sha256;
    ReportState state{ReportState::Running};
    std::string started_at;
    std::optional<std::string> finished_at;
    std::vector<ReportDeviceSnapshot> devices;
    ReportSummary summary;
    std::optional<ReportError> error;
};

struct MutableReportState final {
    ReportData report;
    bool cancellation_latched{false};
    std::optional<ReportError> cancellation_error;
};

static_assert(std::is_nothrow_move_assignable_v<MutableReportState>);

[[nodiscard]] JobReportError make_error(const JobReportErrorKind kind,
                                        std::string message) {
    return {kind, std::move(message)};
}

[[nodiscard]] std::expected<std::size_t, JobReportError> utf8_scalar_count(
    const std::string_view value) {
    const auto validation = validate_canonical_json_utf8(value);
    if (!validation) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidUtf8,
            "invalid UTF-8 at byte " +
                std::to_string(validation.error().input_byte_offset)));
    }
    std::size_t count = 0U;
    for (const unsigned char byte : value) {
        if ((byte & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::expected<void, JobReportError> validate_string(
    const std::string_view value,
    const std::size_t minimum,
    const std::size_t maximum,
    const std::string_view field) {
    const auto count = utf8_scalar_count(value);
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count < minimum || *count > maximum) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidArgument,
            std::string(field) + " length is outside the frozen contract"));
    }
    return {};
}

[[nodiscard]] std::expected<void, JobReportError> validate_optional_string(
    const std::optional<std::string>& value,
    const std::size_t maximum,
    const std::string_view field) {
    if (!value) {
        return {};
    }
    return validate_string(*value, 1U, maximum, field);
}

[[nodiscard]] bool is_leap_year(const unsigned int year) noexcept {
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

struct TimestampParts final {
    std::array<unsigned int, 6U> values{};
    std::string_view fraction;
};

[[nodiscard]] bool ascii_digits(const std::string_view value,
                                const std::size_t offset,
                                const std::size_t count) noexcept {
    if (offset > value.size() || count > value.size() - offset) {
        return false;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const auto byte = static_cast<unsigned char>(value[offset + index]);
        if (byte < static_cast<unsigned char>('0') ||
            byte > static_cast<unsigned char>('9')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] unsigned int decimal(const std::string_view value,
                                   const std::size_t offset,
                                   const std::size_t count) noexcept {
    unsigned int result = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        result = result * 10U +
                 static_cast<unsigned int>(value[offset + index] - '0');
    }
    return result;
}

[[nodiscard]] std::expected<TimestampParts, JobReportError> parse_timestamp(
    const std::string_view value) {
    if (value.size() < 20U || value.back() != 'Z' || value[4] != '-' ||
        value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
        value[16] != ':' || !ascii_digits(value, 0U, 4U) ||
        !ascii_digits(value, 5U, 2U) || !ascii_digits(value, 8U, 2U) ||
        !ascii_digits(value, 11U, 2U) || !ascii_digits(value, 14U, 2U) ||
        !ascii_digits(value, 17U, 2U)) {
        return std::unexpected(make_error(JobReportErrorKind::InvalidTimestamp,
                                          "timestamp is not UTC RFC 3339"));
    }
    if (value.size() != 20U) {
        if (value[19] != '.' || value.size() == 21U ||
            !ascii_digits(value, 20U, value.size() - 21U)) {
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidTimestamp,
                "timestamp fractional seconds are invalid"));
        }
    }

    TimestampParts parts{
        .values = {decimal(value, 0U, 4U),
                   decimal(value, 5U, 2U),
                   decimal(value, 8U, 2U),
                   decimal(value, 11U, 2U),
                   decimal(value, 14U, 2U),
                   decimal(value, 17U, 2U)},
        .fraction = value.size() == 20U
                        ? std::string_view{}
                        : value.substr(20U, value.size() - 21U),
    };
    const auto year = parts.values[0];
    const auto month = parts.values[1];
    const auto day = parts.values[2];
    if (year == 0U || month == 0U || month > 12U || day == 0U ||
        parts.values[3] > 23U || parts.values[4] > 59U ||
        parts.values[5] > 59U) {
        return std::unexpected(make_error(JobReportErrorKind::InvalidTimestamp,
                                          "timestamp fields are out of range"));
    }
    constexpr std::array<unsigned int, 12U> days_in_month{
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    auto maximum_day = days_in_month[month - 1U];
    if (month == 2U && is_leap_year(year)) {
        maximum_day = 29U;
    }
    if (day > maximum_day) {
        return std::unexpected(make_error(JobReportErrorKind::InvalidTimestamp,
                                          "timestamp calendar date is invalid"));
    }
    return parts;
}

[[nodiscard]] std::expected<bool, JobReportError> timestamp_less_equal(
    const std::string_view left,
    const std::string_view right) {
    const auto left_parts = parse_timestamp(left);
    if (!left_parts) {
        return std::unexpected(left_parts.error());
    }
    const auto right_parts = parse_timestamp(right);
    if (!right_parts) {
        return std::unexpected(right_parts.error());
    }
    if (left_parts->values != right_parts->values) {
        return left_parts->values < right_parts->values;
    }
    const auto digits = std::max(left_parts->fraction.size(),
                                 right_parts->fraction.size());
    for (std::size_t index = 0U; index < digits; ++index) {
        const char left_digit = index < left_parts->fraction.size()
                                    ? left_parts->fraction[index]
                                    : '0';
        const char right_digit = index < right_parts->fraction.size()
                                     ? right_parts->fraction[index]
                                     : '0';
        if (left_digit != right_digit) {
            return left_digit < right_digit;
        }
    }
    return true;
}

[[nodiscard]] bool valid_status(const kb_status_t status) noexcept {
    return status >= KB_E_INVALID_ARGUMENT && status <= KB_E_DEVICE_FAIL;
}

[[nodiscard]] bool valid_transfer_state(
    const kb_transfer_state_t state) noexcept {
    return state == KB_TRANSFER_NOT_SENT ||
           state == KB_TRANSFER_PARTIAL_OR_UNKNOWN ||
           state == KB_TRANSFER_FULLY_TRANSFERRED;
}

[[nodiscard]] std::expected<void, JobReportError> validate_report_error(
    const ReportError& error) {
    if (!valid_status(error.code)) {
        return std::unexpected(make_error(JobReportErrorKind::InvalidArgument,
                                          "report error code is invalid"));
    }
    auto result = validate_string(error.message, 0U, 16384U, "error message");
    if (!result) {
        return result;
    }
    result = validate_optional_string(error.device_identifier,
                                      4096U,
                                      "error device identifier");
    if (!result) {
        return result;
    }
    if (error.transfer_certainty &&
        !valid_transfer_state(*error.transfer_certainty)) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidArgument,
            "report error transfer certainty is invalid"));
    }
    return {};
}

[[nodiscard]] std::expected<void, JobReportError> validate_step_spec(
    const ReportStepSpec& step) {
    if (static_cast<std::uint8_t>(step.operation) >
            static_cast<std::uint8_t>(ReportOperation::Oem) ||
        (step.slot &&
         static_cast<std::uint8_t>(*step.slot) >
             static_cast<std::uint8_t>(ReportSlot::B)) ||
        (step.reboot_target &&
         static_cast<std::uint8_t>(*step.reboot_target) >
             static_cast<std::uint8_t>(ReportRebootTarget::Fastboot))) {
        return std::unexpected(make_error(JobReportErrorKind::InvalidArgument,
                                          "step contains an unknown enum value"));
    }
    auto result = validate_optional_string(step.partition, 4096U, "partition");
    if (!result) {
        return result;
    }
    result = validate_optional_string(step.artifact, 4096U, "artifact");
    if (!result) {
        return result;
    }
    result = validate_optional_string(step.oem_command, 4096U, "OEM command");
    if (!result) {
        return result;
    }
    if (step.bytes_total &&
        *step.bytes_total >
            static_cast<std::uint64_t>(kCanonicalJsonMaximumSafeInteger)) {
        return std::unexpected(make_error(JobReportErrorKind::IntegerOutOfRange,
                                          "flash byte count is not I-JSON safe"));
    }
    switch (step.operation) {
        case ReportOperation::Flash:
            if (!step.partition || !step.artifact || step.reboot_target ||
                step.oem_command || !step.bytes_total) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidArgument,
                    "flash step fields do not match the frozen plan identity"));
            }
            break;
        case ReportOperation::Erase:
            if (!step.partition || step.artifact || step.slot ||
                step.reboot_target || step.oem_command || step.bytes_total) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidArgument,
                    "erase step fields do not match the frozen plan identity"));
            }
            break;
        case ReportOperation::SetActive:
            if (step.partition || step.artifact || !step.slot ||
                *step.slot == ReportSlot::Current ||
                *step.slot == ReportSlot::All || step.reboot_target ||
                step.oem_command || step.bytes_total) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidArgument,
                    "set-active step fields do not match the frozen plan identity"));
            }
            break;
        case ReportOperation::Reboot:
            if (step.partition || step.artifact || step.slot ||
                !step.reboot_target || step.oem_command || step.bytes_total) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidArgument,
                    "reboot step fields do not match the frozen plan identity"));
            }
            break;
        case ReportOperation::Oem:
            if (step.partition || step.artifact || step.slot ||
                step.reboot_target || !step.oem_command || step.bytes_total) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidArgument,
                    "OEM step fields do not match the frozen plan identity"));
            }
            break;
        default:
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidArgument,
                "step contains an unknown operation"));
    }
    return {};
}

[[nodiscard]] ReportSummary summarize(
    const std::vector<ReportDeviceSnapshot>& devices) noexcept {
    ReportSummary result{.total = static_cast<std::uint64_t>(devices.size())};
    for (const auto& device : devices) {
        switch (device.state) {
            case ReportWorkState::Pending:
                ++result.pending;
                break;
            case ReportWorkState::Running:
                ++result.running;
                break;
            case ReportWorkState::Succeeded:
                ++result.succeeded;
                break;
            case ReportWorkState::Failed:
                ++result.failed;
                break;
            case ReportWorkState::Cancelled:
                ++result.cancelled;
                break;
            case ReportWorkState::Skipped:
                ++result.skipped;
                break;
        }
    }
    return result;
}

[[nodiscard]] std::expected<void, JobReportError> require_order(
    const std::string_view earlier,
    const std::string_view later,
    const std::string_view message) {
    const auto ordered = timestamp_less_equal(earlier, later);
    if (!ordered) {
        return std::unexpected(ordered.error());
    }
    if (!*ordered) {
        return std::unexpected(
            make_error(JobReportErrorKind::TimestampOrder, std::string(message)));
    }
    return {};
}

[[nodiscard]] bool all_steps(const ReportDeviceSnapshot& device,
                             const ReportWorkState state) noexcept {
    return std::ranges::all_of(device.steps, [state](const auto& step) {
        return step.state == state;
    });
}

[[nodiscard]] std::expected<void, JobReportError> validate_report_data(
    const ReportData& report,
    const bool require_publishable_products = true) {
    auto result = validate_string(report.job_id, 1U, 256U, "jobId");
    if (!result) {
        return result;
    }
    if (report.plan_sha256.size() != 64U ||
        !std::ranges::all_of(report.plan_sha256, [](const char value) {
            return (value >= '0' && value <= '9') ||
                   (value >= 'a' && value <= 'f');
        })) {
        return std::unexpected(make_error(JobReportErrorKind::InvalidArgument,
                                          "planSha256 is invalid"));
    }
    const auto started = parse_timestamp(report.started_at);
    if (!started) {
        return std::unexpected(started.error());
    }
    if (report.finished_at) {
        result = require_order(report.started_at,
                               *report.finished_at,
                               "job finished before it started");
        if (!result) {
            return result;
        }
    }
    if (report.devices.size() >
        static_cast<std::size_t>(kCanonicalJsonMaximumSafeInteger)) {
        return std::unexpected(make_error(JobReportErrorKind::IntegerOutOfRange,
                                          "device count is not I-JSON safe"));
    }
    if (report.summary != summarize(report.devices)) {
        return std::unexpected(make_error(JobReportErrorKind::InvalidTransition,
                                          "summary does not match device states"));
    }

    std::unordered_set<std::string_view> identifiers;
    identifiers.reserve(report.devices.size());
    for (const auto& device : report.devices) {
        if (!identifiers.insert(device.identifier).second) {
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidArgument,
                "report contains duplicate device identifiers"));
        }
        result = validate_string(device.identifier,
                                 1U,
                                 4096U,
                                 "device identifier");
        if (!result) {
            return result;
        }
        result = validate_optional_string(device.serial, 4096U, "serial");
        if (!result) {
            return result;
        }
        result = validate_optional_string(device.usb_path, 4096U, "USB path");
        if (!result) {
            return result;
        }
        result = validate_string(device.target, 1U, 256U, "target");
        if (!result) {
            return result;
        }
        result = validate_string(device.expected_product,
                                 1U,
                                 4096U,
                                 "expected product");
        if (!result) {
            return result;
        }
        result = validate_optional_string(device.observed_product,
                                          4096U,
                                          "observed product");
        if (!result) {
            return result;
        }
        if (!device.serial && !device.usb_path) {
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidArgument,
                "report device has no plan selector identity"));
        }
        if (device.steps.empty() || device.steps.size() > 16384U) {
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidArgument,
                "device step count is outside the frozen contract"));
        }
        if (device.error) {
            result = validate_report_error(*device.error);
            if (!result) {
                return result;
            }
            if (!device.error->device_identifier ||
                *device.error->device_identifier != device.identifier) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidTransition,
                    "device error is scoped to another device"));
            }
        }

        std::optional<std::string_view> previous_finish;
        std::size_t running_steps = 0U;
        std::size_t failed_steps = 0U;
        std::size_t cancelled_steps = 0U;
        for (std::size_t index = 0U; index < device.steps.size(); ++index) {
            const auto& step = device.steps[index];
            if (step.index != index) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidTransition,
                    "report step index is not contiguous"));
            }
            result = validate_step_spec(step.spec);
            if (!result) {
                return result;
            }
            if (step.started_at) {
                result = require_order(report.started_at,
                                       *step.started_at,
                                       "step starts before the job");
                if (!result) {
                    return result;
                }
            }
            if (step.finished_at) {
                result = require_order(report.started_at,
                                       *step.finished_at,
                                       "step finishes before the job");
                if (!result) {
                    return result;
                }
                if (report.finished_at) {
                    result = require_order(*step.finished_at,
                                           *report.finished_at,
                                           "step finishes after the job");
                    if (!result) {
                        return result;
                    }
                }
            }
            if (step.started_at && step.finished_at) {
                result = require_order(*step.started_at,
                                       *step.finished_at,
                                       "step finished before it started");
                if (!result) {
                    return result;
                }
            }
            if (previous_finish) {
                const auto boundary = step.started_at
                                          ? std::optional<std::string_view>{
                                                *step.started_at}
                                          : step.finished_at
                                                ? std::optional<std::string_view>{
                                                      *step.finished_at}
                                                : std::nullopt;
                if (boundary) {
                    result = require_order(*previous_finish,
                                           *boundary,
                                           "device step timestamps overlap");
                    if (!result) {
                        return result;
                    }
                }
            }
            if (step.finished_at) {
                previous_finish = *step.finished_at;
            }

            if (step.spec.operation == ReportOperation::Flash) {
                if (!step.bytes_transferred ||
                    *step.bytes_transferred > *step.spec.bytes_total) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "flash byte counters are invalid"));
                }
            } else if (step.bytes_transferred) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidTransition,
                    "non-DATA step has byte counters"));
            }

            switch (step.state) {
                case ReportWorkState::Pending:
                    if (step.started_at || step.finished_at || step.error ||
                        step.skip_reason ||
                        (step.bytes_transferred &&
                         *step.bytes_transferred != 0U)) {
                        return std::unexpected(make_error(
                            JobReportErrorKind::InvalidTransition,
                            "pending step carries terminal data"));
                    }
                    break;
                case ReportWorkState::Running:
                    ++running_steps;
                    if (!step.started_at || step.finished_at || step.error ||
                        step.skip_reason) {
                        return std::unexpected(make_error(
                            JobReportErrorKind::InvalidTransition,
                            "running step fields are inconsistent"));
                    }
                    break;
                case ReportWorkState::Succeeded:
                    if (!step.started_at || !step.finished_at || step.error ||
                        step.skip_reason ||
                        (step.spec.operation == ReportOperation::Flash &&
                         *step.bytes_transferred != *step.spec.bytes_total)) {
                        return std::unexpected(make_error(
                            JobReportErrorKind::InvalidTransition,
                            "succeeded step fields are inconsistent"));
                    }
                    break;
                case ReportWorkState::Failed:
                    ++failed_steps;
                    if (!step.started_at || !step.finished_at || !step.error ||
                        step.error->code == KB_E_CANCELLED || step.skip_reason) {
                        return std::unexpected(make_error(
                            JobReportErrorKind::InvalidTransition,
                            "failed step fields are inconsistent"));
                    }
                    break;
                case ReportWorkState::Cancelled:
                    ++cancelled_steps;
                    if (!step.finished_at || !step.error ||
                        step.error->code != KB_E_CANCELLED || step.skip_reason) {
                        return std::unexpected(make_error(
                            JobReportErrorKind::InvalidTransition,
                            "cancelled step fields are inconsistent"));
                    }
                    break;
                case ReportWorkState::Skipped:
                    if (step.started_at || !step.finished_at || step.error ||
                        !step.skip_reason ||
                        (step.bytes_transferred &&
                         *step.bytes_transferred != 0U)) {
                        return std::unexpected(make_error(
                            JobReportErrorKind::InvalidTransition,
                            "skipped step fields are inconsistent"));
                    }
                    break;
            }
            if (step.error) {
                result = validate_report_error(*step.error);
                if (!result) {
                    return result;
                }
                if (!step.error->device_identifier ||
                    *step.error->device_identifier != device.identifier) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "step error is scoped to another device"));
                }
            }
        }

        const auto states_before = [&device](const std::size_t boundary,
                                             const ReportWorkState state) {
            return std::ranges::all_of(
                device.steps.begin(),
                device.steps.begin() + static_cast<std::ptrdiff_t>(boundary),
                [state](const auto& step) { return step.state == state; });
        };
        const auto states_after = [&device](const std::size_t boundary,
                                            const ReportWorkState state) {
            return std::ranges::all_of(
                device.steps.begin() + static_cast<std::ptrdiff_t>(boundary),
                device.steps.end(),
                [state](const auto& step) { return step.state == state; });
        };
        switch (device.state) {
            case ReportWorkState::Pending:
                if (!all_steps(device, ReportWorkState::Pending) || device.error) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "pending device has started work"));
                }
                break;
            case ReportWorkState::Running: {
                if (running_steps != 1U || failed_steps != 0U ||
                    cancelled_steps != 0U || device.error) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "running device has invalid active work"));
                }
                const auto active = static_cast<std::size_t>(std::distance(
                    device.steps.begin(),
                    std::ranges::find_if(device.steps, [](const auto& step) {
                        return step.state == ReportWorkState::Running;
                    })));
                if (!states_before(active, ReportWorkState::Succeeded) ||
                    !states_after(active + 1U, ReportWorkState::Pending)) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "running device steps are not serial"));
                }
                break;
            }
            case ReportWorkState::Succeeded:
                if (!all_steps(device, ReportWorkState::Succeeded) ||
                    device.error) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "succeeded device has incomplete work"));
                }
                break;
            case ReportWorkState::Failed:
                if (!device.error || device.error->code == KB_E_CANCELLED ||
                    running_steps != 0U || cancelled_steps != 0U) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "failed device error is invalid"));
                }
                if (!all_steps(device, ReportWorkState::Skipped)) {
                    if (failed_steps != 1U) {
                        return std::unexpected(make_error(
                            JobReportErrorKind::InvalidTransition,
                            "failed device has multiple failure points"));
                    }
                    const auto failed = static_cast<std::size_t>(std::distance(
                        device.steps.begin(),
                        std::ranges::find_if(device.steps, [](const auto& step) {
                            return step.state == ReportWorkState::Failed;
                        })));
                    if (!states_before(failed, ReportWorkState::Succeeded) ||
                        !states_after(failed + 1U,
                                      ReportWorkState::Skipped)) {
                        return std::unexpected(make_error(
                            JobReportErrorKind::InvalidTransition,
                            "failed device steps are not serial"));
                    }
                }
                break;
            case ReportWorkState::Cancelled: {
                if (!device.error || device.error->code != KB_E_CANCELLED ||
                    cancelled_steps != 1U || running_steps != 0U ||
                    failed_steps != 0U) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "cancelled device error is invalid"));
                }
                const auto cancelled = static_cast<std::size_t>(std::distance(
                    device.steps.begin(),
                    std::ranges::find_if(device.steps, [](const auto& step) {
                        return step.state == ReportWorkState::Cancelled;
                    })));
                if (!states_before(cancelled, ReportWorkState::Succeeded) ||
                    !states_after(cancelled + 1U,
                                  ReportWorkState::Skipped)) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "cancelled device steps are not serial"));
                }
                break;
            }
            case ReportWorkState::Skipped:
                if (!all_steps(device, ReportWorkState::Skipped) || device.error) {
                    return std::unexpected(make_error(
                        JobReportErrorKind::InvalidTransition,
                        "skipped device has active work"));
                }
                break;
        }
        if (!device.observed_product ||
            *device.observed_product != device.expected_product) {
            if (device.state != ReportWorkState::Failed || !device.error ||
                !all_steps(device, ReportWorkState::Skipped)) {
                if (!require_publishable_products &&
                    report.state == ReportState::Running &&
                    device.state == ReportWorkState::Pending &&
                    all_steps(device, ReportWorkState::Pending) &&
                    !device.error) {
                    continue;
                }
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidTransition,
                    "unverified product is publishable only as preflight failure"));
            }
        }
    }

    if (report.error) {
        result = validate_report_error(*report.error);
        if (!result) {
            return result;
        }
    }
    if (report.state == ReportState::Running) {
        if (report.finished_at || report.error) {
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidTransition,
                "running report carries terminal fields"));
        }
        return {};
    }
    if (!report.finished_at || report.summary.pending != 0U ||
        report.summary.running != 0U) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidTransition,
            "terminal report contains active work"));
    }
    if (report.state != ReportState::Cancelled &&
        report.summary.cancelled != 0U) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidTransition,
            "device cancellation did not win job publication"));
    }
    switch (report.state) {
        case ReportState::Running:
            break;
        case ReportState::Succeeded:
            if (report.summary.total == 0U ||
                report.summary.succeeded != report.summary.total ||
                report.error) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidTransition,
                    "succeeded report summary is invalid"));
            }
            break;
        case ReportState::PartiallyFailed:
            if (report.summary.succeeded == 0U || report.summary.failed == 0U ||
                report.error) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidTransition,
                    "partially-failed report summary is invalid"));
            }
            break;
        case ReportState::Failed:
            if (report.summary.succeeded != 0U ||
                (report.summary.failed == 0U && !report.error) ||
                (report.error && report.error->code == KB_E_CANCELLED)) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidTransition,
                    "failed report semantics are invalid"));
            }
            break;
        case ReportState::Cancelled:
            if (!report.error || report.error->code != KB_E_CANCELLED) {
                return std::unexpected(make_error(
                    JobReportErrorKind::InvalidTransition,
                    "cancelled report has no cancellation error"));
            }
            break;
    }
    return {};
}

[[nodiscard]] std::string digest_hex(const image::Sha256Digest& digest) {
    constexpr std::array<char, 16U> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string output(64U, '0');
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        const auto value = std::to_integer<unsigned int>(digest[index]);
        output[index * 2U] = digits[value >> 4U];
        output[index * 2U + 1U] = digits[value & 0x0FU];
    }
    return output;
}

[[nodiscard]] std::string_view status_name(const kb_status_t status) {
    switch (status) {
        case KB_E_INVALID_ARGUMENT:
            return "invalid_argument";
        case KB_E_OUT_OF_MEMORY:
            return "out_of_memory";
        case KB_E_NOT_SUPPORTED:
            return "not_supported";
        case KB_E_NO_DEVICE:
            return "no_device";
        case KB_E_AMBIGUOUS_DEVICE:
            return "ambiguous_device";
        case KB_E_BUSY:
            return "busy";
        case KB_E_TIMEOUT:
            return "timeout";
        case KB_E_CANCELLED:
            return "cancelled";
        case KB_E_IO:
            return "io";
        case KB_E_INTERNAL:
            return "internal";
        case KB_E_PROTOCOL:
            return "protocol";
        case KB_E_DEVICE_FAIL:
            return "device_fail";
        default:
            return "internal";
    }
}

[[nodiscard]] std::string_view transfer_name(
    const kb_transfer_state_t state) {
    switch (state) {
        case KB_TRANSFER_NOT_SENT:
            return "not_sent";
        case KB_TRANSFER_PARTIAL_OR_UNKNOWN:
            return "partial_or_unknown";
        case KB_TRANSFER_FULLY_TRANSFERRED:
            return "fully_transferred";
        default:
            return "not_sent";
    }
}

[[nodiscard]] std::string_view report_state_name(const ReportState state) {
    switch (state) {
        case ReportState::Running:
            return "running";
        case ReportState::Succeeded:
            return "succeeded";
        case ReportState::PartiallyFailed:
            return "partially_failed";
        case ReportState::Failed:
            return "failed";
        case ReportState::Cancelled:
            return "cancelled";
    }
    return "failed";
}

[[nodiscard]] std::string_view work_state_name(const ReportWorkState state) {
    switch (state) {
        case ReportWorkState::Pending:
            return "pending";
        case ReportWorkState::Running:
            return "running";
        case ReportWorkState::Succeeded:
            return "succeeded";
        case ReportWorkState::Failed:
            return "failed";
        case ReportWorkState::Cancelled:
            return "cancelled";
        case ReportWorkState::Skipped:
            return "skipped";
    }
    return "failed";
}

[[nodiscard]] std::string_view operation_name(const ReportOperation value) {
    switch (value) {
        case ReportOperation::Flash:
            return "flash";
        case ReportOperation::Erase:
            return "erase";
        case ReportOperation::SetActive:
            return "set_active";
        case ReportOperation::Reboot:
            return "reboot";
        case ReportOperation::Oem:
            return "oem";
    }
    return "flash";
}

[[nodiscard]] std::string_view slot_name(const ReportSlot value) {
    switch (value) {
        case ReportSlot::Current:
            return "current";
        case ReportSlot::Other:
            return "other";
        case ReportSlot::All:
            return "all";
        case ReportSlot::A:
            return "a";
        case ReportSlot::B:
            return "b";
    }
    return "current";
}

[[nodiscard]] std::string_view reboot_name(const ReportRebootTarget value) {
    switch (value) {
        case ReportRebootTarget::System:
            return "system";
        case ReportRebootTarget::Bootloader:
            return "bootloader";
        case ReportRebootTarget::Recovery:
            return "recovery";
        case ReportRebootTarget::Fastboot:
            return "fastboot";
    }
    return "system";
}

[[nodiscard]] std::expected<void, JobReportError> append_quoted(
    std::string& output,
    const std::string_view value) {
    const auto result = append_canonical_json_quoted_string(output, value);
    if (!result) {
        return std::unexpected(make_error(
            result.error().kind == CanonicalJsonErrorKind::InvalidUtf8
                ? JobReportErrorKind::InvalidUtf8
                : JobReportErrorKind::IntegerOutOfRange,
            "unable to encode canonical JobReport JSON"));
    }
    return {};
}

[[nodiscard]] std::expected<void, JobReportError> append_unsigned(
    std::string& output,
    const std::uint64_t value) {
    const auto result = append_canonical_json_unsigned_integer(output, value);
    if (!result) {
        return std::unexpected(make_error(JobReportErrorKind::IntegerOutOfRange,
                                          "report integer is not I-JSON safe"));
    }
    return {};
}

[[nodiscard]] std::expected<void, JobReportError> append_signed(
    std::string& output,
    const std::int64_t value) {
    const auto result = append_canonical_json_signed_integer(output, value);
    if (!result) {
        return std::unexpected(make_error(JobReportErrorKind::IntegerOutOfRange,
                                          "report integer is not I-JSON safe"));
    }
    return {};
}

[[nodiscard]] std::expected<void, JobReportError> append_nullable_string(
    std::string& output,
    const std::optional<std::string>& value) {
    if (!value) {
        output += "null";
        return {};
    }
    return append_quoted(output, *value);
}

[[nodiscard]] std::expected<void, JobReportError> append_error_json(
    std::string& output,
    const std::optional<ReportError>& error) {
    if (!error) {
        output += "null";
        return {};
    }
    output += "{\"code\":";
    auto result = append_signed(output, error->code);
    if (!result) {
        return result;
    }
    output += ",\"deviceIdentifier\":";
    result = append_nullable_string(output, error->device_identifier);
    if (!result) {
        return result;
    }
    output += ",\"message\":";
    result = append_quoted(output, error->message);
    if (!result) {
        return result;
    }
    output += ",\"nativeCode\":";
    if (error->native_code) {
        result = append_signed(output, *error->native_code);
        if (!result) {
            return result;
        }
    } else {
        output += "null";
    }
    output += ",\"status\":";
    result = append_quoted(output, status_name(error->code));
    if (!result) {
        return result;
    }
    output += ",\"transferCertainty\":";
    if (error->transfer_certainty) {
        result = append_quoted(output,
                               transfer_name(*error->transfer_certainty));
        if (!result) {
            return result;
        }
    } else {
        output += "null";
    }
    output.push_back('}');
    return {};
}

[[nodiscard]] std::expected<std::string, JobReportError> serialize_report(
    const ReportData& report) {
    std::string output{"{\"devices\":["};
    for (std::size_t device_index = 0U;
         device_index < report.devices.size();
         ++device_index) {
        if (device_index != 0U) {
            output.push_back(',');
        }
        const auto& device = report.devices[device_index];
        output += "{\"error\":";
        auto result = append_error_json(output, device.error);
        if (!result) {
            return std::unexpected(result.error());
        }
        output += ",\"identifier\":";
        result = append_quoted(output, device.identifier);
        if (!result) {
            return std::unexpected(result.error());
        }
        output += ",\"observedProduct\":";
        result = append_nullable_string(output, device.observed_product);
        if (!result) {
            return std::unexpected(result.error());
        }
        output += ",\"serial\":";
        result = append_nullable_string(output, device.serial);
        if (!result) {
            return std::unexpected(result.error());
        }
        output += ",\"state\":";
        result = append_quoted(output, work_state_name(device.state));
        if (!result) {
            return std::unexpected(result.error());
        }
        output += ",\"steps\":[";
        for (std::size_t step_index = 0U;
             step_index < device.steps.size();
             ++step_index) {
            if (step_index != 0U) {
                output.push_back(',');
            }
            const auto& step = device.steps[step_index];
            output += "{\"artifact\":";
            result = append_nullable_string(output, step.spec.artifact);
            if (!result) {
                return std::unexpected(result.error());
            }
            output += ",\"bytesTotal\":";
            if (step.spec.bytes_total) {
                result = append_unsigned(output, *step.spec.bytes_total);
                if (!result) {
                    return std::unexpected(result.error());
                }
            } else {
                output += "null";
            }
            output += ",\"bytesTransferred\":";
            if (step.bytes_transferred) {
                result = append_unsigned(output, *step.bytes_transferred);
                if (!result) {
                    return std::unexpected(result.error());
                }
            } else {
                output += "null";
            }
            output += ",\"error\":";
            result = append_error_json(output, step.error);
            if (!result) {
                return std::unexpected(result.error());
            }
            output += ",\"finishedAt\":";
            result = append_nullable_string(output, step.finished_at);
            if (!result) {
                return std::unexpected(result.error());
            }
            output += ",\"index\":";
            result = append_unsigned(output, step.index);
            if (!result) {
                return std::unexpected(result.error());
            }
            output += ",\"oemCommand\":";
            result = append_nullable_string(output, step.spec.oem_command);
            if (!result) {
                return std::unexpected(result.error());
            }
            output += ",\"operation\":";
            result = append_quoted(output, operation_name(step.spec.operation));
            if (!result) {
                return std::unexpected(result.error());
            }
            output += ",\"partition\":";
            result = append_nullable_string(output, step.spec.partition);
            if (!result) {
                return std::unexpected(result.error());
            }
            output += ",\"rebootTarget\":";
            if (step.spec.reboot_target) {
                result = append_quoted(output,
                                       reboot_name(*step.spec.reboot_target));
                if (!result) {
                    return std::unexpected(result.error());
                }
            } else {
                output += "null";
            }
            output += ",\"slot\":";
            if (step.spec.slot) {
                result = append_quoted(output, slot_name(*step.spec.slot));
                if (!result) {
                    return std::unexpected(result.error());
                }
            } else {
                output += "null";
            }
            output += ",\"startedAt\":";
            result = append_nullable_string(output, step.started_at);
            if (!result) {
                return std::unexpected(result.error());
            }
            output += ",\"state\":";
            result = append_quoted(output, work_state_name(step.state));
            if (!result) {
                return std::unexpected(result.error());
            }
            output.push_back('}');
        }
        output += "],\"target\":";
        result = append_quoted(output, device.target);
        if (!result) {
            return std::unexpected(result.error());
        }
        output += ",\"usbPath\":";
        result = append_nullable_string(output, device.usb_path);
        if (!result) {
            return std::unexpected(result.error());
        }
        output.push_back('}');
    }
    output += "],\"error\":";
    auto result = append_error_json(output, report.error);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"finishedAt\":";
    result = append_nullable_string(output, report.finished_at);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"jobId\":";
    result = append_quoted(output, report.job_id);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"planSha256\":";
    result = append_quoted(output, report.plan_sha256);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"schemaVersion\":1,\"startedAt\":";
    result = append_quoted(output, report.started_at);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"state\":";
    result = append_quoted(output, report_state_name(report.state));
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"summary\":{\"cancelled\":";
    result = append_unsigned(output, report.summary.cancelled);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"failed\":";
    result = append_unsigned(output, report.summary.failed);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"pending\":";
    result = append_unsigned(output, report.summary.pending);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"running\":";
    result = append_unsigned(output, report.summary.running);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"skipped\":";
    result = append_unsigned(output, report.summary.skipped);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"succeeded\":";
    result = append_unsigned(output, report.summary.succeeded);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += ",\"total\":";
    result = append_unsigned(output, report.summary.total);
    if (!result) {
        return std::unexpected(result.error());
    }
    output += "}}";
    return output;
}

void mark_skipped(ReportStepSnapshot& step,
                  const std::string& finished_at,
                  const ReportSkipReason reason) {
    step.state = ReportWorkState::Skipped;
    step.started_at.reset();
    step.finished_at = finished_at;
    if (step.bytes_transferred) {
        *step.bytes_transferred = 0U;
    }
    step.error.reset();
    step.skip_reason = reason;
}

[[nodiscard]] ReportError scoped_error(ReportError error,
                                       const std::string& identifier) {
    error.device_identifier = identifier;
    return error;
}

[[nodiscard]] std::expected<void, JobReportError> validate_transition_time(
    const ReportData& report,
    const std::string_view timestamp) {
    const auto parsed = parse_timestamp(timestamp);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return require_order(report.started_at,
                         timestamp,
                         "transition occurs before the job started");
}

[[nodiscard]] std::expected<ReportDeviceSnapshot*, JobReportError> device_at(
    ReportData& report,
    const std::size_t index) {
    if (index >= report.devices.size()) {
        return std::unexpected(make_error(JobReportErrorKind::InvalidArgument,
                                          "device index is out of range"));
    }
    return &report.devices[index];
}

[[nodiscard]] bool flash_is_complete(const ReportStepSnapshot& step) noexcept {
    return step.spec.operation != ReportOperation::Flash ||
           *step.bytes_transferred == *step.spec.bytes_total;
}

}  // namespace

struct JobReport::Implementation final {
    ReportData data;
    std::string json;
};

struct JobReportBuilder::Implementation final {
    mutable std::mutex mutex;
    MutableReportState state;
    JobReportBuilderOptions options;

    template <typename Mutation>
    [[nodiscard]] std::expected<void, JobReportError> commit(
        const bool permitted_after_cancellation,
        Mutation&& mutation) {
        std::lock_guard lock{mutex};
        if (state.report.state != ReportState::Running) {
            return std::unexpected(make_error(JobReportErrorKind::AlreadyTerminal,
                                              "JobReport is already terminal"));
        }
        if (state.cancellation_latched && !permitted_after_cancellation) {
            return std::unexpected(make_error(
                JobReportErrorKind::CancellationLatched,
                "job cancellation is latched and wins publication"));
        }
        MutableReportState candidate = state;
        auto result = mutation(candidate);
        if (!result) {
            return result;
        }
        candidate.report.summary = summarize(candidate.report.devices);
        result = validate_report_data(candidate.report, false);
        if (!result) {
            return result;
        }
        if (options.fault_hook != nullptr) {
            options.fault_hook(JobReportFaultPoint::BeforeCommit,
                               options.fault_context);
        }
        state = std::move(candidate);
        return {};
    }

    [[nodiscard]] std::expected<JobReport, JobReportError> snapshot(
        const bool require_terminal) const {
        ReportData copy;
        {
            std::lock_guard lock{mutex};
            const bool terminal = state.report.state != ReportState::Running;
            if (require_terminal && !terminal) {
                return std::unexpected(make_error(
                    JobReportErrorKind::NotTerminal,
                    "terminal JobReport is not available"));
            }
            if (!require_terminal && terminal) {
                return std::unexpected(make_error(
                    JobReportErrorKind::AlreadyTerminal,
                    "running JobReport is no longer available"));
            }
            if (!require_terminal && state.cancellation_latched) {
                return std::unexpected(make_error(
                    JobReportErrorKind::CancellationLatched,
                    "running JobReport cannot be published after cancellation "
                    "is latched"));
            }
            copy = state.report;
        }
        auto validation = validate_report_data(copy);
        if (!validation) {
            return std::unexpected(validation.error());
        }
        auto json = serialize_report(copy);
        if (!json) {
            return std::unexpected(json.error());
        }
        auto published = std::make_unique<JobReport::Implementation>(
            JobReport::Implementation{std::move(copy), std::move(*json)});
        if (options.fault_hook != nullptr) {
            options.fault_hook(JobReportFaultPoint::BeforeSnapshotPublish,
                               options.fault_context);
        }
        return JobReport{std::move(published)};
    }
};

JobReport::JobReport(std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation)) {}

JobReport::~JobReport() = default;
JobReport::JobReport(JobReport&&) noexcept = default;
JobReport& JobReport::operator=(JobReport&&) noexcept = default;

std::string_view JobReport::job_id() const noexcept {
    return implementation_->data.job_id;
}

std::string_view JobReport::plan_sha256() const noexcept {
    return implementation_->data.plan_sha256;
}

ReportState JobReport::state() const noexcept {
    return implementation_->data.state;
}

std::string_view JobReport::started_at() const noexcept {
    return implementation_->data.started_at;
}

const std::optional<std::string>& JobReport::finished_at() const noexcept {
    return implementation_->data.finished_at;
}

const std::vector<ReportDeviceSnapshot>& JobReport::devices() const noexcept {
    return implementation_->data.devices;
}

const ReportSummary& JobReport::summary() const noexcept {
    return implementation_->data.summary;
}

const std::optional<ReportError>& JobReport::error() const noexcept {
    return implementation_->data.error;
}

std::string_view JobReport::canonical_json() const noexcept {
    return implementation_->json;
}

JobReportBuilder::JobReportBuilder(
    std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation)) {}

JobReportBuilder::~JobReportBuilder() = default;
JobReportBuilder::JobReportBuilder(JobReportBuilder&&) noexcept = default;
JobReportBuilder& JobReportBuilder::operator=(JobReportBuilder&&) noexcept =
    default;

std::expected<JobReportBuilder, JobReportError> JobReportBuilder::create(
    std::string job_id,
    const image::Sha256Digest& plan_sha256,
    std::string started_at,
    std::vector<ReportDeviceSpec> devices,
    const JobReportBuilderOptions& options) {
    auto result = validate_string(job_id, 1U, 256U, "jobId");
    if (!result) {
        return std::unexpected(result.error());
    }
    const auto parsed_timestamp = parse_timestamp(started_at);
    if (!parsed_timestamp) {
        return std::unexpected(parsed_timestamp.error());
    }
    if (devices.size() >
        static_cast<std::size_t>(kCanonicalJsonMaximumSafeInteger)) {
        return std::unexpected(make_error(JobReportErrorKind::IntegerOutOfRange,
                                          "device count is not I-JSON safe"));
    }

    std::unordered_set<std::string> identifiers;
    identifiers.reserve(devices.size());
    std::vector<ReportDeviceSnapshot> snapshots;
    snapshots.reserve(devices.size());
    for (auto& device : devices) {
        result = validate_string(device.identifier,
                                 1U,
                                 4096U,
                                 "device identifier");
        if (!result) {
            return std::unexpected(result.error());
        }
        if (!identifiers.insert(device.identifier).second) {
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidArgument,
                "device identifiers must be unique"));
        }
        result = validate_optional_string(device.serial, 4096U, "serial");
        if (!result) {
            return std::unexpected(result.error());
        }
        result = validate_optional_string(device.usb_path, 4096U, "USB path");
        if (!result) {
            return std::unexpected(result.error());
        }
        if (!device.serial && !device.usb_path) {
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidArgument,
                "device needs a serial or USB path plan identity"));
        }
        result = validate_string(device.target, 1U, 256U, "target");
        if (!result) {
            return std::unexpected(result.error());
        }
        result = validate_string(device.expected_product,
                                 1U,
                                 4096U,
                                 "expected product");
        if (!result) {
            return std::unexpected(result.error());
        }
        result = validate_optional_string(device.observed_product,
                                          4096U,
                                          "observed product");
        if (!result) {
            return std::unexpected(result.error());
        }
        if (device.steps.empty() || device.steps.size() > 16384U) {
            return std::unexpected(make_error(
                JobReportErrorKind::InvalidArgument,
                "device needs between 1 and 16384 plan steps"));
        }
        std::vector<ReportStepSnapshot> steps;
        steps.reserve(device.steps.size());
        for (std::size_t index = 0U; index < device.steps.size(); ++index) {
            result = validate_step_spec(device.steps[index]);
            if (!result) {
                return std::unexpected(result.error());
            }
            const bool transfers_data =
                device.steps[index].operation == ReportOperation::Flash;
            steps.push_back(ReportStepSnapshot{
                .index = index,
                .spec = std::move(device.steps[index]),
                .state = ReportWorkState::Pending,
                .started_at = std::nullopt,
                .finished_at = std::nullopt,
                .bytes_transferred =
                    transfers_data
                        ? std::optional<std::uint64_t>{0U}
                        : std::nullopt,
                .error = std::nullopt,
                .skip_reason = std::nullopt,
            });
        }
        snapshots.push_back(ReportDeviceSnapshot{
            .identifier = std::move(device.identifier),
            .serial = std::move(device.serial),
            .usb_path = std::move(device.usb_path),
            .target = std::move(device.target),
            .expected_product = std::move(device.expected_product),
            .observed_product = std::move(device.observed_product),
            .state = ReportWorkState::Pending,
            .steps = std::move(steps),
            .error = std::nullopt,
        });
    }

    ReportData report{
        .job_id = std::move(job_id),
        .plan_sha256 = digest_hex(plan_sha256),
        .state = ReportState::Running,
        .started_at = std::move(started_at),
        .finished_at = std::nullopt,
        .devices = std::move(snapshots),
        .summary = {},
        .error = std::nullopt,
    };
    report.summary = summarize(report.devices);
    // An unverified product is a valid builder state but is intentionally not
    // publishable until verify_product or a preflight failure transition.
    auto implementation = std::make_unique<Implementation>();
    implementation->state.report = std::move(report);
    implementation->options = options;
    return JobReportBuilder{std::move(implementation)};
}

std::expected<void, JobReportError> JobReportBuilder::verify_product(
    const std::size_t device_index,
    std::string observed_product) {
    auto validation =
        validate_string(observed_product, 1U, 4096U, "observed product");
    if (!validation) {
        return validation;
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto selected = device_at(state.report, device_index);
        if (!selected) {
            return std::expected<void, JobReportError>{
                std::unexpected(selected.error())};
        }
        auto& device = **selected;
        if (device.state != ReportWorkState::Pending ||
            device.observed_product) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "device product can be verified only once"))};
        }
        if (observed_product != device.expected_product) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "product mismatch must fail at preflight scope"))};
        }
        device.observed_product = std::move(observed_product);
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::begin_first_step(
    const std::size_t device_index,
    std::string started_at) {
    auto validation = parse_timestamp(started_at);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto time_result = validate_transition_time(state.report, started_at);
        if (!time_result) {
            return time_result;
        }
        auto selected = device_at(state.report, device_index);
        if (!selected) {
            return std::expected<void, JobReportError>{
                std::unexpected(selected.error())};
        }
        auto& device = **selected;
        if (device.state != ReportWorkState::Pending ||
            !device.observed_product ||
            *device.observed_product != device.expected_product) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "device is not ready for destructive execution"))};
        }
        device.state = ReportWorkState::Running;
        device.steps.front().state = ReportWorkState::Running;
        device.steps.front().started_at = std::move(started_at);
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::update_flash_progress(
    const std::size_t device_index,
    const std::size_t step_index,
    const std::uint64_t bytes_transferred) {
    if (bytes_transferred >
        static_cast<std::uint64_t>(kCanonicalJsonMaximumSafeInteger)) {
        return std::unexpected(make_error(JobReportErrorKind::IntegerOutOfRange,
                                          "progress is not I-JSON safe"));
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto selected = device_at(state.report, device_index);
        if (!selected) {
            return std::expected<void, JobReportError>{
                std::unexpected(selected.error())};
        }
        auto& device = **selected;
        if (device.state != ReportWorkState::Running ||
            step_index >= device.steps.size()) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "progress target is not running"))};
        }
        auto& step = device.steps[step_index];
        if (step.state != ReportWorkState::Running ||
            step.spec.operation != ReportOperation::Flash ||
            bytes_transferred < *step.bytes_transferred ||
            bytes_transferred > *step.spec.bytes_total) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "flash progress is non-monotonic or out of range"))};
        }
        *step.bytes_transferred = bytes_transferred;
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::advance_step(
    const std::size_t device_index,
    const std::size_t completed_step_index,
    std::string completed_at,
    std::string next_started_at) {
    auto validation = parse_timestamp(completed_at);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    validation = parse_timestamp(next_started_at);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto result = validate_transition_time(state.report, completed_at);
        if (!result) {
            return result;
        }
        result = require_order(completed_at,
                               next_started_at,
                               "next step starts before prior completion");
        if (!result) {
            return result;
        }
        auto selected = device_at(state.report, device_index);
        if (!selected) {
            return std::expected<void, JobReportError>{
                std::unexpected(selected.error())};
        }
        auto& device = **selected;
        if (device.state != ReportWorkState::Running ||
            completed_step_index >= device.steps.size() ||
            completed_step_index == device.steps.size() - 1U) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "step advance index is invalid"))};
        }
        const auto next_step_index = completed_step_index + 1U;
        auto& completed = device.steps[completed_step_index];
        auto& next = device.steps[next_step_index];
        if (completed.state != ReportWorkState::Running ||
            next.state != ReportWorkState::Pending ||
            !flash_is_complete(completed)) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "step cannot advance before successful completion"))};
        }
        completed.state = ReportWorkState::Succeeded;
        completed.finished_at = std::move(completed_at);
        next.state = ReportWorkState::Running;
        next.started_at = std::move(next_started_at);
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::complete_device(
    const std::size_t device_index,
    const std::size_t final_step_index,
    std::string finished_at) {
    auto validation = parse_timestamp(finished_at);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto result = validate_transition_time(state.report, finished_at);
        if (!result) {
            return result;
        }
        auto selected = device_at(state.report, device_index);
        if (!selected) {
            return std::expected<void, JobReportError>{
                std::unexpected(selected.error())};
        }
        auto& device = **selected;
        if (device.state != ReportWorkState::Running ||
            final_step_index + 1U != device.steps.size()) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "device completion does not name the final step"))};
        }
        auto& step = device.steps[final_step_index];
        if (step.state != ReportWorkState::Running || !flash_is_complete(step)) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "final step is incomplete"))};
        }
        step.state = ReportWorkState::Succeeded;
        step.finished_at = std::move(finished_at);
        device.state = ReportWorkState::Succeeded;
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::fail_step(
    const std::size_t device_index,
    const std::size_t step_index,
    std::string finished_at,
    ReportError error,
    const ReportSkipReason suffix_reason) {
    if (suffix_reason != ReportSkipReason::FollowingStepFailure) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidArgument,
            "failed step suffix needs FollowingStepFailure reason"));
    }
    auto validation = validate_report_error(error);
    if (!validation) {
        return validation;
    }
    if (error.code == KB_E_CANCELLED) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidTransition,
            "KB_E_CANCELLED cannot be encoded as step failure"));
    }
    const auto timestamp = parse_timestamp(finished_at);
    if (!timestamp) {
        return std::unexpected(timestamp.error());
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto result = validate_transition_time(state.report, finished_at);
        if (!result) {
            return result;
        }
        auto selected = device_at(state.report, device_index);
        if (!selected) {
            return std::expected<void, JobReportError>{
                std::unexpected(selected.error())};
        }
        auto& device = **selected;
        if (device.state != ReportWorkState::Running ||
            step_index >= device.steps.size() ||
            device.steps[step_index].state != ReportWorkState::Running) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "only the running step can fail"))};
        }
        auto scoped = scoped_error(std::move(error), device.identifier);
        auto& step = device.steps[step_index];
        step.state = ReportWorkState::Failed;
        step.finished_at = finished_at;
        step.error = scoped;
        device.state = ReportWorkState::Failed;
        device.error = std::move(scoped);
        for (std::size_t index = step_index + 1U; index < device.steps.size();
             ++index) {
            mark_skipped(device.steps[index], finished_at, suffix_reason);
        }
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::fail_device_preflight(
    const std::size_t device_index,
    std::optional<std::string> observed_product,
    std::string finished_at,
    ReportError error) {
    return fail_device_preflight_with_reason(
        device_index,
        std::move(observed_product),
        std::move(finished_at),
        std::move(error),
        ReportSkipReason::DevicePreflightFailure);
}

std::expected<void, JobReportError>
JobReportBuilder::fail_device_preflight_with_reason(
    const std::size_t device_index,
    std::optional<std::string> observed_product,
    std::string finished_at,
    ReportError error,
    const ReportSkipReason reason) {
    if (reason != ReportSkipReason::DevicePreflightFailure &&
        reason != ReportSkipReason::ProductMismatch) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidArgument,
            "device preflight failure needs a preflight skip reason"));
    }
    auto validation = validate_optional_string(observed_product,
                                               4096U,
                                               "observed product");
    if (!validation) {
        return validation;
    }
    validation = validate_report_error(error);
    if (!validation) {
        return validation;
    }
    if (error.code == KB_E_CANCELLED) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidTransition,
            "KB_E_CANCELLED cannot be encoded as preflight failure"));
    }
    const auto timestamp = parse_timestamp(finished_at);
    if (!timestamp) {
        return std::unexpected(timestamp.error());
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto result = validate_transition_time(state.report, finished_at);
        if (!result) {
            return result;
        }
        auto selected = device_at(state.report, device_index);
        if (!selected) {
            return std::expected<void, JobReportError>{
                std::unexpected(selected.error())};
        }
        auto& device = **selected;
        if (device.state != ReportWorkState::Pending) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "preflight failure occurred after execution began"))};
        }
        if (reason == ReportSkipReason::ProductMismatch && observed_product &&
            *observed_product == device.expected_product) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "matching product cannot be published as product "
                           "mismatch"))};
        }
        if (reason == ReportSkipReason::DevicePreflightFailure &&
            observed_product &&
            *observed_product != device.expected_product) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "observed product mismatch must use product "
                           "preflight failure"))};
        }
        device.observed_product = std::move(observed_product);
        auto scoped = scoped_error(std::move(error), device.identifier);
        device.state = ReportWorkState::Failed;
        device.error = std::move(scoped);
        for (auto& step : device.steps) {
            mark_skipped(step, finished_at, reason);
        }
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::fail_product_preflight(
    const std::size_t device_index,
    std::optional<std::string> observed_product,
    std::string finished_at,
    ReportError error) {
    return fail_device_preflight_with_reason(
        device_index,
        std::move(observed_product),
        std::move(finished_at),
        std::move(error),
        ReportSkipReason::ProductMismatch);
}

std::expected<void, JobReportError> JobReportBuilder::skip_pending_device(
    const std::size_t device_index,
    std::string finished_at,
    const ReportSkipReason reason) {
    if (reason != ReportSkipReason::PolicyStopped &&
        reason != ReportSkipReason::JobPreflightFailure) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidArgument,
            "pending device skip reason is not a policy/preflight stop"));
    }
    auto validation = parse_timestamp(finished_at);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto result = validate_transition_time(state.report, finished_at);
        if (!result) {
            return result;
        }
        auto selected = device_at(state.report, device_index);
        if (!selected) {
            return std::expected<void, JobReportError>{
                std::unexpected(selected.error())};
        }
        auto& device = **selected;
        if (device.state != ReportWorkState::Pending ||
            !device.observed_product ||
            *device.observed_product != device.expected_product) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "only a verified pending device can be skipped"))};
        }
        device.state = ReportWorkState::Skipped;
        for (auto& step : device.steps) {
            mark_skipped(step, finished_at, reason);
        }
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::request_cancellation(
    ReportError error) {
    auto validation = validate_report_error(error);
    if (!validation) {
        return validation;
    }
    if (error.code != KB_E_CANCELLED) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidTransition,
            "cancellation latch requires KB_E_CANCELLED"));
    }
    error.device_identifier.reset();
    return implementation_->commit(true, [&](MutableReportState& state) {
        if (state.cancellation_latched) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::CancellationLatched,
                           "job cancellation was already latched"))};
        }
        const auto unresolved_product = std::ranges::find_if(
            state.report.devices, [](const auto& device) {
                return device.state == ReportWorkState::Pending &&
                       (!device.observed_product ||
                        *device.observed_product != device.expected_product);
            });
        if (unresolved_product != state.report.devices.end()) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "device product outcome is not complete; use a "
                           "zero-device cancellation report before binding"))};
        }
        state.cancellation_latched = true;
        state.cancellation_error = std::move(error);
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::finish_cancelled(
    std::string finished_at,
    const ReportSkipReason suffix_reason) {
    if (suffix_reason != ReportSkipReason::JobCancellation &&
        suffix_reason != ReportSkipReason::FollowingStepCancellation) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidArgument,
            "cancellation suffix needs a cancellation skip reason"));
    }
    auto validation = parse_timestamp(finished_at);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    return implementation_->commit(true, [&](MutableReportState& state) {
        if (!state.cancellation_latched || !state.cancellation_error) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "job cancellation was not latched"))};
        }
        auto result = validate_transition_time(state.report, finished_at);
        if (!result) {
            return result;
        }
        for (auto& device : state.report.devices) {
            if (device.state == ReportWorkState::Pending) {
                if (!device.observed_product ||
                    *device.observed_product != device.expected_product) {
                    return std::expected<void, JobReportError>{std::unexpected(
                        make_error(JobReportErrorKind::InvalidTransition,
                                   "unverified device cannot be cancelled in a "
                                   "cross-document report"))};
                }
                auto scoped = scoped_error(*state.cancellation_error,
                                           device.identifier);
                device.state = ReportWorkState::Cancelled;
                device.error = scoped;
                auto& first = device.steps.front();
                first.state = ReportWorkState::Cancelled;
                first.finished_at = finished_at;
                first.error = std::move(scoped);
                for (std::size_t index = 1U; index < device.steps.size();
                     ++index) {
                    mark_skipped(device.steps[index],
                                 finished_at,
                                 suffix_reason);
                }
            } else if (device.state == ReportWorkState::Running) {
                const auto active = std::ranges::find_if(
                    device.steps, [](const auto& step) {
                        return step.state == ReportWorkState::Running;
                    });
                if (active == device.steps.end()) {
                    return std::expected<void, JobReportError>{std::unexpected(
                        make_error(JobReportErrorKind::InvalidTransition,
                                   "running device has no running step"))};
                }
                auto scoped = scoped_error(*state.cancellation_error,
                                           device.identifier);
                device.state = ReportWorkState::Cancelled;
                device.error = scoped;
                active->state = ReportWorkState::Cancelled;
                active->finished_at = finished_at;
                active->error = std::move(scoped);
                const auto active_index = static_cast<std::size_t>(
                    std::distance(device.steps.begin(), active));
                for (std::size_t index = active_index + 1U;
                     index < device.steps.size();
                     ++index) {
                    mark_skipped(device.steps[index],
                                 finished_at,
                                 suffix_reason);
                }
            }
        }
        state.report.state = ReportState::Cancelled;
        state.report.finished_at = std::move(finished_at);
        state.report.error = state.cancellation_error;
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::finish(
    std::string finished_at) {
    auto validation = parse_timestamp(finished_at);
    if (!validation) {
        return std::unexpected(validation.error());
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto result = validate_transition_time(state.report, finished_at);
        if (!result) {
            return result;
        }
        state.report.summary = summarize(state.report.devices);
        const auto& summary = state.report.summary;
        if (summary.pending != 0U || summary.running != 0U ||
            summary.cancelled != 0U) {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "normal finish has active or cancelled devices"))};
        }
        if (summary.total > 0U && summary.succeeded == summary.total) {
            state.report.state = ReportState::Succeeded;
        } else if (summary.succeeded > 0U && summary.failed > 0U) {
            state.report.state = ReportState::PartiallyFailed;
        } else if (summary.succeeded == 0U && summary.failed > 0U) {
            state.report.state = ReportState::Failed;
        } else {
            return std::expected<void, JobReportError>{std::unexpected(
                make_error(JobReportErrorKind::InvalidTransition,
                           "device states do not derive a terminal job state"))};
        }
        state.report.finished_at = std::move(finished_at);
        return std::expected<void, JobReportError>{};
    });
}

std::expected<void, JobReportError> JobReportBuilder::finish_failed(
    std::string finished_at,
    ReportError error,
    const ReportSkipReason pending_reason) {
    if (pending_reason != ReportSkipReason::JobPreflightFailure) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidArgument,
            "job failure needs JobPreflightFailure skip reason"));
    }
    auto validation = validate_report_error(error);
    if (!validation) {
        return validation;
    }
    if (error.code == KB_E_CANCELLED) {
        return std::unexpected(make_error(
            JobReportErrorKind::InvalidTransition,
            "KB_E_CANCELLED cannot be encoded as failed job"));
    }
    const auto timestamp = parse_timestamp(finished_at);
    if (!timestamp) {
        return std::unexpected(timestamp.error());
    }
    return implementation_->commit(false, [&](MutableReportState& state) {
        auto result = validate_transition_time(state.report, finished_at);
        if (!result) {
            return result;
        }
        for (auto& device : state.report.devices) {
            if (device.state == ReportWorkState::Running ||
                device.state == ReportWorkState::Succeeded ||
                device.state == ReportWorkState::Cancelled) {
                return std::expected<void, JobReportError>{std::unexpected(
                    make_error(JobReportErrorKind::InvalidTransition,
                               "job-level preflight failure occurred after "
                               "destructive work"))};
            }
            if (device.state == ReportWorkState::Failed &&
                !all_steps(device, ReportWorkState::Skipped)) {
                return std::expected<void, JobReportError>{std::unexpected(
                    make_error(JobReportErrorKind::InvalidTransition,
                               "job-level preflight failure cannot wrap an "
                               "executed device failure"))};
            }
            if (device.state == ReportWorkState::Pending) {
                if (!device.observed_product ||
                    *device.observed_product != device.expected_product) {
                    return std::expected<void, JobReportError>{std::unexpected(
                        make_error(JobReportErrorKind::InvalidTransition,
                                   "unverified pending device needs a device "
                                   "preflight failure"))};
                }
                device.state = ReportWorkState::Skipped;
                for (auto& step : device.steps) {
                    mark_skipped(step, finished_at, pending_reason);
                }
            }
        }
        state.report.state = ReportState::Failed;
        state.report.finished_at = std::move(finished_at);
        state.report.error = std::move(error);
        return std::expected<void, JobReportError>{};
    });
}

std::expected<JobReport, JobReportError>
JobReportBuilder::running_snapshot() const {
    return implementation_->snapshot(false);
}

std::expected<JobReport, JobReportError>
JobReportBuilder::terminal_snapshot() const {
    return implementation_->snapshot(true);
}

}  // namespace kairosboot::fleet
