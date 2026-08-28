#include <kairosboot/kairosboot.h>

#include "src/api/device_selection.hpp"
#include "src/api/device_selector.hpp"
#include "src/api/error_handle.hpp"
#include "src/api/command_result_handle.hpp"
#include "src/api/error_mapping.hpp"
#include "src/api/operation_state.hpp"
#include "src/fastboot/primitive_update_device.hpp"
#include "src/fastboot/primitive_service.hpp"
#include "src/fastboot/update_executor.hpp"
#include "src/fastboot/update_package_preflight.hpp"
#include "src/fastboot/variable_parser.hpp"
#include "src/image/artifact_source.hpp"
#include "src/image/file_source.hpp"
#include "src/image/flash_artifact.hpp"
#include "src/image/sparse_flash_plan.hpp"
#include "src/kairosboot_internal.hpp"
#include "src/protocol/fastboot_protocol.hpp"
#include "src/transport/image_transfer_source.hpp"
#include "src/transport/libusb_runtime.hpp"
#include "src/transport/sequential_streaming_transport.hpp"
#include "src/transport/tcp_fastboot.hpp"
#include "src/transport/udp_fastboot.hpp"
#include "src/transport/usb_fastboot.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <utility>
#include <vector>

struct kb_context_usb_state {
  mutable std::mutex usb_runtime_mutex;
  std::shared_ptr<kairosboot::transport::LibusbRuntime> usb_runtime;
};

struct kb_context {
  kb_context_options_t options{};
  std::shared_ptr<kb_context_usb_state> usb_state;
};

struct kb_device_info {
  std::string serial;
  std::string usb_path;
  std::string product;
};

struct kb_device_list {
  std::vector<kb_device_info> devices;
};

struct kb_command_result {
  explicit kb_command_result(
      std::shared_ptr<const kairosboot::api::CommandResultPayload> payload)
      : handle(std::move(payload)) {}

  kairosboot::api::CommandResultHandle handle;
};

struct kb_operation {
  explicit kb_operation(kairosboot::api::OperationState::Task task)
      : state(std::make_unique<kairosboot::api::OperationState>(
            std::move(task))) {}

  std::unique_ptr<kairosboot::api::OperationState> state;
  mutable std::mutex error_mutex;
  mutable std::unique_ptr<kb_error> public_error;
};

namespace {

constexpr uint32_t kDefaultTimeoutMs = KB_WAIT_INFINITE;
constexpr std::uint64_t kDefaultMaximumReceiveBytes = 64ULL * 1024ULL * 1024ULL;

std::mutex g_usb_runtime_mutex;
std::weak_ptr<kairosboot::transport::LibusbRuntime> g_usb_runtime;

void clear_error(kb_error_t **error) noexcept {
  if (error != nullptr) {
    *error = nullptr;
  }
}

kb_status_t fail(kb_error_t **error, kb_status_t status, const char *message,
                 const char *device_identifier = "",
                 int32_t native_code = 0,
                 kb_transfer_state_t transfer_state =
                     KB_TRANSFER_NOT_SENT) noexcept {
  if (error != nullptr) {
    try {
      *error = new kb_error{
          .status = status,
          .message = message,
          .device_identifier =
              device_identifier == nullptr ? "" : device_identifier,
          .native_code = native_code,
          .transfer_state = transfer_state,
          .device_message = {},
          .command_messages = {},
          .inbound_expected = std::nullopt,
          .inbound_transferred = 0,
          .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
          .session_poisoned = false,
      };
    } catch (...) {
      *error = nullptr;
    }
  }
  return status;
}

kb_status_t fail(
    kb_error_t **error,
    const kairosboot::api::OperationErrorPayload &payload) noexcept {
  if (error != nullptr) {
    try {
      *error = new kb_error{
          payload.status,
          payload.message,
          payload.device_identifier,
          payload.native_code,
          payload.transfer_state,
          payload.device_message,
          payload.command_messages,
          payload.inbound_expected,
          payload.inbound_transferred,
          payload.inbound_transfer_state,
          payload.session_poisoned,
      };
    } catch (...) {
      *error = nullptr;
    }
  }
  return payload.status;
}

bool valid_utf8(const std::string_view value) noexcept {
  const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
  size_t index = 0;
  while (index < value.size()) {
    const unsigned char lead = bytes[index];
    if (lead <= 0x7FU) {
      ++index;
      continue;
    }

    size_t continuation_count = 0;
    unsigned char first_minimum = 0x80U;
    unsigned char first_maximum = 0xBFU;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      continuation_count = 1;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation_count = 2;
      if (lead == 0xE0U) {
        first_minimum = 0xA0U;
      } else if (lead == 0xEDU) {
        first_maximum = 0x9FU;
      }
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation_count = 3;
      if (lead == 0xF0U) {
        first_minimum = 0x90U;
      } else if (lead == 0xF4U) {
        first_maximum = 0x8FU;
      }
    } else {
      return false;
    }

    if (continuation_count > value.size() - index - 1) {
      return false;
    }
    const unsigned char first = bytes[index + 1];
    if (first < first_minimum || first > first_maximum) {
      return false;
    }
    for (size_t offset = 2; offset <= continuation_count; ++offset) {
      const unsigned char continuation = bytes[index + offset];
      if (continuation < 0x80U || continuation > 0xBFU) {
        return false;
      }
    }
    index += continuation_count + 1;
  }
  return true;
}

bool valid_fastboot_parameter(const std::string_view value,
                              const size_t prefix_size) noexcept {
  if (value.empty() ||
      value.size() >
          kairosboot::protocol::kDefaultMaxCommandBytes - prefix_size) {
    return false;
  }
  for (const unsigned char character : value) {
    if (character < 0x20U || character > 0x7EU) {
      return false;
    }
  }
  return true;
}

bool valid_partition_name_characters(const std::string_view value) noexcept {
  return std::ranges::all_of(value, [](const unsigned char character) {
    const auto ascii_alphanumeric =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
    return ascii_alphanumeric || character == '_' || character == '-' ||
           character == '.';
  });
}

size_t decimal_digit_count(std::uint64_t value) noexcept {
  size_t result = 1;
  while (value >= 10) {
    value /= 10;
    ++result;
  }
  return result;
}

kb_status_t validate_logical_partition_name(
    const char *name, const size_t command_overhead,
    const char *device_selector, kb_error_t **error) noexcept {
  if (name == nullptr || name[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "logical partition name must not be empty", device_selector);
  }
  const std::string_view value{name};
  if (!valid_partition_name_characters(value)) {
    return fail(
        error, KB_E_INVALID_ARGUMENT,
        "logical partition name must use ASCII letters, digits, '.', '-' or '_'",
        device_selector);
  }
  if (!valid_fastboot_parameter(value, command_overhead)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "logical partition name must be printable ASCII and the command must fit the Fastboot limit",
                device_selector);
  }
  return KB_OK;
}

bool valid_fetch_partition(const std::string_view value) noexcept {
  return !value.empty() && valid_partition_name_characters(value);
}

size_t fetch_range_component_size(const uint64_t value) noexcept {
  size_t digits = 1;
  for (auto remaining = value; remaining >= 16; remaining /= 16) {
    ++digits;
  }
  return 3U + std::max<size_t>(8U, digits);
}

std::filesystem::path utf8_path(const std::string_view value) {
#if defined(_WIN32)
  std::u8string converted;
  converted.reserve(value.size());
  for (const unsigned char character : value) {
    converted.push_back(static_cast<char8_t>(character));
  }
  return std::filesystem::path{converted};
#else
  return std::filesystem::path{value};
#endif
}

kairosboot::transport::UsbInterfaceFilter fastboot_usb_filter() {
  kairosboot::transport::UsbInterfaceFilter filter;
  filter.interface_class = 0xFF;
  filter.interface_subclass = 0x42;
  filter.interface_protocol = 0x03;
  return filter;
}

std::string physical_usb_path(
    const kairosboot::transport::UsbDeviceInfo &device);

std::string device_identifier(
    const kairosboot::transport::UsbDeviceInfo &device) {
  return device.serial_utf8.empty() ? physical_usb_path(device)
                                    : device.serial_utf8;
}

kairosboot::api::OperationOutcome cancelled_operation(
    std::string device, const kb_transfer_state_t transfer_state,
    const char *message = "operation cancelled") {
  return kairosboot::api::OperationOutcome::cancelled({
      .status = KB_E_CANCELLED,
      .message = message,
      .native_code = 0,
      .transfer_state = transfer_state,
      .device_identifier = std::move(device),
      .device_message = {},
      .command_messages = {},
      .inbound_expected = std::nullopt,
      .inbound_transferred = 0,
      .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
      .session_poisoned = false,
  });
}

kairosboot::api::OperationOutcome operation_failure(
    kairosboot::api::OperationErrorPayload payload) {
  if (payload.status == KB_E_CANCELLED) {
    return kairosboot::api::OperationOutcome::cancelled(std::move(payload));
  }
  return kairosboot::api::OperationOutcome::failed(std::move(payload));
}

bool report_progress(const kb_flash_options_t &options,
                     const uint64_t completed, const uint64_t total,
                     const char *stage,
                     const std::string &device) noexcept {
  if (options.progress_callback == nullptr) {
    return true;
  }
  const kb_progress_t progress{
      sizeof(kb_progress_t),
      KB_API_VERSION,
      completed,
      total,
      stage,
      device.c_str(),
  };
  try {
    return options.progress_callback(&progress, options.progress_user_data) !=
           KB_PROGRESS_CANCEL;
  } catch (...) {
    return false;
  }
}

bool valid_context_options(const kb_context_options_t *options) noexcept {
  return options == nullptr ||
         (options->struct_size >= KB_CONTEXT_OPTIONS_V1_SIZE &&
          options->api_version == KB_API_VERSION);
}

bool valid_flash_options(const kb_flash_options_t *options) noexcept {
  return options == nullptr ||
         (options->struct_size >= KB_FLASH_OPTIONS_V1_SIZE &&
          options->api_version == KB_API_VERSION);
}

bool valid_update_options(const kb_update_options_t *options) noexcept {
  return options == nullptr ||
         (options->struct_size >= KB_UPDATE_OPTIONS_V1_SIZE &&
          options->api_version == KB_API_VERSION &&
          (options->wipe == 0 || options->wipe == 1));
}

[[nodiscard]] kb_update_options_t update_options_or_default(
    const kb_update_options_t *options) noexcept {
  kb_update_options_t result{};
  result.struct_size = sizeof(result);
  result.api_version = KB_API_VERSION;
  result.timeout_ms = kDefaultTimeoutMs;
  if (options != nullptr) {
    result.timeout_ms = options->timeout_ms;
    result.wipe = options->wipe;
    result.progress_callback = options->progress_callback;
    result.progress_user_data = options->progress_user_data;
  }
  return result;
}

[[nodiscard]] bool report_update_progress(
    const kb_update_options_t &options, const std::uint64_t completed,
    const std::uint64_t total, const char *stage,
    const std::string &device) noexcept {
  if (options.progress_callback == nullptr) {
    return true;
  }
  const kb_progress_t progress{
      sizeof(kb_progress_t), KB_API_VERSION, completed, total, stage,
      device.c_str()};
  try {
    return options.progress_callback(&progress, options.progress_user_data) !=
           KB_PROGRESS_CANCEL;
  } catch (...) {
    return false;
  }
}

using UpdateClock = std::chrono::steady_clock;

[[nodiscard]] kairosboot::api::OperationErrorPayload update_error(
    const kb_status_t status, std::string message,
    const std::string_view identifier,
    const kb_transfer_state_t transfer_state = KB_TRANSFER_NOT_SENT) {
  return {
      .status = status,
      .message = std::move(message),
      .native_code = 0,
      .transfer_state = transfer_state,
      .device_identifier = std::string(identifier),
      .device_message = {},
      .command_messages = {},
      .inbound_expected = std::nullopt,
      .inbound_transferred = 0,
      .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
      .session_poisoned = false,
  };
}

