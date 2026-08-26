#include <kairosboot/kairosboot.h>

#include "src/api/device_selection.hpp"
#include "src/api/error_mapping.hpp"
#include "src/api/operation_state.hpp"
#include "src/fastboot/primitive_service.hpp"
#include "src/fastboot/variable_parser.hpp"
#include "src/image/file_source.hpp"
#include "src/image/flash_artifact.hpp"
#include "src/image/sparse_flash_plan.hpp"
#include "src/protocol/fastboot_protocol.hpp"
#include "src/transport/image_transfer_source.hpp"
#include "src/transport/libusb_runtime.hpp"
#include "src/transport/usb_fastboot.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct kb_context {
  kb_context_options_t options{};
  std::shared_ptr<kairosboot::transport::LibusbRuntime> usb_runtime;
};

struct kb_device_info {
  std::string serial;
  std::string usb_path;
  std::string product;
};

struct kb_device_list {
  std::vector<kb_device_info> devices;
};

struct kb_error {
  kb_status_t status{KB_OK};
  std::string message;
  std::string device_identifier;
  int32_t native_code{0};
  kb_transfer_state_t transfer_state{KB_TRANSFER_NOT_SENT};
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
      *error = new kb_error{status, message,
                            device_identifier == nullptr ? ""
                                                         : device_identifier,
                            native_code, transfer_state};
    } catch (...) {
      *error = nullptr;
    }
  }
  return status;
}

kb_status_t fail(
    kb_error_t **error,
    const kairosboot::api::OperationErrorPayload &payload) noexcept {
  return fail(error, payload.status, payload.message.c_str(),
              payload.device_identifier.c_str(), payload.native_code,
              payload.transfer_state);
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
      KB_E_CANCELLED,
      message,
      0,
      transfer_state,
      std::move(device),
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
         (options->struct_size >= sizeof(kb_context_options_t) &&
          options->api_version == KB_API_VERSION);
}

bool valid_flash_options(const kb_flash_options_t *options) noexcept {
  return options == nullptr ||
         (options->struct_size >= sizeof(kb_flash_options_t) &&
          options->api_version == KB_API_VERSION);
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
    return KB_E_NO_DEVICE;
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
    });
    return operation->public_error.get();
  } catch (...) {
    return nullptr;
  }
}

} // namespace

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

void KB_CALL kb_version_init(kb_version_t *version) {
  if (version == nullptr) {
    return;
  }
  *version = {};
  version->struct_size = sizeof(*version);
  version->api_version = KB_API_VERSION;
}

kb_status_t KB_CALL kb_get_version(kb_version_t *version) {
  if (version == nullptr || version->struct_size < sizeof(*version) ||
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
    if (options != nullptr) {
      result->options = *options;
    }
    auto runtime = acquire_usb_runtime();
    if (!runtime) {
      const auto status = runtime_error_status(runtime.error().kind);
      return fail(error, status, runtime_error_message(runtime.error().kind),
                  "", runtime.error().native_code);
    }
    result->usb_runtime = std::move(*runtime);
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
  if (context->usb_runtime == nullptr) {
    return fail(error, KB_E_INTERNAL, "context has no USB runtime");
  }

  kairosboot::transport::UsbInterfaceFilter filter;
  filter.interface_class = 0xFF;
  filter.interface_subclass = 0x42;
  filter.interface_protocol = 0x03;
  auto enumerated = context->usb_runtime->enumerate(filter);
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

    if (context->usb_runtime == nullptr) {
      return fail(error, KB_E_INTERNAL, "context has no USB runtime",
                  serial_or_null);
    }
    auto enumerated = context->usb_runtime->enumerate(fastboot_usb_filter());
    if (!enumerated) {
      return fail(error, kairosboot::api::normalize_public_error(
                             enumerated.error(), requested_identifier));
    }
    auto selected =
        kairosboot::api::select_usb_device(*enumerated, requested_serial);
    if (!selected) {
      return fail(error, kairosboot::api::normalize_public_error(
                             selected.error(), requested_identifier));
    }

    kb_flash_options_t flash_options;
    kb_flash_options_init(&flash_options);
    if (options_or_null != nullptr) {
      flash_options = *options_or_null;
    }

    std::shared_ptr<const kairosboot::image::IImageSource> image_source =
        std::move(*file_source);
    auto runtime = context->usb_runtime;
    auto device = std::move(*selected);
    auto selected_identifier = device_identifier(device);
    std::string partition_copy{partition_view};

    auto task = [runtime = std::move(runtime), device = std::move(device),
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

      kairosboot::transport::UsbFastbootTransportOptions transport_options;
      transport_options.bulk_out.timeout_ms = flash_options.timeout_ms;
      auto opened = kairosboot::transport::UsbFastbootTransport::open(
          runtime, device, std::move(transport_options));
      if (!opened) {
        return operation_failure(kairosboot::api::normalize_public_error(
            opened.error(), selected_identifier));
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

  const kb_status_t result = kb_operation_wait(operation, KB_WAIT_INFINITE);
  if (result != KB_OK && error != nullptr) {
    const kb_error_t *operation_error = kb_operation_error(operation);
    if (operation_error == nullptr) {
      (void)fail(error, result, kb_status_string(result));
    } else {
      (void)fail(error, result, kb_error_message(operation_error),
                 kb_error_device_identifier(operation_error),
                 kb_error_native_code(operation_error),
                 kb_error_transfer_state(operation_error));
    }
  }
  kb_operation_release(operation);
  return result;
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

void KB_CALL kb_operation_release(kb_operation_t *operation) {
  delete operation;
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

void KB_CALL kb_error_release(kb_error_t *error) { delete error; }

} // extern "C"