[[nodiscard]] std::expected<UpdateClock::time_point,
                            kairosboot::api::OperationErrorPayload>
update_deadline(const UpdateClock::time_point started,
                const std::uint32_t timeout_ms,
                const std::string_view identifier) {
  if (timeout_ms == KB_WAIT_INFINITE) {
    return UpdateClock::time_point::max();
  }
  const auto duration = std::chrono::duration_cast<UpdateClock::duration>(
      std::chrono::milliseconds{timeout_ms});
  if (duration > UpdateClock::time_point::max() - started) {
    return std::unexpected(update_error(
        KB_E_INVALID_ARGUMENT,
        "update operation timeout overflows steady_clock", identifier));
  }
  return started + duration;
}

[[nodiscard]] std::expected<std::uint32_t,
                            kairosboot::api::OperationErrorPayload>
remaining_update_timeout(const UpdateClock::time_point deadline,
                         const std::string_view identifier,
                         const std::string_view phase) {
  if (deadline == UpdateClock::time_point::max()) {
    return KB_WAIT_INFINITE;
  }
  const auto now = UpdateClock::now();
  if (now >= deadline) {
    return std::unexpected(update_error(
        KB_E_TIMEOUT,
        "update operation deadline expired before " + std::string(phase),
        identifier));
  }
  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  if (remaining <= 0) {
    return std::unexpected(update_error(
        KB_E_TIMEOUT,
        "update operation deadline expired before " + std::string(phase),
        identifier));
  }
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      static_cast<std::uint64_t>(remaining),
      static_cast<std::uint64_t>(KB_WAIT_INFINITE) - 1U));
}

const char *runtime_error_message(
    const kairosboot::transport::LibusbRuntimeErrorKind kind) noexcept {
  using kairosboot::transport::LibusbRuntimeErrorKind;
  switch (kind) {
  case LibusbRuntimeErrorKind::invalid_function_table:
    return "The libusb function table is incomplete.";
  case LibusbRuntimeErrorKind::version_mismatch:
    return "KairosBoot requires exactly libusb 1.0.30.";
  case LibusbRuntimeErrorKind::already_running:
    return "A different libusb runtime already owns the process context.";
  case LibusbRuntimeErrorKind::init_failed:
    return "libusb context initialization failed.";
  case LibusbRuntimeErrorKind::event_thread_failed:
    return "The libusb event thread could not be started.";
  case LibusbRuntimeErrorKind::event_loop_failed:
    return "The libusb event loop failed.";
  case LibusbRuntimeErrorKind::runtime_stopped:
    return "The libusb runtime is stopped.";
  case LibusbRuntimeErrorKind::enumeration_failed:
    return "USB device enumeration failed.";
  case LibusbRuntimeErrorKind::invalid_device:
    return "The USB device snapshot is invalid.";
  case LibusbRuntimeErrorKind::device_not_found:
    return "The USB device is no longer present at its physical path.";
  case LibusbRuntimeErrorKind::open_failed:
    return "The USB device could not be opened.";
  case LibusbRuntimeErrorKind::configuration_failed:
    return "The USB device configuration could not be selected.";
  case LibusbRuntimeErrorKind::interface_busy:
    return "The Fastboot USB interface is already in use.";
  case LibusbRuntimeErrorKind::claim_failed:
    return "The Fastboot USB interface could not be claimed.";
  case LibusbRuntimeErrorKind::alternate_setting_failed:
    return "The Fastboot USB alternate setting could not be selected.";
  case LibusbRuntimeErrorKind::operation_cancelled:
    return "The USB open operation was cancelled at a safe stage boundary.";
  case LibusbRuntimeErrorKind::operation_timed_out:
    return "The USB open operation exceeded its deadline at a safe stage boundary.";
  case LibusbRuntimeErrorKind::identity_changed:
    return "The USB device identity changed while its interface was being opened.";
  }
  return "An unknown libusb runtime error occurred.";
}

kb_status_t runtime_error_status(
    const kairosboot::transport::LibusbRuntimeErrorKind kind) noexcept {
  using kairosboot::transport::LibusbRuntimeErrorKind;
  switch (kind) {
  case LibusbRuntimeErrorKind::version_mismatch:
    return KB_E_NOT_SUPPORTED;
  case LibusbRuntimeErrorKind::already_running:
  case LibusbRuntimeErrorKind::interface_busy:
    return KB_E_BUSY;
  case LibusbRuntimeErrorKind::invalid_function_table:
  case LibusbRuntimeErrorKind::invalid_device:
    return KB_E_INTERNAL;
  case LibusbRuntimeErrorKind::device_not_found:
  case LibusbRuntimeErrorKind::identity_changed:
    return KB_E_NO_DEVICE;
  case LibusbRuntimeErrorKind::operation_cancelled:
    return KB_E_CANCELLED;
  case LibusbRuntimeErrorKind::operation_timed_out:
    return KB_E_TIMEOUT;
  case LibusbRuntimeErrorKind::init_failed:
  case LibusbRuntimeErrorKind::event_thread_failed:
  case LibusbRuntimeErrorKind::event_loop_failed:
  case LibusbRuntimeErrorKind::runtime_stopped:
  case LibusbRuntimeErrorKind::enumeration_failed:
  case LibusbRuntimeErrorKind::open_failed:
  case LibusbRuntimeErrorKind::configuration_failed:
  case LibusbRuntimeErrorKind::claim_failed:
  case LibusbRuntimeErrorKind::alternate_setting_failed:
    return KB_E_IO;
  }
  return KB_E_INTERNAL;
}

std::expected<std::shared_ptr<kairosboot::transport::LibusbRuntime>,
              kairosboot::transport::LibusbRuntimeError>
acquire_usb_runtime() {
  std::lock_guard lock(g_usb_runtime_mutex);
  if (auto active = g_usb_runtime.lock()) {
    if (active->running()) {
      return active;
    }
    return std::unexpected(kairosboot::transport::LibusbRuntimeError{
        kairosboot::transport::LibusbRuntimeErrorKind::runtime_stopped});
  }

  auto created = kairosboot::transport::LibusbRuntime::create();
  if (!created) {
    return std::unexpected(created.error());
  }
  g_usb_runtime = *created;
  return *created;
}

std::expected<std::shared_ptr<kairosboot::transport::LibusbRuntime>,
              kairosboot::transport::LibusbRuntimeError>
acquire_context_usb_runtime(kb_context_t &context) {
  if (context.usb_state == nullptr) {
    return std::unexpected(kairosboot::transport::LibusbRuntimeError{
        .kind = kairosboot::transport::LibusbRuntimeErrorKind::init_failed,
        .native_code = 0,
        .actual_major = 0,
        .actual_minor = 0,
        .actual_micro = 0,
    });
  }
  std::scoped_lock lock(context.usb_state->usb_runtime_mutex);
  if (context.usb_state->usb_runtime != nullptr &&
      context.usb_state->usb_runtime->running()) {
    return context.usb_state->usb_runtime;
  }
  auto runtime = acquire_usb_runtime();
  if (!runtime) {
    return std::unexpected(runtime.error());
  }
  context.usb_state->usb_runtime = *runtime;
  return *runtime;
}

std::expected<std::shared_ptr<kairosboot::transport::LibusbRuntime>,
              kairosboot::transport::LibusbRuntimeError>
acquire_context_usb_runtime(
    const std::shared_ptr<kb_context_usb_state> &state) {
  if (state == nullptr) {
    return std::unexpected(kairosboot::transport::LibusbRuntimeError{
        .kind = kairosboot::transport::LibusbRuntimeErrorKind::init_failed,
        .native_code = 0,
        .actual_major = 0,
        .actual_minor = 0,
        .actual_micro = 0,
    });
  }
  std::scoped_lock lock(state->usb_runtime_mutex);
  if (state->usb_runtime != nullptr && state->usb_runtime->running()) {
    return state->usb_runtime;
  }
  auto runtime = acquire_usb_runtime();
  if (!runtime) {
    return std::unexpected(runtime.error());
  }
  state->usb_runtime = *runtime;
  return *runtime;
}

std::string physical_usb_path(
    const kairosboot::transport::UsbDeviceInfo &device) {
  if (device.port_path.empty()) {
    return {};
  }
  std::string result = "usb:" + std::to_string(device.bus_number) + "-";
  for (size_t index = 0; index < device.port_path.size(); ++index) {
    if (index != 0) {
      result.push_back('.');
    }
    result += std::to_string(device.port_path[index]);
  }
  return result;
}

const char *device_field(const kb_device_list_t *devices, size_t index,
                         const std::string kb_device_info::*field) noexcept {
  if (devices == nullptr || index >= devices->devices.size()) {
    return nullptr;
  }
  return (devices->devices[index].*field).c_str();
}

kb_operation_state_t public_operation_state(
    const kairosboot::api::OperationPhase phase) noexcept {
  using kairosboot::api::OperationPhase;
  switch (phase) {
  case OperationPhase::Created:
    return KB_OPERATION_CREATED;
  case OperationPhase::Running:
    return KB_OPERATION_RUNNING;
  case OperationPhase::Succeeded:
    return KB_OPERATION_SUCCEEDED;
  case OperationPhase::Failed:
    return KB_OPERATION_FAILED;
  case OperationPhase::Cancelled:
    return KB_OPERATION_CANCELLED;
  }
  return KB_OPERATION_FAILED;
}

const kb_error_t *materialize_operation_error(
    const kb_operation_t *operation) noexcept {
  if (operation == nullptr || operation->state == nullptr) {
    return nullptr;
  }
  const auto phase = operation->state->phase();
  if (phase != kairosboot::api::OperationPhase::Failed &&
      phase != kairosboot::api::OperationPhase::Cancelled) {
    return nullptr;
  }

  std::scoped_lock lock(operation->error_mutex);
  if (operation->public_error != nullptr) {
    return operation->public_error.get();
  }
  try {
    const auto payload = operation->state->error();
    if (!payload.has_value()) {
      return nullptr;
    }
    operation->public_error = std::make_unique<kb_error>(kb_error{
        payload->status,
        payload->message,
        payload->device_identifier,
        payload->native_code,
        payload->transfer_state,
        payload->device_message,
        payload->command_messages,
        payload->inbound_expected,
        payload->inbound_transferred,
        payload->inbound_transfer_state,
        payload->session_poisoned,
    });
    return operation->public_error.get();
  } catch (...) {
    return nullptr;
  }
}

class MemorySink final : public kairosboot::protocol::ITransferSink {
public:
  [[nodiscard]] kairosboot::protocol::TransferResult write(
      const std::uint64_t offset,
      const std::span<const std::byte> source) noexcept override {
    if (offset != bytes_.size() ||
        source.size() > std::numeric_limits<std::size_t>::max() - bytes_.size()) {
      return {
          .status = kairosboot::protocol::TransportStatus::IoError,
          .transferred = 0,
          .certainty = kairosboot::protocol::TransferCertainty::NotTransferred,
          .detail = "memory receive sink offset is invalid",
      };
    }
    try {
      bytes_.insert(bytes_.end(), source.begin(), source.end());
      return {
          .status = kairosboot::protocol::TransportStatus::Ok,
          .transferred = source.size(),
          .certainty =
              kairosboot::protocol::TransferCertainty::FullyTransferred,
      };
    } catch (...) {
      return {
          .status = kairosboot::protocol::TransportStatus::IoError,
          .transferred = 0,
          .certainty = kairosboot::protocol::TransferCertainty::NotTransferred,
          .detail = "memory receive sink allocation failed",
      };
    }
  }

  [[nodiscard]] std::vector<std::byte> take() noexcept {
    return std::move(bytes_);
  }

private:
  std::vector<std::byte> bytes_;
};

class MemorySource final : public kairosboot::protocol::ITransferSource {
public:
  explicit MemorySource(std::vector<std::byte> bytes) noexcept
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] bool read_exact(
      const std::uint64_t offset,
      const std::span<std::byte> destination) noexcept override {
    if (offset > bytes_.size() ||
        destination.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
      return false;
    }
    std::copy_n(
        bytes_.data() + static_cast<std::size_t>(offset), destination.size(),
        destination.data());
    return true;
  }

private:
  std::vector<std::byte> bytes_;
};

// TCP and UDP transports expose the common byte-stream contract but do not
// need a specialized USB transfer ring. This adapter streams an immutable
// source in bounded chunks so public stage cancellation/progress has identical
// semantics across transports without materializing a second payload copy.

struct PreparedTarget final {
  kairosboot::api::DeviceSelector selector;
  std::shared_ptr<kairosboot::transport::LibusbRuntime> usb_runtime;
  std::optional<kairosboot::transport::UsbDeviceInfo> usb_device;
};

struct PrimitiveExecution final {
  kairosboot::fastboot::PrimitiveReply reply;
  std::vector<std::byte> data;
};

[[nodiscard]] std::expected<PrimitiveExecution,
                            kairosboot::fastboot::PrimitiveError>
primitive_execution(
    std::expected<kairosboot::fastboot::PrimitiveReply,
                  kairosboot::fastboot::PrimitiveError> reply) {
  if (!reply) {
    return std::unexpected(std::move(reply.error()));
  }
  return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
}

using PrimitiveExecutor = std::function<std::expected<
    PrimitiveExecution, kairosboot::fastboot::PrimitiveError>(
    kairosboot::fastboot::PrimitiveService &,
    kairosboot::api::OperationState::TaskContext &,
    const kb_command_options_t &,
    const std::string &)>;

[[nodiscard]] bool valid_command_options(
    const kb_command_options_t *options) noexcept {
  return options == nullptr ||
         (options->struct_size >= KB_COMMAND_OPTIONS_V1_SIZE &&
          options->api_version == KB_API_VERSION &&
          options->maximum_receive_bytes != 0);
}

[[nodiscard]] kb_command_options_t command_options_or_default(
    const kb_command_options_t *options) noexcept {
  kb_command_options_t result{};
  result.struct_size = sizeof(result);
  result.api_version = KB_API_VERSION;
  result.timeout_ms = kDefaultTimeoutMs;
  result.maximum_receive_bytes = kDefaultMaximumReceiveBytes;
  if (options != nullptr) {
    result.timeout_ms = options->timeout_ms;
    result.progress_callback = options->progress_callback;
    result.progress_user_data = options->progress_user_data;
    result.maximum_receive_bytes = options->maximum_receive_bytes;
  }
  return result;
}

[[nodiscard]] std::expected<kairosboot::api::DeviceSelector,
                            kairosboot::api::OperationErrorPayload>
parse_target_selector(const char *selector_text) {
  const auto text = selector_text == nullptr
                        ? std::optional<std::string_view>{}
                        : std::optional<std::string_view>{selector_text};
  auto parsed = kairosboot::api::parse_device_selector(text);
  if (!parsed) {
    return std::unexpected(kairosboot::api::OperationErrorPayload{
        .status = parsed.error().status,
        .message = parsed.error().message,
        .native_code = 0,
        .transfer_state = KB_TRANSFER_NOT_SENT,
        .device_identifier = selector_text == nullptr ? "" : selector_text,
        .device_message = {},
        .command_messages = {},
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
        .session_poisoned = false,
    });
  }
  return std::move(*parsed);
}

[[nodiscard]] std::expected<PreparedTarget,
                            kairosboot::api::OperationErrorPayload>
bind_target(
    kairosboot::api::DeviceSelector selector,
    std::shared_ptr<kairosboot::transport::LibusbRuntime> usb_runtime) {
  PreparedTarget target{
      .selector = std::move(selector),
      .usb_runtime = {},
      .usb_device = std::nullopt,
  };
  if (target.selector.kind == kairosboot::api::DeviceSelectorKind::Tcp ||
      target.selector.kind == kairosboot::api::DeviceSelectorKind::Udp) {
    return target;
  }
  if (usb_runtime == nullptr) {
    return std::unexpected(kairosboot::api::OperationErrorPayload{
        .status = KB_E_INTERNAL,
        .message = "prepared USB runtime is missing",
        .native_code = 0,
        .transfer_state = KB_TRANSFER_NOT_SENT,
        .device_identifier = target.selector.identifier,
        .device_message = {},
        .command_messages = {},
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
        .session_poisoned = false,
    });
  }

  auto devices = usb_runtime->enumerate(fastboot_usb_filter());
  if (!devices) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        devices.error(), target.selector.identifier));
  }
  auto selected = kairosboot::api::select_usb_device(*devices, target.selector);
  if (!selected) {
    return std::unexpected(kairosboot::api::OperationErrorPayload{
        .status = selected.error().status,
        .message = selected.error().message,
        .native_code = 0,
        .transfer_state = KB_TRANSFER_NOT_SENT,
        .device_identifier = target.selector.identifier,
        .device_message = {},
        .command_messages = {},
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
        .session_poisoned = false,
    });
  }
  target.usb_runtime = std::move(usb_runtime);
  target.selector.identifier = device_identifier(*selected);
  target.usb_device = std::move(*selected);
  return target;
}

[[nodiscard]] std::expected<PreparedTarget,
                            kairosboot::api::OperationErrorPayload>
prepare_target(kb_context_t &context, const char *selector_text) {
  auto parsed = parse_target_selector(selector_text);
  if (!parsed) {
    return std::unexpected(std::move(parsed.error()));
  }

  if (parsed->kind == kairosboot::api::DeviceSelectorKind::Tcp ||
      parsed->kind == kairosboot::api::DeviceSelectorKind::Udp) {
    return bind_target(std::move(*parsed), {});
  }

  auto runtime = acquire_context_usb_runtime(context);
  if (!runtime) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        runtime.error(), parsed->identifier));
  }
  return bind_target(std::move(*parsed), std::move(*runtime));
}

[[nodiscard]] std::expected<PreparedTarget,
                            kairosboot::api::OperationErrorPayload>
prepare_flash_target(
    kb_context_t &context,
    const std::optional<std::string_view> legacy_serial) {
  const auto requested_identifier =
      legacy_serial.has_value() ? std::string{*legacy_serial} : std::string{};
  const bool network_selector =
      legacy_serial.has_value() &&
      (legacy_serial->starts_with("tcp:") ||
       legacy_serial->starts_with("udp:"));
  if (network_selector) {
    return prepare_target(context, requested_identifier.c_str());
  }

  auto runtime = acquire_context_usb_runtime(context);
  if (!runtime) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        runtime.error(), requested_identifier));
  }
  auto enumerated = (*runtime)->enumerate(fastboot_usb_filter());
  if (!enumerated) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        enumerated.error(), requested_identifier));
  }
  auto selected =
      kairosboot::api::select_usb_device(*enumerated, legacy_serial);
  if (!selected) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        selected.error(), requested_identifier));
  }

  const auto selected_identifier = device_identifier(*selected);
  return PreparedTarget{
      .selector =
          {
              .kind = legacy_serial.has_value()
                          ? kairosboot::api::DeviceSelectorKind::UsbSerial
                          : kairosboot::api::DeviceSelectorKind::UsbUnique,
              .value = requested_identifier,
              .usb_bus = 0,
              .usb_ports = {},
              .identifier = selected_identifier,
          },
      .usb_runtime = std::move(*runtime),
      .usb_device = std::move(*selected),
  };
}

[[nodiscard]] kairosboot::api::OperationErrorPayload network_error(
    const kairosboot::transport::TcpError &error,
    const std::string &identifier) {
  kb_status_t status = KB_E_IO;
  switch (error.kind) {
  case kairosboot::transport::TcpErrorKind::InvalidEndpoint:
    status = KB_E_INVALID_ARGUMENT;
    break;
  case kairosboot::transport::TcpErrorKind::Timeout:
    status = KB_E_TIMEOUT;
    break;
  case kairosboot::transport::TcpErrorKind::Cancelled:
    status = KB_E_CANCELLED;
    break;
  case kairosboot::transport::TcpErrorKind::ResolveFailed:
  case kairosboot::transport::TcpErrorKind::ConnectFailed:
  case kairosboot::transport::TcpErrorKind::HandshakeFailed:
  case kairosboot::transport::TcpErrorKind::Disconnected:
  case kairosboot::transport::TcpErrorKind::Io:
    status = KB_E_IO;
    break;
  }
  return {
      .status = status,
      .message = error.message,
      .native_code = error.native_error,
      .transfer_state = KB_TRANSFER_NOT_SENT,
      .device_identifier = identifier,
      .device_message = {},
      .command_messages = {},
      .inbound_expected = std::nullopt,
      .inbound_transferred = 0,
      .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
      .session_poisoned = false,
  };
}

[[nodiscard]] kairosboot::api::OperationErrorPayload network_error(
    const kairosboot::transport::UdpError &error,
    const std::string &identifier) {
  kb_status_t status = KB_E_IO;
  switch (error.kind) {
  case kairosboot::transport::UdpErrorKind::InvalidEndpoint:
    status = KB_E_INVALID_ARGUMENT;
    break;
  case kairosboot::transport::UdpErrorKind::Timeout:
    status = KB_E_TIMEOUT;
    break;
  case kairosboot::transport::UdpErrorKind::Cancelled:
    status = KB_E_CANCELLED;
    break;
  case kairosboot::transport::UdpErrorKind::Protocol:
    status = KB_E_PROTOCOL;
    break;
  case kairosboot::transport::UdpErrorKind::ResolveFailed:
  case kairosboot::transport::UdpErrorKind::SocketFailed:
  case kairosboot::transport::UdpErrorKind::HandshakeFailed:
  case kairosboot::transport::UdpErrorKind::Io:
    status = KB_E_IO;
    break;
  }
  return {
      .status = status,
      .message = error.message,
      .native_code = error.native_error,
      .transfer_state = KB_TRANSFER_NOT_SENT,
      .device_identifier = identifier,
      .device_message = {},
      .command_messages = {},
      .inbound_expected = std::nullopt,
      .inbound_transferred = 0,
      .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
      .session_poisoned = false,
  };
}

class UpdateDeadlineTransport final
    : public kairosboot::protocol::ITransportSession,
      public kairosboot::protocol::IStreamingTransportSession {
public:
  UpdateDeadlineTransport(
      std::unique_ptr<kairosboot::protocol::ITransportSession> transport,
      const UpdateClock::time_point deadline)
      : transport_(std::move(transport)), deadline_(deadline) {}

  [[nodiscard]] kairosboot::protocol::TransferResult write(
      const std::span<const std::byte> bytes,
      const std::chrono::milliseconds timeout) override {
    auto bounded = bounded_timeout(timeout);
    return bounded ? transport_->write(bytes, *bounded)
                   : deadline_failure("write");
  }

  [[nodiscard]] kairosboot::protocol::TransferResult read(
      const std::span<std::byte> destination,
      const std::chrono::milliseconds timeout) override {
    auto bounded = bounded_timeout(timeout);
    return bounded ? transport_->read(destination, *bounded)
                   : deadline_failure("read");
  }

  [[nodiscard]] kairosboot::protocol::TransferResult read_data(
      const std::span<std::byte> destination,
      const std::chrono::milliseconds timeout) override {
    auto bounded = bounded_timeout(timeout);
    return bounded ? transport_->read_data(destination, *bounded)
                   : deadline_failure("data read");
  }

  [[nodiscard]] kairosboot::protocol::TransferResult write_source(
      std::shared_ptr<kairosboot::protocol::ITransferSource> source,
      const std::chrono::milliseconds timeout,
      const kairosboot::protocol::TransferProgressObserver &observer) override {
    auto bounded = bounded_timeout(timeout);
    if (!bounded) {
      return deadline_failure("streaming write");
    }
    auto *streaming = dynamic_cast<
        kairosboot::protocol::IStreamingTransportSession *>(transport_.get());
    if (streaming == nullptr) {
      return {
          .status = kairosboot::protocol::TransportStatus::IoError,
          .transferred = 0,
          .certainty =
              kairosboot::protocol::TransferCertainty::NotTransferred,
          .truncated = false,
          .detail = "the deadline transport has no streaming backend",
          .native_code = 0,
      };
    }
    return streaming->write_source(std::move(source), *bounded, observer);
  }

  void request_cancel() noexcept override { transport_->request_cancel(); }
  void close() noexcept override { transport_->close(); }

private:
  [[nodiscard]] std::optional<std::chrono::milliseconds> bounded_timeout(
      const std::chrono::milliseconds requested) const noexcept {
    const auto now = UpdateClock::now();
    if (now >= deadline_) {
      return std::nullopt;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        deadline_ - now);
    return std::min(requested, remaining);
  }

  [[nodiscard]] static kairosboot::protocol::TransferResult deadline_failure(
      const std::string_view operation) {
    return {
        .status = kairosboot::protocol::TransportStatus::Timeout,
        .transferred = 0,
        .certainty = kairosboot::protocol::TransferCertainty::NotTransferred,
        .truncated = false,
        .detail = "update operation deadline expired before transport " +
                  std::string(operation),
        .native_code = 0,
    };
  }

  std::unique_ptr<kairosboot::protocol::ITransportSession> transport_;
  UpdateClock::time_point deadline_;
};

[[nodiscard]] std::unique_ptr<kairosboot::protocol::ITransportSession>
with_update_deadline(
    std::unique_ptr<kairosboot::protocol::ITransportSession> transport,
    const UpdateClock::time_point deadline) {
  if (deadline == UpdateClock::time_point::max()) {
    return transport;
  }
  return std::make_unique<UpdateDeadlineTransport>(std::move(transport),
                                                   deadline);
}

[[nodiscard]] std::expected<
    std::unique_ptr<kairosboot::protocol::ITransportSession>,
    kairosboot::api::OperationErrorPayload>
open_target(
    const PreparedTarget &target,
    const kb_command_options_t &options,
    const std::stop_token cancellation,
    const UpdateClock::time_point update_deadline =
        UpdateClock::time_point::max()) {
  const auto timeout = std::chrono::milliseconds{options.timeout_ms};
  if (target.selector.kind == kairosboot::api::DeviceSelectorKind::Tcp) {
    kairosboot::transport::TcpTransportOptions transport_options;
    transport_options.connect_timeout = timeout;
    transport_options.handshake_timeout = timeout;
    transport_options.cancellation = cancellation;
    auto connected = kairosboot::transport::connect_tcp_fastboot(
        target.selector.value, transport_options);
    if (!connected) {
      return std::unexpected(network_error(
          connected.error(), target.selector.identifier));
    }
    std::unique_ptr<kairosboot::protocol::ITransportSession> native =
        std::move(*connected);
    return kairosboot::transport::make_sequential_streaming_transport(
        with_update_deadline(std::move(native), update_deadline));
  }
  if (target.selector.kind == kairosboot::api::DeviceSelectorKind::Udp) {
    kairosboot::transport::UdpTransportOptions transport_options;
    transport_options.connect_timeout = timeout;
    transport_options.query_timeout = timeout;
    transport_options.initialization_timeout = timeout;
    transport_options.cancellation = cancellation;
    auto connected = kairosboot::transport::connect_udp_fastboot(
        target.selector.value, transport_options);
    if (!connected) {
      return std::unexpected(network_error(
          connected.error(), target.selector.identifier));
    }
    std::unique_ptr<kairosboot::protocol::ITransportSession> native =
        std::move(*connected);
    return kairosboot::transport::make_sequential_streaming_transport(
        with_update_deadline(std::move(native), update_deadline));
  }
  if (target.usb_runtime == nullptr || !target.usb_device.has_value()) {
    return std::unexpected(kairosboot::api::OperationErrorPayload{
        .status = KB_E_INTERNAL,
        .message = "prepared USB target is incomplete",
        .native_code = 0,
        .transfer_state = KB_TRANSFER_NOT_SENT,
        .device_identifier = target.selector.identifier,
        .device_message = {},
        .command_messages = {},
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0,
        .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
        .session_poisoned = false,
    });
  }
  kairosboot::transport::UsbFastbootTransportOptions transport_options;
  transport_options.bulk_out.timeout_ms = options.timeout_ms;
  auto opened = kairosboot::transport::UsbFastbootTransport::open(
      target.usb_runtime, *target.usb_device, transport_options);
  if (!opened) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        opened.error(), target.selector.identifier));
  }
  std::unique_ptr<kairosboot::protocol::ITransportSession> native =
      std::move(*opened);
  return with_update_deadline(std::move(native), update_deadline);
}

[[nodiscard]] std::vector<kairosboot::api::CommandMessagePayload>
command_messages(const std::vector<kairosboot::protocol::Response> &responses) {
  std::vector<kairosboot::api::CommandMessagePayload> result;
  result.reserve(responses.size());
  for (const auto &response : responses) {
    if (response.kind != kairosboot::protocol::ResponseKind::Info &&
        response.kind != kairosboot::protocol::ResponseKind::Text) {
      continue;
    }
    result.push_back({
        response.kind == kairosboot::protocol::ResponseKind::Text
            ? kairosboot::api::CommandMessageKind::Text
            : kairosboot::api::CommandMessageKind::Info,
        response.payload,
    });
  }
  return result;
}

[[nodiscard]] std::shared_ptr<const kairosboot::api::CommandResultPayload>
make_command_result(
    PrimitiveExecution execution,
    const std::string &identifier) {
  auto result = std::make_shared<kairosboot::api::CommandResultPayload>();
  result->terminal_payload = std::move(execution.reply.terminal.payload);
  result->messages = command_messages(execution.reply.informational);
  result->data = std::move(execution.data);
  result->device_identifier = identifier;
  return result;
}

[[nodiscard]] const kairosboot::api::CommandResultPayload *
command_result_payload(const kb_command_result_t *result) noexcept {
  return result == nullptr ? nullptr : result->handle.get();
}

[[nodiscard]] bool report_command_progress(
    const kb_command_options_t &options,
    const std::uint64_t completed,
    const std::uint64_t total,
    const char *stage,
    const std::string &identifier) noexcept {
  if (options.progress_callback == nullptr) {
    return true;
  }
  const kb_progress_t progress{
      sizeof(kb_progress_t), KB_API_VERSION, completed, total, stage,
      identifier.c_str()};
  try {
    return options.progress_callback(&progress, options.progress_user_data) !=
           KB_PROGRESS_CANCEL;
  } catch (...) {
    return false;
  }
}

kb_status_t start_primitive_async(
    kb_context_t *context,
    const char *selector_text,
    const kb_command_options_t *options,
    PrimitiveExecutor executor,
    kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (context == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "context must not be null");
  }
  if (!valid_command_options(options)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "command options have an incompatible size, API version, or receive bound",
                selector_text);
  }

  try {
    auto target = prepare_target(*context, selector_text);
    if (!target) {
      return fail(error, target.error());
    }
    auto copied_options = command_options_or_default(options);
    auto identifier = target->selector.identifier;
    auto task = [target = std::move(*target), copied_options,
                 executor = std::move(executor),
                 identifier = std::move(identifier)](
                    kairosboot::api::OperationState::TaskContext &task_context)
        mutable -> kairosboot::api::OperationOutcome {
      if (task_context.cancel_requested()) {
        return cancelled_operation(identifier, KB_TRANSFER_NOT_SENT);
      }
      auto transport = open_target(
          target, copied_options, task_context.cancellation_token());
      if (!transport) {
        return operation_failure(std::move(transport.error()));
      }
      kairosboot::protocol::SessionOptions session_options;
      session_options.io_timeout =
          std::chrono::milliseconds{copied_options.timeout_ms};
      kairosboot::protocol::FastbootSession session(
          std::move(*transport), session_options);
      kairosboot::fastboot::PrimitiveService service(session);
      auto cancellation = task_context.register_cancellation_hook(
          [&service] { service.request_cancel(); });
      auto executed = executor(
          service, task_context, copied_options, identifier);
      if (!executed) {
        return operation_failure(kairosboot::api::normalize_public_error(
            executed.error(), identifier));
      }
      return kairosboot::api::OperationOutcome::succeeded(
          make_command_result(std::move(*executed), identifier));
    };
    auto result = std::make_unique<kb_operation>(std::move(task));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL,
                  "unable to start the Fastboot command operation",
                  selector_text);
    }
    *operation = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the Fastboot command operation",
                selector_text);
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the Fastboot command operation",
                selector_text);
  }
}

kb_status_t finish_blocking_command(
    kb_operation_t *operation,
    kb_command_result_t **result,
    kb_error_t **error) {
  if (result == nullptr) {
    kb_operation_release(operation);
    return fail(error, KB_E_INVALID_ARGUMENT,
                "command result output pointer must not be null");
  }
  *result = nullptr;
  const auto waited = kb_operation_wait(operation, KB_WAIT_INFINITE);
  if (waited != KB_OK) {
    const auto *operation_error = kb_operation_error(operation);
    if (operation_error != nullptr) {
      const auto payload = operation->state->error();
      if (payload.has_value()) {
        static_cast<void>(fail(error, *payload));
      }
    } else {
      static_cast<void>(fail(error, waited, kb_status_string(waited)));
    }
    kb_operation_release(operation);
    return waited;
  }
  const auto extracted = kb_operation_command_result(operation, result, error);
  kb_operation_release(operation);
  return extracted;
}

template <typename AsyncStart, typename... Arguments>
kb_status_t run_blocking_command(
    AsyncStart async_start,
    kb_command_result_t **result,
    kb_error_t **error,
    Arguments &&...arguments) {
  clear_error(error);
  if (result == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "command result output pointer must not be null");
  }
  *result = nullptr;
  kb_operation_t *operation = nullptr;
  const auto started = async_start(
      std::forward<Arguments>(arguments)..., &operation, error);
  if (started != KB_OK) {
    return started;
  }
  return finish_blocking_command(operation, result, error);
}

class UpdateProgressCancelled final : public std::exception {
public:
  [[nodiscard]] const char *what() const noexcept override {
    return "update operation cancelled by the progress callback";
  }
};

[[nodiscard]] const char *update_event_stage(
    const kairosboot::fastboot::UpdateExecutionEventKind kind) noexcept {
  using kairosboot::fastboot::UpdateExecutionEventKind;
  switch (kind) {
  case UpdateExecutionEventKind::ValidationStarted:
  case UpdateExecutionEventKind::PreparedPackageValidated:
  case UpdateExecutionEventKind::RequirementSatisfied:
  case UpdateExecutionEventKind::RequirementSkipped:
  case UpdateExecutionEventKind::RequirementFailed:
    return "validate";
  case UpdateExecutionEventKind::GetVarQuery:
  case UpdateExecutionEventKind::GetVarResult:
  case UpdateExecutionEventKind::GetVarCacheHit:
    return "getvar";
  case UpdateExecutionEventKind::ValidationCompleted:
    return "prepare";
  case UpdateExecutionEventKind::TaskStarted:
  case UpdateExecutionEventKind::TaskCompleted:
  case UpdateExecutionEventKind::TaskFailed:
  case UpdateExecutionEventKind::ExecutionCompleted:
    return "execute";
  }
  return "execute";
}

[[nodiscard]] kb_transfer_state_t completed_update_transfer_state(
    const std::size_t completed_tasks,
    const std::size_t total_tasks) noexcept {
  if (total_tasks != 0U && completed_tasks == total_tasks) {
    return KB_TRANSFER_FULLY_TRANSFERRED;
  }
  return completed_tasks == 0U ? KB_TRANSFER_NOT_SENT
                               : KB_TRANSFER_PARTIAL_OR_UNKNOWN;
}

[[nodiscard]] kairosboot::api::OperationOutcome run_prepared_public_update(
    const std::shared_ptr<kb_context_usb_state> &usb_state,
    kairosboot::api::DeviceSelector selector,
    const std::filesystem::path &package_path,
    const kb_update_options_t &options,
    kairosboot::api::OperationState::TaskContext &task_context) {
  auto identifier = selector.identifier;
  const auto started = UpdateClock::now();
  auto deadline = update_deadline(started, options.timeout_ms, identifier);
  if (!deadline) {
    return operation_failure(std::move(deadline.error()));
  }
  if (task_context.cancel_requested() ||
      !report_update_progress(options, 0U, 0U, "preflight", identifier)) {
    return cancelled_operation(identifier, KB_TRANSFER_NOT_SENT,
                               "update cancelled before package preflight");
  }

  kairosboot::image::ArtifactSourceResolver resolver;
  auto prepared = kairosboot::fastboot::preflight_update_package(
      resolver, package_path, options.wipe != 0,
      kairosboot::fastboot::UpdatePackagePreflightLimits{}, *deadline,
      task_context.cancellation_token());
  if (!prepared) {
    return operation_failure(kairosboot::api::normalize_public_error(
        prepared.error(), identifier));
  }
  const auto total_tasks = prepared->plan.tasks.size();
  if (task_context.cancel_requested() ||
      !report_update_progress(options, 0U, 0U, "select", identifier)) {
    return cancelled_operation(identifier, KB_TRANSFER_NOT_SENT,
                               "update cancelled before device selection");
  }
  if (task_context.cancel_requested()) {
    return cancelled_operation(identifier, KB_TRANSFER_NOT_SENT,
                               "update cancelled before device selection");
  }
  if (auto remaining = remaining_update_timeout(
          *deadline, identifier, "device selection");
      !remaining) {
    return operation_failure(std::move(remaining.error()));
  }

  std::expected<PreparedTarget, kairosboot::api::OperationErrorPayload> target =
      std::unexpected(update_error(
          KB_E_INTERNAL, "device target was not bound", identifier));
  if (selector.kind == kairosboot::api::DeviceSelectorKind::Tcp ||
      selector.kind == kairosboot::api::DeviceSelectorKind::Udp) {
    target = bind_target(std::move(selector), {});
  } else {
    auto runtime = acquire_context_usb_runtime(usb_state);
    if (!runtime) {
      return operation_failure(kairosboot::api::normalize_public_error(
          runtime.error(), identifier));
    }
    target = bind_target(std::move(selector), std::move(*runtime));
  }
  if (!target) {
    return operation_failure(std::move(target.error()));
  }
  identifier = target->selector.identifier;

  if (task_context.cancel_requested() ||
      !report_update_progress(options, 0U, 0U, "open", identifier)) {
    return cancelled_operation(identifier, KB_TRANSFER_NOT_SENT,
                               "update cancelled before transport open");
  }
  if (task_context.cancel_requested()) {
    return cancelled_operation(identifier, KB_TRANSFER_NOT_SENT,
                               "update cancelled before transport open");
  }
  auto open_timeout = remaining_update_timeout(
      *deadline, identifier, "transport open");
  if (!open_timeout) {
    return operation_failure(std::move(open_timeout.error()));
  }
  kb_command_options_t transport_options{};
  kb_command_options_init(&transport_options);
  transport_options.timeout_ms = *open_timeout;
  auto transport = open_target(
      *target, transport_options, task_context.cancellation_token(),
      *deadline);
  if (!transport) {
    return operation_failure(std::move(transport.error()));
  }

  auto protocol_timeout = remaining_update_timeout(
      *deadline, identifier, "device validation");
  if (!protocol_timeout) {
    return operation_failure(std::move(protocol_timeout.error()));
  }
  kairosboot::protocol::SessionOptions session_options{};
  session_options.io_timeout = std::chrono::milliseconds{*protocol_timeout};
  kairosboot::protocol::FastbootSession session(
      std::move(*transport), session_options);
  kairosboot::fastboot::PrimitiveService service(session);

  bool callback_cancelled = false;
  kairosboot::fastboot::PrimitiveUpdateDeviceOptions device_options{
      .host_resparse_limit = kairosboot::image::kDefaultResparseLimitBytes,
      .progress = [&options, &identifier](
                      const kairosboot::fastboot::PrimitiveUpdateProgress
                          &progress) {
        return report_update_progress(
                   options, progress.completed_bytes, progress.total_bytes,
                   "download", identifier)
                   ? kairosboot::fastboot::PrimitiveUpdateProgressAction::
                         Continue
                   : kairosboot::fastboot::PrimitiveUpdateProgressAction::
                         Cancel;
      },
  };
  kairosboot::fastboot::PrimitiveUpdateDevice device(
      service, std::move(device_options));

  kairosboot::fastboot::UpdateExecutorOptions executor_options{
      .known_partitions =
          kairosboot::fastboot::frozen_update_known_partitions(),
      .deadline = *deadline == UpdateClock::time_point::max()
                      ? std::optional<UpdateClock::time_point>{}
                      : std::optional<UpdateClock::time_point>{*deadline},
      .observer = [&options, &identifier, &callback_cancelled](
                      const kairosboot::fastboot::UpdateExecutionEvent &event) {
        if (!report_update_progress(
                options, 0U, 0U, update_event_stage(event.kind), identifier)) {
          callback_cancelled = true;
          throw UpdateProgressCancelled{};
        }
      },
  };
  auto executed = kairosboot::fastboot::execute_prepared_update(
      *prepared, device, executor_options, task_context.cancellation_token());
  if (!executed) {
    auto payload = kairosboot::api::normalize_public_error(
        executed.error(), total_tasks, identifier);
    if (callback_cancelled && !executed.error().device_error.has_value()) {
      payload.status = KB_E_CANCELLED;
      payload.message = "update operation cancelled by the progress callback";
      payload.transfer_state = completed_update_transfer_state(
          executed.error().completed_tasks, total_tasks);
    }
    return operation_failure(std::move(payload));
  }

  if (auto completion = remaining_update_timeout(
          *deadline, identifier, "completion");
      !completion) {
    auto payload = std::move(completion.error());
    payload.transfer_state = completed_update_transfer_state(
        executed->completed_tasks, total_tasks);
    return operation_failure(std::move(payload));
  }
  if (task_context.cancel_requested() ||
      !report_update_progress(options, 0U, 0U, "complete", identifier) ||
      task_context.cancel_requested()) {
    return cancelled_operation(
        identifier,
        completed_update_transfer_state(executed->completed_tasks, total_tasks),
        "update cancelled after completing all prepared tasks");
  }
  if (auto completion = remaining_update_timeout(
          *deadline, identifier, "completion callback");
      !completion) {
    auto payload = std::move(completion.error());
    payload.transfer_state = completed_update_transfer_state(
        executed->completed_tasks, total_tasks);
    return operation_failure(std::move(payload));
  }
  return kairosboot::api::OperationOutcome::succeeded();
}

kb_status_t finish_blocking_operation(kb_operation_t *operation,
                                      kb_error_t **error) {
  const auto waited = kb_operation_wait(operation, KB_WAIT_INFINITE);
  if (waited != KB_OK) {
    const auto payload = operation->state->error();
    if (payload.has_value()) {
      static_cast<void>(fail(error, *payload));
    } else {
      static_cast<void>(fail(error, waited, kb_status_string(waited)));
    }
  }
  kb_operation_release(operation);
  return waited;
}

} // namespace

namespace kairosboot::api {

std::expected<std::shared_ptr<transport::LibusbRuntime>, OperationErrorPayload>
acquire_fleet_usb_runtime(kb_context_t &context) {
  auto runtime = acquire_context_usb_runtime(context);
  if (!runtime) {
    return std::unexpected(normalize_public_error(runtime.error(), {}));
  }
  return std::move(*runtime);
}

} // namespace kairosboot::api

extern "C" {

void KB_CALL kb_context_options_init(kb_context_options_t *options) {
  if (options == nullptr) {
    return;
  }
  *options = {};
  options->struct_size = sizeof(*options);
  options->api_version = KB_API_VERSION;
}

void KB_CALL kb_flash_options_init(kb_flash_options_t *options) {
  if (options == nullptr) {
    return;
  }
  *options = {};
  options->struct_size = sizeof(*options);
  options->api_version = KB_API_VERSION;
  options->timeout_ms = kDefaultTimeoutMs;
}

void KB_CALL kb_update_options_init(kb_update_options_t *options) {
  if (options == nullptr) {
    return;
  }
  *options = {};
  options->struct_size = sizeof(*options);
  options->api_version = KB_API_VERSION;
  options->timeout_ms = kDefaultTimeoutMs;
}

void KB_CALL kb_command_options_init(kb_command_options_t *options) {
  if (options == nullptr) {
    return;
  }
  *options = {};
  options->struct_size = sizeof(*options);
  options->api_version = KB_API_VERSION;
  options->timeout_ms = kDefaultTimeoutMs;
  options->maximum_receive_bytes = kDefaultMaximumReceiveBytes;
}

void KB_CALL kb_version_init(kb_version_t *version) {
  if (version == nullptr) {
    return;
  }
  *version = {};
  version->struct_size = sizeof(*version);
  version->api_version = KB_API_VERSION;
}

kb_status_t KB_CALL kb_get_version(kb_version_t *version) {
  if (version == nullptr || version->struct_size < KB_VERSION_V1_SIZE ||
      version->api_version != KB_API_VERSION) {
    return KB_E_INVALID_ARGUMENT;
  }
  version->major = KAIROSBOOT_VERSION_MAJOR;
  version->minor = KAIROSBOOT_VERSION_MINOR;
  version->patch = KAIROSBOOT_VERSION_PATCH;
  version->string = KAIROSBOOT_VERSION_STRING;
  return KB_OK;
}

const char *KB_CALL kb_status_string(kb_status_t status) {
  switch (status) {
  case KB_OK:
    return "ok";
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
    return "unknown";
  }
}

kb_status_t KB_CALL kb_context_create(const kb_context_options_t *options,
                                      kb_context_t **context,
                                      kb_error_t **error) {
  clear_error(error);
  if (context == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "context output pointer must not be null");
  }
  *context = nullptr;
  if (!valid_context_options(options)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "context options have an incompatible size or API version");
  }

  try {
    auto result = std::make_unique<kb_context>();
    kb_context_options_init(&result->options);
    result->usb_state = std::make_shared<kb_context_usb_state>();
    if (options != nullptr) {
      result->options.log_callback = options->log_callback;
      result->options.log_user_data = options->log_user_data;
    }
    *context = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY, "unable to allocate a context");
  } catch (...) {
    return fail(error, KB_E_INTERNAL, "unable to create a context");
  }
}

void KB_CALL kb_context_release(kb_context_t *context) { delete context; }

kb_status_t KB_CALL kb_enumerate_devices(kb_context_t *context,
                                         kb_device_list_t **devices,
                                         kb_error_t **error) {
  clear_error(error);
  if (devices == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device list output pointer must not be null");
  }
  *devices = nullptr;
  if (context == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "context must not be null");
  }
  auto runtime = acquire_context_usb_runtime(*context);
  if (!runtime) {
    return fail(error, runtime_error_status(runtime.error().kind),
                runtime_error_message(runtime.error().kind), "",
                runtime.error().native_code);
  }

  kairosboot::transport::UsbInterfaceFilter filter;
  filter.interface_class = 0xFF;
  filter.interface_subclass = 0x42;
  filter.interface_protocol = 0x03;
  auto enumerated = (*runtime)->enumerate(filter);
  if (!enumerated) {
    const auto status = runtime_error_status(enumerated.error().kind);
    return fail(error, status,
                runtime_error_message(enumerated.error().kind), "",
                enumerated.error().native_code);
  }

  try {
    auto result = std::make_unique<kb_device_list>();
    result->devices.reserve(enumerated->size());
    for (const auto &device : *enumerated) {
      result->devices.push_back(kb_device_info{
          device.serial_utf8, physical_usb_path(device), std::string{}});
    }
    *devices = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the USB device list");
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to build the USB device list");
  }
}

size_t KB_CALL kb_device_list_count(const kb_device_list_t *devices) {
  return devices == nullptr ? 0 : devices->devices.size();
}

const char *KB_CALL kb_device_list_serial(const kb_device_list_t *devices,
                                          size_t index) {
  return device_field(devices, index, &kb_device_info::serial);
}

const char *KB_CALL kb_device_list_usb_path(const kb_device_list_t *devices,
                                            size_t index) {
  return device_field(devices, index, &kb_device_info::usb_path);
}

const char *KB_CALL kb_device_list_product(const kb_device_list_t *devices,
                                           size_t index) {
  return device_field(devices, index, &kb_device_info::product);
}

void KB_CALL kb_device_list_release(kb_device_list_t *devices) {
  delete devices;
}

kb_status_t KB_CALL kb_flash_file_async(
    kb_context_t *context, const char *serial_or_null, const char *partition,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (context == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "context must not be null");
  }
  if (partition == nullptr || partition[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "partition must not be empty", serial_or_null);
  }
  if (file_path == nullptr || file_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "file path must not be empty", serial_or_null);
  }
  if (!valid_flash_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flash options have an incompatible size or API version",
                serial_or_null);
  }

  try {
    const std::optional<std::string_view> requested_serial =
        serial_or_null == nullptr
            ? std::nullopt
            : std::optional<std::string_view>{serial_or_null};
    if (requested_serial.has_value() && requested_serial->empty()) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "explicit device serial must not be empty");
    }
    if (requested_serial.has_value() && !valid_utf8(*requested_serial)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "device serial must be valid UTF-8");
    }

    const std::string_view partition_view{partition};
    if (!valid_fastboot_parameter(partition_view,
                                  std::string_view{"flash:"}.size())) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "partition must be printable ASCII and fit the Fastboot "
                  "command limit",
                  serial_or_null);
    }

    const std::string_view file_path_view{file_path};
    if (!valid_utf8(file_path_view)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "file path must be valid UTF-8", serial_or_null);
    }

    const std::string requested_identifier =
        requested_serial.has_value() ? std::string{*requested_serial}
                                     : std::string{};
    auto file_source =
        kairosboot::image::FileImageSource::open(utf8_path(file_path_view));
    if (!file_source) {
      return fail(error, kairosboot::api::normalize_public_error(
                             file_source.error(), requested_identifier));
    }

    auto prepared_target = prepare_flash_target(*context, requested_serial);
    if (!prepared_target) {
      return fail(error, prepared_target.error());
    }

    kb_flash_options_t flash_options;
    kb_flash_options_init(&flash_options);
    if (options_or_null != nullptr) {
      flash_options.timeout_ms = options_or_null->timeout_ms;
      flash_options.progress_callback = options_or_null->progress_callback;
      flash_options.progress_user_data = options_or_null->progress_user_data;
    }

    std::shared_ptr<const kairosboot::image::IImageSource> image_source =
        std::move(*file_source);
    auto selected_identifier = prepared_target->selector.identifier;
    std::string partition_copy{partition_view};

    auto task = [target = std::move(*prepared_target),
                 image_source = std::move(image_source),
                 partition_copy = std::move(partition_copy), flash_options,
                 selected_identifier = std::move(selected_identifier)](
                    kairosboot::api::OperationState::TaskContext
                        &task_context) mutable
        -> kairosboot::api::OperationOutcome {
      if (task_context.cancel_requested()) {
        return cancelled_operation(selected_identifier,
                                   KB_TRANSFER_NOT_SENT);
      }

      auto artifact = kairosboot::image::FlashArtifact::inspect(
          image_source, task_context.cancellation_token());
      if (!artifact) {
        return operation_failure(kairosboot::api::normalize_public_error(
            artifact.error(), selected_identifier));
      }
      if (artifact->metadata().transfer_size == 0) {
        auto valid_size = kairosboot::fastboot::validate_download_size(0);
        return operation_failure(kairosboot::api::normalize_public_error(
            valid_size.error(), selected_identifier));
      }

      kb_command_options_t transport_options;
      kb_command_options_init(&transport_options);
      transport_options.timeout_ms = flash_options.timeout_ms;
      auto opened = open_target(target, transport_options,
                                task_context.cancellation_token());
      if (!opened) {
        return operation_failure(std::move(opened.error()));
      }

      std::unique_ptr<kairosboot::protocol::ITransportSession>
          protocol_transport = std::move(*opened);
      kairosboot::protocol::SessionOptions session_options;
      session_options.io_timeout =
          std::chrono::milliseconds{flash_options.timeout_ms};
      kairosboot::protocol::FastbootSession session(
          std::move(protocol_transport), session_options);
      kairosboot::fastboot::PrimitiveService service(session);
      auto cancellation = task_context.register_cancellation_hook(
          [&service] { service.request_cancel(); });

      std::uint64_t target_max_download_size = 0;
      auto maximum = service.getvar("max-download-size");
      if (maximum) {
        target_max_download_size =
            kairosboot::fastboot::parse_unsigned_variable(
                maximum->terminal.payload)
                .value_or(0);
      } else if (maximum.error().code !=
                 kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
        return operation_failure(kairosboot::api::normalize_public_error(
            maximum.error(), selected_identifier));
      }

      auto plan = kairosboot::image::SparseFlashPlan::create(
          *artifact, target_max_download_size,
          kairosboot::image::kDefaultResparseLimitBytes,
          task_context.cancellation_token());
      if (!plan) {
        return operation_failure(kairosboot::api::normalize_public_error(
            plan.error(), selected_identifier));
      }

      std::vector<std::shared_ptr<kairosboot::protocol::ITransferSource>>
          transfer_sources;
      transfer_sources.reserve(plan->parts().size());
      for (const auto& part : plan->parts()) {
        auto source = kairosboot::transport::ImageTransferSource::create(
            part.source);
        if (!source) {
          return operation_failure(kairosboot::api::normalize_public_error(
              source.error(), selected_identifier));
        }
        if (auto valid_size = kairosboot::fastboot::validate_download_size(
                (*source)->size());
            !valid_size) {
          return operation_failure(kairosboot::api::normalize_public_error(
              valid_size.error(), selected_identifier));
        }
        transfer_sources.push_back(std::move(*source));
      }

      const auto total_transfer_size = plan->transfer_size();
      if (task_context.cancel_requested() ||
          !report_progress(flash_options, 0, total_transfer_size, "download",
                           selected_identifier)) {
        return cancelled_operation(selected_identifier,
                                   KB_TRANSFER_NOT_SENT);
      }

      std::uint64_t completed_before_part = 0;
      for (const auto& source : transfer_sources) {
        const kairosboot::protocol::TransferProgressObserver observer =
            [&task_context, &flash_options, &selected_identifier,
             completed_before_part,
             total_transfer_size](const std::uint64_t completed,
                                  const std::uint64_t) {
              if (task_context.cancel_requested() ||
                  !report_progress(
                      flash_options, completed_before_part + completed,
                      total_transfer_size, "download", selected_identifier)) {
                return kairosboot::protocol::TransferProgressAction::cancel;
              }
              return kairosboot::protocol::TransferProgressAction::
                  continue_transfer;
            };

        auto flashed = service.download_and_flash_source(
            partition_copy, source, observer);
        if (!flashed) {
          auto payload = kairosboot::api::normalize_public_error(
              flashed.error(), selected_identifier);
          kairosboot::api::accumulate_flash_transfer_state(
              payload, flashed.error().operation, completed_before_part,
              source->size(), total_transfer_size);
          return operation_failure(std::move(payload));
        }
        completed_before_part += source->size();
      }

      if (task_context.cancel_requested() ||
          !report_progress(flash_options, total_transfer_size,
                           total_transfer_size, "complete",
                           selected_identifier)) {
        return cancelled_operation(
            selected_identifier, KB_TRANSFER_FULLY_TRANSFERRED,
            "operation cancelled after the flash completed");
      }
      return kairosboot::api::OperationOutcome::succeeded();
    };

    auto result = std::make_unique<kb_operation>(std::move(task));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL,
                  "unable to start the flash operation", serial_or_null);
    }
    *operation = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the flash operation", serial_or_null);
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), serial_or_null,
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the flash operation", serial_or_null);
  }
}

kb_status_t KB_CALL kb_flash_file(
    kb_context_t *context, const char *serial_or_null, const char *partition,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_error_t **error) {
  kb_operation_t *operation = nullptr;
  const kb_status_t start = kb_flash_file_async(
      context, serial_or_null, partition, file_path, options_or_null, &operation,
      error);
  if (start != KB_OK) {
    return start;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_update_package_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *package_path, const kb_update_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (context == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "context must not be null",
                device_selector_or_null);
  }
  if (package_path == nullptr || package_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "update package path must not be empty",
                device_selector_or_null);
  }
  if (!valid_update_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "update options have an incompatible size, API version, or "
                "wipe value",
                device_selector_or_null);
  }

  try {
    const std::string_view package_path_view{package_path};
    if (!valid_utf8(package_path_view)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "update package path must be valid UTF-8",
                  device_selector_or_null);
    }
    auto selector = parse_target_selector(device_selector_or_null);
    if (!selector) {
      return fail(error, selector.error());
    }

    auto copied_options = update_options_or_default(options_or_null);
    auto native_package_path =
        std::filesystem::absolute(utf8_path(package_path_view));
    auto usb_state = context->usb_state;
    auto task = [usb_state = std::move(usb_state),
                 selector = std::move(*selector),
                 package = std::move(native_package_path), copied_options](
                    kairosboot::api::OperationState::TaskContext &task_context)
        mutable -> kairosboot::api::OperationOutcome {
      return run_prepared_public_update(
          usb_state, std::move(selector), package, copied_options,
          task_context);
    };
    auto result = std::make_unique<kb_operation>(std::move(task));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL,
                  "unable to start the update package operation",
                  device_selector_or_null);
    }
    *operation = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the update package operation",
                device_selector_or_null);
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_selector_or_null,
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the update package operation",
                device_selector_or_null);
  }
}

kb_status_t KB_CALL kb_update_package(
    kb_context_t *context, const char *device_selector_or_null,
    const char *package_path, const kb_update_options_t *options_or_null,
    kb_error_t **error) {
  clear_error(error);
  kb_operation_t *operation = nullptr;
  const auto started = kb_update_package_async(
      context, device_selector_or_null, package_path, options_or_null,
      &operation, error);
  if (started != KB_OK) {
    return started;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_getvar_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *variable, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (variable == nullptr ||
      !valid_fastboot_parameter(variable, std::string_view{"getvar:"}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "getvar name must be printable ASCII and fit the Fastboot command limit",
                device_selector_or_null);
  }
  std::string variable_copy{variable};
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [variable_copy = std::move(variable_copy)](
          kairosboot::fastboot::PrimitiveService &service,
          kairosboot::api::OperationState::TaskContext &,
          const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = service.getvar(variable_copy);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_getvar(
    kb_context_t *context, const char *device_selector_or_null,
    const char *variable, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_getvar_async, result, error, context, device_selector_or_null,
      variable, options_or_null);
}

kb_status_t KB_CALL kb_erase_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (partition == nullptr ||
      !valid_fastboot_parameter(partition, std::string_view{"erase:"}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "erase partition must be printable ASCII and fit the Fastboot command limit",
                device_selector_or_null);
  }
  std::string partition_copy{partition};
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [partition_copy = std::move(partition_copy)](
          kairosboot::fastboot::PrimitiveService &service,
          kairosboot::api::OperationState::TaskContext &,
          const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = service.erase(partition_copy);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_erase(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_erase_async, result, error, context, device_selector_or_null,
      partition, options_or_null);
}

kb_status_t KB_CALL kb_set_active_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *slot, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (slot == nullptr ||
      !valid_fastboot_parameter(slot, std::string_view{"set_active:"}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "slot must be printable ASCII and fit the Fastboot command limit",
                device_selector_or_null);
  }
  std::string slot_copy{slot};
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [slot_copy = std::move(slot_copy)](
          kairosboot::fastboot::PrimitiveService &service,
          kairosboot::api::OperationState::TaskContext &,
          const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = service.set_active(slot_copy);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_set_active(
    kb_context_t *context, const char *device_selector_or_null,
    const char *slot, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_set_active_async, result, error, context, device_selector_or_null,
      slot, options_or_null);
}

kb_status_t KB_CALL kb_flashing_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_flashing_command_t command,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  kairosboot::fastboot::FlashingCommand native_command;
  switch (command) {
  case KB_FLASHING_LOCK:
    native_command = kairosboot::fastboot::FlashingCommand::Lock;
    break;
  case KB_FLASHING_UNLOCK:
    native_command = kairosboot::fastboot::FlashingCommand::Unlock;
    break;
  case KB_FLASHING_LOCK_CRITICAL:
    native_command = kairosboot::fastboot::FlashingCommand::LockCritical;
    break;
  case KB_FLASHING_UNLOCK_CRITICAL:
    native_command = kairosboot::fastboot::FlashingCommand::UnlockCritical;
    break;
  case KB_FLASHING_GET_UNLOCK_ABILITY:
    native_command = kairosboot::fastboot::FlashingCommand::GetUnlockAbility;
    break;
  default:
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flashing command is invalid", device_selector_or_null);
  }
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [native_command](kairosboot::fastboot::PrimitiveService &service,
                       kairosboot::api::OperationState::TaskContext &,
                       const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        return primitive_execution(service.flashing(native_command));
      },
      operation, error);
}

kb_status_t KB_CALL kb_flashing(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_flashing_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_flashing_async, result, error, context, device_selector_or_null,
      command, options_or_null);
}

kb_status_t KB_CALL kb_gsi_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_gsi_command_t command,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  kairosboot::fastboot::GsiCommand native_command;
  switch (command) {
  case KB_GSI_WIPE:
    native_command = kairosboot::fastboot::GsiCommand::Wipe;
    break;
  case KB_GSI_DISABLE:
    native_command = kairosboot::fastboot::GsiCommand::Disable;
    break;
  case KB_GSI_STATUS:
    native_command = kairosboot::fastboot::GsiCommand::Status;
    break;
  default:
    return fail(error, KB_E_INVALID_ARGUMENT, "GSI command is invalid",
                device_selector_or_null);
  }
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [native_command](kairosboot::fastboot::PrimitiveService &service,
                       kairosboot::api::OperationState::TaskContext &,
                       const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        return primitive_execution(service.gsi(native_command));
      },
      operation, error);
}

kb_status_t KB_CALL kb_gsi(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_gsi_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_gsi_async, result, error, context, device_selector_or_null, command,
      options_or_null);
}

kb_status_t KB_CALL kb_snapshot_update_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_snapshot_update_command_t command,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  kairosboot::fastboot::SnapshotUpdateCommand native_command;
  switch (command) {
  case KB_SNAPSHOT_UPDATE_CANCEL:
    native_command = kairosboot::fastboot::SnapshotUpdateCommand::Cancel;
    break;
  case KB_SNAPSHOT_UPDATE_MERGE:
    native_command = kairosboot::fastboot::SnapshotUpdateCommand::Merge;
    break;
  default:
    return fail(error, KB_E_INVALID_ARGUMENT,
                "snapshot-update command is invalid",
                device_selector_or_null);
  }
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [native_command](kairosboot::fastboot::PrimitiveService &service,
                       kairosboot::api::OperationState::TaskContext &,
                       const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        return primitive_execution(service.snapshot_update(native_command));
      },
      operation, error);
}

kb_status_t KB_CALL kb_snapshot_update(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_snapshot_update_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_snapshot_update_async, result, error, context,
      device_selector_or_null, command, options_or_null);
}

kb_status_t KB_CALL kb_create_logical_partition_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name, const uint64_t size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  const auto overhead =
      std::string_view{"create-logical-partition:"}.size() + 1U +
      decimal_digit_count(size);
  if (const auto valid = validate_logical_partition_name(
          partition_name, overhead, device_selector_or_null, error);
      valid != KB_OK) {
    return valid;
  }
  try {
    std::string name_copy{partition_name};
    return start_primitive_async(
        context, device_selector_or_null, options_or_null,
        [name_copy = std::move(name_copy), size](
            kairosboot::fastboot::PrimitiveService &service,
            kairosboot::api::OperationState::TaskContext &,
            const kb_command_options_t &, const std::string &)
            -> std::expected<PrimitiveExecution,
                             kairosboot::fastboot::PrimitiveError> {
          return primitive_execution(
              service.create_logical_partition(name_copy, size));
        },
        operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the create-logical-partition operation",
                device_selector_or_null);
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the create-logical-partition operation",
                device_selector_or_null);
  }
}

kb_status_t KB_CALL kb_create_logical_partition(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name, const uint64_t size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_create_logical_partition_async, result, error, context,
      device_selector_or_null, partition_name, size, options_or_null);
}

kb_status_t KB_CALL kb_delete_logical_partition_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  const auto overhead =
      std::string_view{"delete-logical-partition:"}.size();
  if (const auto valid = validate_logical_partition_name(
          partition_name, overhead, device_selector_or_null, error);
      valid != KB_OK) {
    return valid;
  }
  try {
    std::string name_copy{partition_name};
    return start_primitive_async(
        context, device_selector_or_null, options_or_null,
        [name_copy = std::move(name_copy)](
            kairosboot::fastboot::PrimitiveService &service,
            kairosboot::api::OperationState::TaskContext &,
            const kb_command_options_t &, const std::string &)
            -> std::expected<PrimitiveExecution,
                             kairosboot::fastboot::PrimitiveError> {
          return primitive_execution(
              service.delete_logical_partition(name_copy));
        },
        operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the delete-logical-partition operation",
                device_selector_or_null);
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the delete-logical-partition operation",
                device_selector_or_null);
  }
}

kb_status_t KB_CALL kb_delete_logical_partition(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_delete_logical_partition_async, result, error, context,
      device_selector_or_null, partition_name, options_or_null);
}

kb_status_t KB_CALL kb_resize_logical_partition_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name, const uint64_t size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  const auto overhead =
      std::string_view{"resize-logical-partition:"}.size() + 1U +
      decimal_digit_count(size);
  if (const auto valid = validate_logical_partition_name(
          partition_name, overhead, device_selector_or_null, error);
      valid != KB_OK) {
    return valid;
  }
  try {
    std::string name_copy{partition_name};
    return start_primitive_async(
        context, device_selector_or_null, options_or_null,
        [name_copy = std::move(name_copy), size](
            kairosboot::fastboot::PrimitiveService &service,
            kairosboot::api::OperationState::TaskContext &,
            const kb_command_options_t &, const std::string &)
            -> std::expected<PrimitiveExecution,
                             kairosboot::fastboot::PrimitiveError> {
          return primitive_execution(
              service.resize_logical_partition(name_copy, size));
        },
        operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the resize-logical-partition operation",
                device_selector_or_null);
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the resize-logical-partition operation",
                device_selector_or_null);
  }
}

kb_status_t KB_CALL kb_resize_logical_partition(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name, const uint64_t size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_resize_logical_partition_async, result, error, context,
      device_selector_or_null, partition_name, size, options_or_null);
}

kb_status_t KB_CALL kb_reboot_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_reboot_target_t target,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  kairosboot::fastboot::RebootTarget native_target;
  switch (target) {
  case KB_REBOOT_SYSTEM:
    native_target = kairosboot::fastboot::RebootTarget::System;
    break;
  case KB_REBOOT_BOOTLOADER:
    native_target = kairosboot::fastboot::RebootTarget::Bootloader;
    break;
  case KB_REBOOT_RECOVERY:
    native_target = kairosboot::fastboot::RebootTarget::Recovery;
    break;
  case KB_REBOOT_FASTBOOT:
    native_target = kairosboot::fastboot::RebootTarget::Fastboot;
    break;
  default:
    return fail(error, KB_E_INVALID_ARGUMENT, "reboot target is invalid",
                device_selector_or_null);
  }
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [native_target](kairosboot::fastboot::PrimitiveService &service,
                      kairosboot::api::OperationState::TaskContext &,
                      const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = service.reboot(native_target);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_reboot(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_reboot_target_t target,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_reboot_async, result, error, context, device_selector_or_null, target,
      options_or_null);
}

kb_status_t KB_CALL kb_continue_boot_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [](kairosboot::fastboot::PrimitiveService &service,
         kairosboot::api::OperationState::TaskContext &,
         const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = service.continue_boot();
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_continue_boot(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_continue_boot_async, result, error, context, device_selector_or_null,
      options_or_null);
}

kb_status_t KB_CALL kb_oem_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *command_suffix,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (command_suffix == nullptr ||
      !valid_fastboot_parameter(command_suffix, std::string_view{"oem "}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "OEM suffix must be printable ASCII and fit the Fastboot command limit",
                device_selector_or_null);
  }
  std::string suffix_copy{command_suffix};
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [suffix_copy = std::move(suffix_copy)](
          kairosboot::fastboot::PrimitiveService &service,
          kairosboot::api::OperationState::TaskContext &,
          const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = service.oem(suffix_copy);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_oem(
    kb_context_t *context, const char *device_selector_or_null,
    const char *command_suffix,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_oem_async, result, error, context, device_selector_or_null,
      command_suffix, options_or_null);
}

kb_status_t KB_CALL kb_raw_command_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *command, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (command == nullptr || !valid_fastboot_parameter(command, 0)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "raw command must be printable ASCII and fit the Fastboot command limit",
                device_selector_or_null);
  }
  std::string command_copy{command};
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [command_copy = std::move(command_copy)](
          kairosboot::fastboot::PrimitiveService &service,
          kairosboot::api::OperationState::TaskContext &,
          const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = service.raw_command(command_copy);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_raw_command(
    kb_context_t *context, const char *device_selector_or_null,
    const char *command, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_raw_command_async, result, error, context, device_selector_or_null,
      command, options_or_null);
}

kb_status_t KB_CALL kb_boot_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [](kairosboot::fastboot::PrimitiveService &service,
         kairosboot::api::OperationState::TaskContext &,
         const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = service.boot_downloaded();
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_boot(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_boot_async, result, error, context, device_selector_or_null,
      options_or_null);
}

kb_status_t KB_CALL kb_stage_async(
    kb_context_t *context, const char *device_selector_or_null,
    const void *data, const size_t data_size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (data == nullptr || data_size == 0 ||
      data_size > std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "stage data must be non-empty and fit the Fastboot 32-bit length",
                device_selector_or_null);
  }
  try {
    const auto *first = static_cast<const std::byte *>(data);
    std::vector<std::byte> copied(first, first + data_size);
    return start_primitive_async(
        context, device_selector_or_null, options_or_null,
        [copied = std::move(copied)](
            kairosboot::fastboot::PrimitiveService &service,
            kairosboot::api::OperationState::TaskContext &task_context,
            const kb_command_options_t &options,
            const std::string &identifier) mutable
            -> std::expected<PrimitiveExecution,
                             kairosboot::fastboot::PrimitiveError> {
          const kairosboot::protocol::TransferProgressObserver observer =
              [&task_context, &options, &identifier](
                  const std::uint64_t completed, const std::uint64_t total) {
                return task_context.cancel_requested() ||
                               !report_command_progress(
                                   options, completed, total, "stage",
                                   identifier)
                           ? kairosboot::protocol::TransferProgressAction::cancel
                           : kairosboot::protocol::TransferProgressAction::
                                 continue_transfer;
              };
          auto source = std::make_shared<MemorySource>(std::move(copied));
          auto reply = service.stage_source(std::move(source), observer);
          if (!reply) {
            return std::unexpected(std::move(reply.error()));
          }
          return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
        },
        operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY, "unable to copy stage data",
                device_selector_or_null);
  }
}

kb_status_t KB_CALL kb_stage(
    kb_context_t *context, const char *device_selector_or_null,
    const void *data, const size_t data_size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_stage_async, result, error, context, device_selector_or_null, data,
      data_size, options_or_null);
}

kb_status_t KB_CALL kb_upload_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [](kairosboot::fastboot::PrimitiveService &service,
         kairosboot::api::OperationState::TaskContext &task_context,
         const kb_command_options_t &options, const std::string &identifier)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto sink = std::make_shared<MemorySink>();
        const kairosboot::protocol::TransferProgressObserver observer =
            [&task_context, &options, &identifier](
                const std::uint64_t completed, const std::uint64_t total) {
              return task_context.cancel_requested() ||
                             !report_command_progress(
                                 options, completed, total, "upload", identifier)
                         ? kairosboot::protocol::TransferProgressAction::cancel
                         : kairosboot::protocol::TransferProgressAction::
                               continue_transfer;
            };
        auto reply = service.upload_to_sink(
            sink, options.maximum_receive_bytes, observer);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{
            .reply = std::move(*reply), .data = sink->take()};
      },
      operation, error);
}

kb_status_t KB_CALL kb_upload(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_upload_async, result, error, context, device_selector_or_null,
      options_or_null);
}

kb_status_t KB_CALL kb_fetch_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const uint64_t offset_or_unspecified,
    const uint64_t size_or_unspecified,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  constexpr auto maximum_range =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (partition == nullptr || !valid_fetch_partition(partition)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch partition must use letters, digits, '_', '-', or '.'",
                device_selector_or_null);
  }
  if (size_or_unspecified != KB_FETCH_UNSPECIFIED &&
      offset_or_unspecified == KB_FETCH_UNSPECIFIED) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch size requires an explicit offset",
                device_selector_or_null);
  }
  if ((offset_or_unspecified != KB_FETCH_UNSPECIFIED &&
       offset_or_unspecified > maximum_range) ||
      (size_or_unspecified != KB_FETCH_UNSPECIFIED &&
       size_or_unspecified > maximum_range)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch offset and size must fit signed 64-bit values",
                device_selector_or_null);
  }
  size_t command_size = std::string_view{"fetch:"}.size() +
                        std::string_view{partition}.size();
  if (offset_or_unspecified != KB_FETCH_UNSPECIFIED) {
    command_size += fetch_range_component_size(offset_or_unspecified);
  }
  if (size_or_unspecified != KB_FETCH_UNSPECIFIED) {
    command_size += fetch_range_component_size(size_or_unspecified);
  }
  if (command_size > kairosboot::protocol::kDefaultMaxCommandBytes) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch command exceeds the Fastboot command limit",
                device_selector_or_null);
  }
  kairosboot::fastboot::FetchRange range;
  if (offset_or_unspecified != KB_FETCH_UNSPECIFIED) {
    range.offset = offset_or_unspecified;
  }
  if (size_or_unspecified != KB_FETCH_UNSPECIFIED) {
    range.size = size_or_unspecified;
  }
  std::string partition_copy{partition};
  return start_primitive_async(
      context, device_selector_or_null, options_or_null,
      [partition_copy = std::move(partition_copy), range](
          kairosboot::fastboot::PrimitiveService &service,
          kairosboot::api::OperationState::TaskContext &task_context,
          const kb_command_options_t &options, const std::string &identifier)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto sink = std::make_shared<MemorySink>();
        const kairosboot::protocol::TransferProgressObserver observer =
            [&task_context, &options, &identifier](
                const std::uint64_t completed, const std::uint64_t total) {
              return task_context.cancel_requested() ||
                             !report_command_progress(
                                 options, completed, total, "fetch", identifier)
                         ? kairosboot::protocol::TransferProgressAction::cancel
                         : kairosboot::protocol::TransferProgressAction::
                               continue_transfer;
            };
        auto reply = service.fetch_to_sink(
            partition_copy, range, sink, options.maximum_receive_bytes,
            observer);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{
            .reply = std::move(*reply), .data = sink->take()};
      },
      operation, error);
}

kb_status_t KB_CALL kb_fetch(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const uint64_t offset_or_unspecified,
    const uint64_t size_or_unspecified,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_fetch_async, result, error, context, device_selector_or_null,
      partition, offset_or_unspecified, size_or_unspecified, options_or_null);
}

kb_status_t KB_CALL kb_operation_wait(kb_operation_t *operation,
                                      uint32_t timeout_ms) {
  if (operation == nullptr || operation->state == nullptr) {
    return KB_E_INVALID_ARGUMENT;
  }
  if (timeout_ms == KB_WAIT_INFINITE) {
    operation->state->wait();
  } else if (operation->state->wait_for(
                 std::chrono::milliseconds{timeout_ms}) ==
             kairosboot::api::OperationWaitResult::Timeout) {
    return KB_E_TIMEOUT;
  }
  return operation->state->status();
}

kb_status_t KB_CALL kb_operation_cancel(kb_operation_t *operation) {
  if (operation == nullptr || operation->state == nullptr) {
    return KB_E_INVALID_ARGUMENT;
  }
  operation->state->cancel();
  return KB_OK;
}

kb_operation_state_t KB_CALL
kb_operation_state(const kb_operation_t *operation) {
  return operation == nullptr || operation->state == nullptr
             ? KB_OPERATION_FAILED
             : public_operation_state(operation->state->phase());
}

const kb_error_t *KB_CALL
kb_operation_error(const kb_operation_t *operation) {
  return materialize_operation_error(operation);
}

kb_status_t KB_CALL kb_operation_command_result(
    const kb_operation_t *operation, kb_command_result_t **result,
    kb_error_t **error) {
  clear_error(error);
  if (result == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "command result output pointer must not be null");
  }
  *result = nullptr;
  if (operation == nullptr || operation->state == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "operation must not be null");
  }
  const auto phase = operation->state->phase();
  if (phase == kairosboot::api::OperationPhase::Created ||
      phase == kairosboot::api::OperationPhase::Running) {
    return fail(error, KB_E_BUSY, "operation has not completed");
  }
  if (phase != kairosboot::api::OperationPhase::Succeeded) {
    const auto payload = operation->state->error();
    return payload.has_value()
               ? fail(error, *payload)
               : fail(error, KB_E_INTERNAL,
                      "operation failed without an error payload");
  }
  try {
    const auto payload = operation->state->command_result();
    if (payload == nullptr) {
      return fail(error, KB_E_NOT_SUPPORTED,
                  "operation did not produce a command result");
    }
    auto handle = std::make_unique<kb_command_result>(payload);
    *result = handle.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the command result handle");
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to materialize the command result");
  }
}

void KB_CALL kb_operation_release(kb_operation_t *operation) {
  delete operation;
}

const uint8_t *KB_CALL kb_command_result_terminal_payload(
    const kb_command_result_t *result, size_t *size) {
  const auto *payload = command_result_payload(result);
  if (size != nullptr) {
    *size = payload == nullptr ? 0 : payload->terminal_payload.size();
  }
  return payload == nullptr || payload->terminal_payload.empty()
             ? nullptr
             : reinterpret_cast<const uint8_t *>(
                   payload->terminal_payload.data());
}

size_t KB_CALL kb_command_result_message_count(
    const kb_command_result_t *result) {
  const auto *payload = command_result_payload(result);
  return payload == nullptr ? 0 : payload->messages.size();
}

kb_command_message_kind_t KB_CALL kb_command_result_message_kind(
    const kb_command_result_t *result, const size_t index) {
  const auto *payload = command_result_payload(result);
  if (payload == nullptr || index >= payload->messages.size()) {
    return KB_COMMAND_MESSAGE_INFO;
  }
  return payload->messages[index].kind ==
                 kairosboot::api::CommandMessageKind::Text
             ? KB_COMMAND_MESSAGE_TEXT
             : KB_COMMAND_MESSAGE_INFO;
}

const uint8_t *KB_CALL kb_command_result_message_payload(
    const kb_command_result_t *result, const size_t index, size_t *size) {
  if (size != nullptr) {
    *size = 0;
  }
  const auto *result_payload = command_result_payload(result);
  if (result_payload == nullptr || index >= result_payload->messages.size()) {
    return nullptr;
  }
  const auto &payload = result_payload->messages[index].text;
  if (size != nullptr) {
    *size = payload.size();
  }
  return payload.empty()
             ? nullptr
             : reinterpret_cast<const uint8_t *>(payload.data());
}

const uint8_t *KB_CALL kb_command_result_data(
    const kb_command_result_t *result, size_t *size) {
  const auto *payload = command_result_payload(result);
  if (size != nullptr) {
    *size = payload == nullptr ? 0 : payload->data.size();
  }
  return payload == nullptr || payload->data.empty()
             ? nullptr
             : reinterpret_cast<const uint8_t *>(
                   payload->data.data());
}

const char *KB_CALL kb_command_result_device_identifier(
    const kb_command_result_t *result) {
  const auto *payload = command_result_payload(result);
  return payload == nullptr ? "" : payload->device_identifier.c_str();
}

void KB_CALL kb_command_result_release(kb_command_result_t *result) {
  delete result;
}

kb_status_t KB_CALL kb_error_status(const kb_error_t *error) {
  return error == nullptr ? KB_OK : error->status;
}

const char *KB_CALL kb_error_message(const kb_error_t *error) {
  return error == nullptr ? "" : error->message.c_str();
}

const char *KB_CALL kb_error_device_identifier(const kb_error_t *error) {
  return error == nullptr ? "" : error->device_identifier.c_str();
}

int32_t KB_CALL kb_error_native_code(const kb_error_t *error) {
  return error == nullptr ? 0 : error->native_code;
}

kb_transfer_state_t KB_CALL
kb_error_transfer_state(const kb_error_t *error) {
  return error == nullptr ? KB_TRANSFER_NOT_SENT : error->transfer_state;
}

const uint8_t *KB_CALL kb_error_device_message(
    const kb_error_t *error, size_t *size) {
  if (size != nullptr) {
    *size = error == nullptr ? 0 : error->device_message.size();
  }
  return error == nullptr || error->device_message.empty()
             ? nullptr
             : reinterpret_cast<const uint8_t *>(error->device_message.data());
}

size_t KB_CALL kb_error_command_message_count(const kb_error_t *error) {
  return error == nullptr ? 0 : error->command_messages.size();
}

kb_command_message_kind_t KB_CALL kb_error_command_message_kind(
    const kb_error_t *error, const size_t index) {
  if (error == nullptr || index >= error->command_messages.size()) {
    return KB_COMMAND_MESSAGE_INFO;
  }
  return error->command_messages[index].kind ==
                 kairosboot::api::CommandMessageKind::Text
             ? KB_COMMAND_MESSAGE_TEXT
             : KB_COMMAND_MESSAGE_INFO;
}

const uint8_t *KB_CALL kb_error_command_message_payload(
    const kb_error_t *error, const size_t index, size_t *size) {
  if (size != nullptr) {
    *size = 0;
  }
  if (error == nullptr || index >= error->command_messages.size()) {
    return nullptr;
  }
  const auto &payload = error->command_messages[index].text;
  if (size != nullptr) {
    *size = payload.size();
  }
  return payload.empty()
             ? nullptr
             : reinterpret_cast<const uint8_t *>(payload.data());
}

uint64_t KB_CALL kb_error_inbound_expected_bytes(const kb_error_t *error) {
  return error == nullptr || !error->inbound_expected.has_value()
             ? KB_FETCH_UNSPECIFIED
             : *error->inbound_expected;
}

uint64_t KB_CALL kb_error_inbound_transferred_bytes(const kb_error_t *error) {
  return error == nullptr ? 0 : error->inbound_transferred;
}

kb_transfer_state_t KB_CALL
kb_error_inbound_transfer_state(const kb_error_t *error) {
  return error == nullptr ? KB_TRANSFER_NOT_SENT
                          : error->inbound_transfer_state;
}

int32_t KB_CALL kb_error_session_poisoned(const kb_error_t *error) {
  return error != nullptr && error->session_poisoned ? 1 : 0;
}

void KB_CALL kb_error_release(kb_error_t *error) { delete error; }

} // extern "C"
