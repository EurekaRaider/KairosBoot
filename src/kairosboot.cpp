#include <kairosboot/kairosboot.h>

#include "src/api/device_selection.hpp"
#include "src/api/device_selector.hpp"
#include "src/api/error_handle.hpp"
#include "src/api/command_result_handle.hpp"
#include "src/api/error_mapping.hpp"
#include "src/api/operation_state.hpp"
#include "src/fastboot/file_receive_service.hpp"
#include "src/fastboot/libusb_reconnect_adapters.hpp"
#include "src/fastboot/primitive_update_device.hpp"
#include "src/fastboot/primitive_service.hpp"
#include "src/fastboot/slot_planner.hpp"
#include "src/fastboot/super_optimizer.hpp"
#include "src/fastboot/update_executor.hpp"
#include "src/fastboot/update_package_preflight.hpp"
#include "src/fastboot/variable_parser.hpp"
#include "src/fleet/device_preflight.hpp"
#include "src/image/artifact_source.hpp"
#include "src/image/boot_image_builder.hpp"
#include "src/image/file_source.hpp"
#include "src/image/filesystem_formatter.hpp"
#include "src/image/flash_artifact.hpp"
#include "src/image/sparse_flash_plan.hpp"
#include "src/image/sha256.hpp"
#include "src/image/vendor_boot_repacker.hpp"
#include "src/image/vbmeta_flag_source.hpp"
#include "src/kairosboot_internal.hpp"
#include "src/protocol/fastboot_protocol.hpp"
#include "src/protocol/file_transfer_sink.hpp"
#include "src/transport/image_transfer_source.hpp"
#include "src/transport/buffer_budget.hpp"
#include "src/transport/libusb_runtime.hpp"
#include "src/transport/sequential_streaming_transport.hpp"
#include "src/transport/tcp_fastboot.hpp"
#include "src/transport/udp_fastboot.hpp"
#include "src/transport/usb_fastboot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <expected>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <span>
#include <utility>
#include <vector>

struct kb_context_usb_state {
  mutable std::mutex usb_runtime_mutex;
  std::shared_ptr<kairosboot::transport::LibusbRuntime> usb_runtime;
  std::uint16_t usb_vendor_id{};
};

struct kb_context {
  kb_context_options_t options{};
  std::shared_ptr<kb_context_usb_state> usb_state;
};

struct kb_device_operation_state {
  std::atomic_bool busy{false};
};

struct kb_device {
  std::atomic_size_t reference_count{1U};
  kb_context context;
  std::shared_ptr<kb_device_operation_state> operation_state;
  std::string selector;
  std::string identifier;
  std::string serial;
  std::string usb_path;
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

class DeviceOperationLease final {
public:
  explicit DeviceOperationLease(
      std::shared_ptr<kb_device_operation_state> state) noexcept
      : state_(std::move(state)) {}

  ~DeviceOperationLease() {
    state_->busy.store(false, std::memory_order_release);
  }

  DeviceOperationLease(const DeviceOperationLease &) = delete;
  DeviceOperationLease &operator=(const DeviceOperationLease &) = delete;

private:
  std::shared_ptr<kb_device_operation_state> state_;
};

[[nodiscard]] std::shared_ptr<DeviceOperationLease>
try_acquire_device_operation(kb_device_t &device) {
  auto state = device.operation_state;
  bool expected = false;
  if (!state->busy.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return {};
  }
  try {
    return std::make_shared<DeviceOperationLease>(state);
  } catch (...) {
    state->busy.store(false, std::memory_order_release);
    throw;
  }
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

[[nodiscard]] kairosboot::api::OperationErrorPayload local_flash_error(
    const kb_status_t status, std::string message,
    const std::string_view device_identifier) {
  return {
      .status = status,
      .message = std::move(message),
      .native_code = 0,
      .transfer_state = KB_TRANSFER_NOT_SENT,
      .device_identifier = std::string{device_identifier},
      .device_message = {},
      .command_messages = {},
      .inbound_expected = std::nullopt,
      .inbound_transferred = 0,
      .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
      .session_poisoned = false,
  };
}

[[nodiscard]] std::expected<
    std::shared_ptr<const kairosboot::image::IImageSource>,
    kairosboot::api::OperationErrorPayload>
open_raw_boot_part(const std::string_view path,
                   const std::string_view description,
                   const bool require_non_empty,
                   const std::string_view device_identifier) {
  auto source = kairosboot::image::FileImageSource::open(utf8_path(path));
  if (!source) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        source.error(), std::string{device_identifier}));
  }
  if (require_non_empty && (*source)->size() == 0U) {
    return std::unexpected(local_flash_error(
        KB_E_INVALID_ARGUMENT, std::string{description} + " must not be empty",
        device_identifier));
  }
  if ((*source)->size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(local_flash_error(
        KB_E_INVALID_ARGUMENT,
        std::string{description} + " exceeds UINT32_MAX bytes",
        device_identifier));
  }
  return std::shared_ptr<const kairosboot::image::IImageSource>{
      std::move(*source)};
}

[[nodiscard]] kairosboot::api::OperationErrorPayload boot_builder_error(
    const kairosboot::image::BootImageBuildError &error,
    const std::string_view device_identifier) {
  using kairosboot::image::BootImageBuildErrorKind;
  kb_status_t status = KB_E_IO;
  switch (error.kind) {
  case BootImageBuildErrorKind::InvalidArgument:
  case BootImageBuildErrorKind::SizeOverflow:
    status = KB_E_INVALID_ARGUMENT;
    break;
  case BootImageBuildErrorKind::Cancelled:
    status = KB_E_CANCELLED;
    break;
  case BootImageBuildErrorKind::Allocation:
    status = KB_E_OUT_OF_MEMORY;
    break;
  case BootImageBuildErrorKind::Source:
  case BootImageBuildErrorKind::Truncated:
    break;
  }
  return local_flash_error(status, error.message, device_identifier);
}

[[nodiscard]] std::expected<
    std::shared_ptr<const kairosboot::image::IImageSource>,
    kairosboot::api::OperationErrorPayload>
make_flash_raw_source(const std::string_view kernel_path,
                      const std::optional<std::string_view> ramdisk_path,
                      const std::optional<std::string_view> second_stage_path,
                      kairosboot::image::LegacyBootImageOptions options,
                      const std::string_view operation_name,
                      const std::string_view device_identifier) {
  const std::string kernel_description =
      std::string{operation_name} + " kernel";
  auto kernel = open_raw_boot_part(kernel_path, kernel_description, true,
                                   device_identifier);
  if (!kernel) {
    return std::unexpected(std::move(kernel.error()));
  }

  constexpr std::array<std::byte, 8> boot_magic{
      std::byte{'A'}, std::byte{'N'}, std::byte{'D'}, std::byte{'R'},
      std::byte{'O'}, std::byte{'I'}, std::byte{'D'}, std::byte{'!'}};
  constexpr std::size_t minimum_boot_header_v3 = 1580U;
  std::array<std::byte, boot_magic.size()> prefix{};
  auto prefix_read = (*kernel)->read_at(0U, prefix);
  if (!prefix_read) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        prefix_read.error(), std::string{device_identifier}));
  }
  const bool is_boot_image = *prefix_read == prefix.size() &&
                             std::ranges::equal(prefix, boot_magic);
  if (is_boot_image) {
    if ((*kernel)->size() < minimum_boot_header_v3) {
      return std::unexpected(local_flash_error(
          KB_E_INVALID_ARGUMENT,
          std::string{operation_name} +
              " boot image is too short to contain a valid header",
          device_identifier));
    }
    if (ramdisk_path.has_value() || second_stage_path.has_value()) {
      return std::unexpected(local_flash_error(
          KB_E_INVALID_ARGUMENT,
          std::string{operation_name} +
              " cannot combine an Android boot image with ramdisk or "
              "second-stage files",
          device_identifier));
    }
    const kairosboot::image::LegacyBootImageOptions defaults;
    if (options.header_version != defaults.header_version ||
        !options.os_version.empty() || !options.os_patch_level.empty() ||
        !options.dtb_path.empty() ||
        options.dtb_offset != defaults.dtb_offset) {
      return std::unexpected(local_flash_error(
          KB_E_INVALID_ARGUMENT,
          std::string{operation_name} +
              " cannot apply boot construction options to a prebuilt image",
          device_identifier));
    }
    return std::move(*kernel);
  }

  std::shared_ptr<const kairosboot::image::IImageSource> ramdisk;
  if (ramdisk_path.has_value()) {
    auto loaded = open_raw_boot_part(
        *ramdisk_path, std::string{operation_name} + " ramdisk",
        false, device_identifier);
    if (!loaded) {
      return std::unexpected(std::move(loaded.error()));
    }
    ramdisk = std::move(*loaded);
  }
  std::shared_ptr<const kairosboot::image::IImageSource> second_stage;
  if (second_stage_path.has_value()) {
    auto loaded = open_raw_boot_part(
        *second_stage_path, std::string{operation_name} + " second stage",
        false, device_identifier);
    if (!loaded) {
      return std::unexpected(std::move(loaded.error()));
    }
    second_stage = std::move(*loaded);
  }
  std::shared_ptr<const kairosboot::image::IImageSource> dtb;
  if (!options.dtb_path.empty()) {
    auto loaded = open_raw_boot_part(
        options.dtb_path, std::string{operation_name} + " DTB", true,
        device_identifier);
    if (!loaded) {
      return std::unexpected(std::move(loaded.error()));
    }
    dtb = std::move(*loaded);
  }
  options.maximum_output_bytes = std::numeric_limits<std::uint32_t>::max();
  auto built = kairosboot::image::build_boot_image(
      std::move(*kernel), std::move(ramdisk), std::move(second_stage),
      std::move(dtb), std::move(options));
  if (!built) {
    return std::unexpected(
        boot_builder_error(built.error(), device_identifier));
  }
  return std::shared_ptr<const kairosboot::image::IImageSource>{
      std::move(*built)};
}

kairosboot::transport::UsbInterfaceFilter
fastboot_usb_filter(const std::uint16_t vendor_id = 0U) {
  kairosboot::transport::UsbInterfaceFilter filter;
  if (vendor_id != 0U) {
    filter.vendor_id = vendor_id;
  }
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

kairosboot::api::OperationErrorPayload format_host_failure(
    const kairosboot::image::FilesystemFormatError &error,
    const std::string &device) {
  kb_status_t status = KB_E_IO;
  if (error.kind ==
      kairosboot::image::FilesystemFormatErrorKind::InvalidArgument) {
    status = KB_E_INVALID_ARGUMENT;
  } else if (error.kind ==
             kairosboot::image::FilesystemFormatErrorKind::Unsupported) {
    status = KB_E_NOT_SUPPORTED;
  }
  return {
      .status = status,
      .message = error.message,
      .native_code = error.native_code,
      .transfer_state = KB_TRANSFER_NOT_SENT,
      .device_identifier = device,
      .device_message = {},
      .command_messages = {},
      .inbound_expected = std::nullopt,
      .inbound_transferred = 0,
      .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
      .session_poisoned = false,
  };
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
  if (options == nullptr) {
    return true;
  }
  if (options->struct_size < KB_CONTEXT_OPTIONS_V1_SIZE ||
      options->api_version != KB_API_VERSION) {
    return false;
  }
  return options->struct_size < KB_CONTEXT_OPTIONS_VENDOR_ID_SIZE ||
         options->usb_vendor_id <=
             std::numeric_limits<std::uint16_t>::max();
}

bool valid_flash_options(const kb_flash_options_t *options) noexcept {
  if (options == nullptr) {
    return true;
  }
  if (options->struct_size < KB_FLASH_OPTIONS_V1_SIZE ||
      options->api_version != KB_API_VERSION) {
    return false;
  }
  if (options->struct_size >= KB_FLASH_OPTIONS_AVB_FLAGS_SIZE &&
      ((options->disable_verity != 0 && options->disable_verity != 1) ||
       (options->disable_verification != 0 &&
        options->disable_verification != 1))) {
    return false;
  }
  if (options->struct_size >=
          offsetof(kb_flash_options_t, force) + sizeof(options->force) &&
      options->force != 0 && options->force != 1) {
    return false;
  }
  if (options->struct_size >= KB_FLASH_OPTIONS_FORCE_FS_SIZE &&
      (options->filesystem_options & ~KB_FILESYSTEM_OPTIONS_ALL) != 0U) {
    return false;
  }
  if (options->struct_size <
      offsetof(kb_flash_options_t, set_active) + sizeof(options->set_active)) {
    return true;
  }
  if (options->set_active != 0 && options->set_active != 1) {
    return false;
  }
  return options->struct_size < KB_FLASH_OPTIONS_SLOT_POLICY_SIZE ||
         options->active_slot == nullptr || options->set_active == 1;
}

[[nodiscard]] kb_flash_options_t flash_options_or_default(
    const kb_flash_options_t *options) noexcept {
  kb_flash_options_t result{};
  result.struct_size = sizeof(result);
  result.api_version = KB_API_VERSION;
  result.timeout_ms = kDefaultTimeoutMs;
  if (options == nullptr) {
    return result;
  }
  result.timeout_ms = options->timeout_ms;
  result.progress_callback = options->progress_callback;
  result.progress_user_data = options->progress_user_data;
  if (options->struct_size >= KB_FLASH_OPTIONS_AVB_FLAGS_SIZE) {
    result.disable_verity = options->disable_verity;
    result.disable_verification = options->disable_verification;
  }
  if (options->struct_size >=
      offsetof(kb_flash_options_t, sparse_limit_bytes) +
          sizeof(options->sparse_limit_bytes)) {
    result.sparse_limit_bytes = options->sparse_limit_bytes;
  }
  if (options->struct_size >=
      offsetof(kb_flash_options_t, force) + sizeof(options->force)) {
    result.force = options->force;
  }
  if (options->struct_size >= KB_FLASH_OPTIONS_FORCE_FS_SIZE) {
    result.filesystem_options = options->filesystem_options;
  }
  return result;
}

bool valid_legacy_boot_options(
    const kb_legacy_boot_options_t *options) noexcept {
  return options == nullptr ||
         (options->struct_size >= KB_LEGACY_BOOT_OPTIONS_V1_SIZE &&
          options->api_version == KB_API_VERSION);
}

bool valid_legacy_boot_option_strings(
    const kb_legacy_boot_options_t *options) noexcept {
  if (options == nullptr) {
    return true;
  }
  if (options->command_line != nullptr &&
      !valid_utf8(options->command_line)) {
    return false;
  }
  return (options->struct_size <
              offsetof(kb_legacy_boot_options_t, os_version) +
                  sizeof(options->os_version) ||
          options->os_version == nullptr || valid_utf8(options->os_version)) &&
         (options->struct_size <
              offsetof(kb_legacy_boot_options_t, os_patch_level) +
                  sizeof(options->os_patch_level) ||
          options->os_patch_level == nullptr ||
          valid_utf8(options->os_patch_level)) &&
         (options->struct_size <
              offsetof(kb_legacy_boot_options_t, dtb_path) +
                  sizeof(options->dtb_path) ||
          options->dtb_path == nullptr || valid_utf8(options->dtb_path));
}

[[nodiscard]] kairosboot::image::LegacyBootImageOptions
legacy_boot_options_or_default(const kb_legacy_boot_options_t *options) {
  kairosboot::image::LegacyBootImageOptions result;
  if (options == nullptr) {
    return result;
  }
  result.command_line =
      options->command_line == nullptr ? "" : options->command_line;
  result.base = options->base;
  result.page_size = options->page_size;
  result.kernel_offset = options->kernel_offset;
  result.ramdisk_offset = options->ramdisk_offset;
  result.second_offset = options->second_offset;
  result.tags_offset = options->tags_offset;
  if (options->struct_size >=
      offsetof(kb_legacy_boot_options_t, header_version) +
          sizeof(options->header_version)) {
    result.header_version = options->header_version;
  }
  if (options->struct_size >=
      offsetof(kb_legacy_boot_options_t, os_version) +
          sizeof(options->os_version)) {
    result.os_version =
        options->os_version == nullptr ? "" : options->os_version;
  }
  if (options->struct_size >=
      offsetof(kb_legacy_boot_options_t, os_patch_level) +
          sizeof(options->os_patch_level)) {
    result.os_patch_level = options->os_patch_level == nullptr
                                ? ""
                                : options->os_patch_level;
  }
  if (options->struct_size >=
      offsetof(kb_legacy_boot_options_t, dtb_path) +
          sizeof(options->dtb_path)) {
    result.dtb_path =
        options->dtb_path == nullptr ? "" : options->dtb_path;
  }
  if (options->struct_size >= KB_LEGACY_BOOT_OPTIONS_MODERN_SIZE) {
    result.dtb_offset = options->dtb_offset;
  }
  return result;
}

bool valid_update_options(const kb_update_options_t *options) noexcept {
  if (options == nullptr) {
    return true;
  }
  const auto valid_optional_bool = [options](const std::size_t offset) noexcept {
    if (options->struct_size < offset + sizeof(int32_t)) {
      return true;
    }
    int32_t value{};
    std::memcpy(&value,
                reinterpret_cast<const std::byte *>(options) + offset,
                sizeof(value));
    return value == 0 || value == 1;
  };
  if (!(options->struct_size >= KB_UPDATE_OPTIONS_V1_SIZE &&
         options->api_version == KB_API_VERSION &&
         (options->wipe == 0 || options->wipe == 1) &&
         valid_optional_bool(offsetof(kb_update_options_t, skip_reboot)) &&
         valid_optional_bool(offsetof(kb_update_options_t, skip_secondary)) &&
         valid_optional_bool(
             offsetof(kb_update_options_t, exclude_dynamic_partitions)) &&
         valid_optional_bool(
             offsetof(kb_update_options_t, disable_fastboot_info)) &&
         valid_optional_bool(offsetof(kb_update_options_t, disable_verity)) &&
         valid_optional_bool(
             offsetof(kb_update_options_t, disable_verification)) &&
         valid_optional_bool(offsetof(kb_update_options_t, force)) &&
         valid_optional_bool(
             offsetof(kb_update_options_t, disable_super_optimization)))) {
    return false;
  }
  if (options->struct_size >= KB_UPDATE_OPTIONS_FORCE_FS_SIZE &&
      (options->filesystem_options & ~KB_FILESYSTEM_OPTIONS_ALL) != 0U) {
    return false;
  }
  if (options->struct_size <
      offsetof(kb_update_options_t, set_active) + sizeof(options->set_active)) {
    return true;
  }
  if (options->set_active != 0 && options->set_active != 1) {
    return false;
  }
  return options->struct_size < KB_UPDATE_OPTIONS_SLOT_POLICY_SIZE ||
         options->active_slot == nullptr || options->set_active == 1;
}

[[nodiscard]] kairosboot::image::VbmetaFlags flash_vbmeta_flags(
    const kb_flash_options_t &options) noexcept {
  if (options.struct_size < KB_FLASH_OPTIONS_AVB_FLAGS_SIZE) {
    return {};
  }
  return {.disable_verity = options.disable_verity != 0,
          .disable_verification = options.disable_verification != 0};
}

[[nodiscard]] kairosboot::image::VbmetaFlags update_vbmeta_flags(
    const kb_update_options_t &options) noexcept {
  if (options.struct_size < KB_UPDATE_OPTIONS_AVB_FLAGS_SIZE) {
    return {};
  }
  return {.disable_verity = options.disable_verity != 0,
          .disable_verification = options.disable_verification != 0};
}

struct SlotPolicy final {
  std::optional<std::string> slot;
  bool set_active{};
  std::optional<std::string> active_slot;
};

struct CopiedUpdateOptions final {
  kb_update_options_t native{};
  SlotPolicy slot_policy;
};

[[nodiscard]] kairosboot::api::OperationErrorPayload update_error(
    kb_status_t status, std::string message, std::string_view identifier,
    kb_transfer_state_t transfer_state = KB_TRANSFER_NOT_SENT);

[[nodiscard]] std::expected<SlotPolicy, std::string> copy_slot_policy(
    const char *slot, const int32_t set_active,
    const char *active_slot) {
  SlotPolicy result;
  const auto copy_slot = [](const char *value,
                            const std::string_view description)
      -> std::expected<std::optional<std::string>, std::string> {
    if (value == nullptr) {
      return std::optional<std::string>{};
    }
    const std::string_view view{value};
    if (view.empty()) {
      return std::unexpected(std::string{description} +
                             " must not be empty");
    }
    auto parsed = kairosboot::fastboot::parse_slot_selection(view);
    if (!parsed) {
      return std::unexpected(parsed.error().message);
    }
    return std::optional<std::string>{std::string{view}};
  };

  auto copied_slot = copy_slot(slot, "slot");
  if (!copied_slot) {
    return std::unexpected(std::move(copied_slot.error()));
  }
  result.slot = std::move(*copied_slot);
  result.set_active = set_active != 0;
  auto copied_active = copy_slot(active_slot, "active slot");
  if (!copied_active) {
    return std::unexpected(std::move(copied_active.error()));
  }
  result.active_slot = std::move(*copied_active);
  return result;
}

[[nodiscard]] std::expected<SlotPolicy, std::string>
copy_flash_slot_policy(const kb_flash_options_t *options) {
  if (options == nullptr) {
    return SlotPolicy{};
  }
  const char *slot = nullptr;
  int32_t set_active = 0;
  const char *active_slot = nullptr;
  if (options->struct_size >=
      offsetof(kb_flash_options_t, slot) + sizeof(options->slot)) {
    slot = options->slot;
  }
  if (options->struct_size >=
      offsetof(kb_flash_options_t, set_active) + sizeof(options->set_active)) {
    set_active = options->set_active;
  }
  if (options->struct_size >= KB_FLASH_OPTIONS_SLOT_POLICY_SIZE) {
    active_slot = options->active_slot;
  }
  return copy_slot_policy(slot, set_active, active_slot);
}

[[nodiscard]] std::expected<CopiedUpdateOptions, std::string>
update_options_or_default(const kb_update_options_t *options) {
  CopiedUpdateOptions result;
  result.native.struct_size = sizeof(result.native);
  result.native.api_version = KB_API_VERSION;
  result.native.timeout_ms = kDefaultTimeoutMs;
  if (options != nullptr) {
    result.native.timeout_ms = options->timeout_ms;
    result.native.wipe = options->wipe;
    result.native.progress_callback = options->progress_callback;
    result.native.progress_user_data = options->progress_user_data;
    if (options->struct_size >=
        offsetof(kb_update_options_t, skip_reboot) +
            sizeof(options->skip_reboot)) {
      result.native.skip_reboot = options->skip_reboot;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, skip_secondary) +
            sizeof(options->skip_secondary)) {
      result.native.skip_secondary = options->skip_secondary;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, exclude_dynamic_partitions) +
            sizeof(options->exclude_dynamic_partitions)) {
      result.native.exclude_dynamic_partitions =
          options->exclude_dynamic_partitions;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, disable_fastboot_info) +
            sizeof(options->disable_fastboot_info)) {
      result.native.disable_fastboot_info = options->disable_fastboot_info;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, disable_verity) +
            sizeof(options->disable_verity)) {
      result.native.disable_verity = options->disable_verity;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, disable_verification) +
            sizeof(options->disable_verification)) {
      result.native.disable_verification = options->disable_verification;
    }
    const char *slot = nullptr;
    int32_t set_active = 0;
    const char *active_slot = nullptr;
    if (options->struct_size >=
        offsetof(kb_update_options_t, slot) + sizeof(options->slot)) {
      slot = options->slot;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, set_active) + sizeof(options->set_active)) {
      set_active = options->set_active;
    }
    if (options->struct_size >= KB_UPDATE_OPTIONS_SLOT_POLICY_SIZE) {
      active_slot = options->active_slot;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, sparse_limit_bytes) +
            sizeof(options->sparse_limit_bytes)) {
      result.native.sparse_limit_bytes = options->sparse_limit_bytes;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, force) + sizeof(options->force)) {
      result.native.force = options->force;
    }
    if (options->struct_size >= KB_UPDATE_OPTIONS_FORCE_FS_SIZE) {
      result.native.filesystem_options = options->filesystem_options;
    }
    if (options->struct_size >=
        offsetof(kb_update_options_t, disable_super_optimization) +
            sizeof(options->disable_super_optimization)) {
      result.native.disable_super_optimization =
          options->disable_super_optimization;
    }
    auto policy = copy_slot_policy(slot, set_active, active_slot);
    if (!policy) {
      return std::unexpected(std::move(policy.error()));
    }
    result.slot_policy = std::move(*policy);
  }
  return result;
}

[[nodiscard]] kairosboot::api::OperationErrorPayload slot_error(
    const kairosboot::fastboot::SlotError &error,
    const std::string_view identifier) {
  if (error.query_error) {
    auto result = kairosboot::api::normalize_public_error(
        *error.query_error, identifier);
    result.message = error.message;
    if (error.query_error->code ==
        kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
      result.status = KB_E_NOT_SUPPORTED;
    }
    return result;
  }
  kb_status_t status = KB_E_INVALID_ARGUMENT;
  switch (error.code) {
  case kairosboot::fastboot::SlotErrorCode::Unsupported:
    status = KB_E_NOT_SUPPORTED;
    break;
  case kairosboot::fastboot::SlotErrorCode::Cancelled:
    status = KB_E_CANCELLED;
    break;
  case kairosboot::fastboot::SlotErrorCode::TimedOut:
    status = KB_E_TIMEOUT;
    break;
  case kairosboot::fastboot::SlotErrorCode::InvalidDeviceResponse:
  case kairosboot::fastboot::SlotErrorCode::QueryFailed:
    status = KB_E_PROTOCOL;
    break;
  case kairosboot::fastboot::SlotErrorCode::InvalidArgument:
  case kairosboot::fastboot::SlotErrorCode::Ambiguous:
    status = KB_E_INVALID_ARGUMENT;
    break;
  }
  return update_error(status, error.message, identifier);
}

[[nodiscard]] std::expected<kairosboot::fastboot::PartitionSlotPlan,
                            kairosboot::fastboot::SlotError>
plan_partition_slots(
    kairosboot::fastboot::SlotPlanner &planner,
    const std::string_view partition, const std::string_view requested_slot,
    const kairosboot::fastboot::UpdateOperationContext &context) {
  const auto separator = partition.find(':');
  const auto base = partition.substr(0U, separator);
  auto plan = planner.plan_partition(base, requested_slot, context);
  if (!plan || separator == std::string_view::npos) {
    return plan;
  }
  const auto suffix = partition.substr(separator);
  for (auto &name : plan->partition_names) {
    name.append(suffix);
  }
  return plan;
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
    const kb_transfer_state_t transfer_state) {
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

[[nodiscard]] kairosboot::api::OperationErrorPayload vbmeta_error(
    const kairosboot::image::VbmetaFlagError &error,
    const std::string_view identifier) {
  using kairosboot::image::VbmetaFlagErrorKind;
  kb_status_t status = KB_E_IO;
  if (error.kind == VbmetaFlagErrorKind::Malformed) {
    status = KB_E_INVALID_ARGUMENT;
  } else if (error.kind == VbmetaFlagErrorKind::Cancelled) {
    status = KB_E_CANCELLED;
  }
  return update_error(status,
                      error.message + " (input offset " +
                          std::to_string(error.input_offset) + ")",
                      identifier);
}

[[nodiscard]] kairosboot::api::OperationErrorPayload sha256_error(
    const kairosboot::image::Sha256Error &error,
    const std::string_view identifier) {
  using kairosboot::image::Sha256ErrorKind;
  const auto status = error.kind == Sha256ErrorKind::Cancelled
                          ? KB_E_CANCELLED
                          : KB_E_IO;
  return update_error(status,
                      error.message + " (input offset " +
                          std::to_string(error.input_offset) + ")",
                      identifier);
}

[[nodiscard]] std::expected<void, kairosboot::api::OperationErrorPayload>
prepare_update_vbmeta_flags(
    kairosboot::fastboot::PreparedUpdatePackage &prepared,
    const kairosboot::image::VbmetaFlags flags,
    const std::string_view identifier,
    const std::stop_token cancellation) {
  for (auto &task : prepared.plan.tasks) {
    if (!task.apply_vbmeta) {
      continue;
    }
    if (flags.any()) {
      const auto position = std::find_if(
          prepared.artifacts.begin(), prepared.artifacts.end(),
          [&task](const kairosboot::fastboot::PreparedUpdateArtifact &value) {
            return value.name == task.artifact;
          });
      if (position == prepared.artifacts.end() || !position->resolved ||
          !position->resolved->source) {
        return std::unexpected(update_error(
            KB_E_INTERNAL,
            "vbmeta update task does not map to a prepared artifact",
            identifier));
      }
      auto mutated = kairosboot::image::apply_vbmeta_flags(
          position->resolved->source, flags, cancellation);
      if (!mutated) {
        return std::unexpected(vbmeta_error(mutated.error(), identifier));
      }
      if (*mutated != position->resolved->source) {
        auto digest = kairosboot::image::compute_sha256(**mutated, cancellation);
        if (!digest) {
          return std::unexpected(sha256_error(digest.error(), identifier));
        }
        auto inspected =
            kairosboot::image::FlashArtifact::inspect(*mutated, cancellation);
        if (!inspected) {
          return std::unexpected(kairosboot::api::normalize_public_error(
              inspected.error(), identifier));
        }
        auto resolved =
            std::make_shared<const kairosboot::image::ResolvedArtifact>(
                kairosboot::image::ResolvedArtifact{
                    .source = std::move(*mutated),
                    .sha256 = *digest,
                    .origin = position->resolved->origin,
                    .logical_name = position->resolved->logical_name});
        position->resolved = std::move(resolved);
        position->artifact =
            std::make_shared<const kairosboot::image::FlashArtifact>(
                std::move(*inspected));
      }
    }
    task.apply_vbmeta = false;
  }
  return {};
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

const char *device_error_identifier(const kb_device_t *device) noexcept {
  return device == nullptr ? nullptr : device->identifier.c_str();
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

class OffsetTransferSink final : public kairosboot::protocol::ITransferSink {
public:
  OffsetTransferSink(
      std::shared_ptr<kairosboot::protocol::FileTransferSink> sink,
      const std::uint64_t base) noexcept
      : sink_(std::move(sink)), base_(base) {}

  [[nodiscard]] kairosboot::protocol::TransferResult write(
      const std::uint64_t offset,
      const std::span<const std::byte> source) noexcept override {
    if (offset > std::numeric_limits<std::uint64_t>::max() - base_) {
      return {
          .status = kairosboot::protocol::TransportStatus::IoError,
          .transferred = 0,
          .certainty =
              kairosboot::protocol::TransferCertainty::NotTransferred,
          .detail = "vendor_boot fetch offset overflow",
      };
    }
    return sink_->write(base_ + offset, source);
  }

private:
  std::shared_ptr<kairosboot::protocol::FileTransferSink> sink_;
  std::uint64_t base_{};
};

class ScopedTemporaryFile final {
public:
  explicit ScopedTemporaryFile(std::filesystem::path path) noexcept
      : path_(std::move(path)) {}
  ScopedTemporaryFile(const ScopedTemporaryFile &) = delete;
  ScopedTemporaryFile &operator=(const ScopedTemporaryFile &) = delete;
  ScopedTemporaryFile(ScopedTemporaryFile &&) = delete;
  ScopedTemporaryFile &operator=(ScopedTemporaryFile &&) = delete;
  ~ScopedTemporaryFile() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(path_, ignored));
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::expected<std::filesystem::path,
                            kairosboot::api::OperationErrorPayload>
vendor_boot_temporary_path(const std::string_view device_identifier) {
  std::error_code error;
  const auto directory = std::filesystem::temp_directory_path(error);
  if (error || directory.empty()) {
    auto payload = local_flash_error(
        KB_E_IO, "unable to locate the temporary directory for vendor_boot",
        device_identifier);
    payload.native_code = error.value();
    return std::unexpected(std::move(payload));
  }
  constexpr std::string_view hexadecimal{"0123456789abcdef"};
  std::random_device random;
  for (std::size_t attempt = 0U; attempt < 32U; ++attempt) {
    std::string name{".kairosboot-vendor-boot-"};
    name.reserve(name.size() + 32U + 4U);
    for (std::size_t index = 0U; index < 4U; ++index) {
      const auto value = static_cast<std::uint32_t>(random());
      for (std::size_t nibble = 0U; nibble < 8U; ++nibble) {
        name.push_back(hexadecimal[(value >> (nibble * 4U)) & 0x0fU]);
      }
    }
    name += ".img";
    const auto candidate = directory / name;
    const bool exists = std::filesystem::exists(candidate, error);
    if (!error && !exists) {
      return candidate;
    }
    error.clear();
  }
  return std::unexpected(local_flash_error(
      KB_E_IO, "unable to reserve a unique vendor_boot temporary path",
      device_identifier));
}

[[nodiscard]] kairosboot::api::OperationErrorPayload vendor_boot_repack_error(
    const kairosboot::image::VendorBootRepackError &error,
    const std::string_view device_identifier) {
  using kairosboot::image::VendorBootRepackErrorKind;
  kb_status_t status = KB_E_IO;
  switch (error.kind) {
  case VendorBootRepackErrorKind::InvalidArgument:
  case VendorBootRepackErrorKind::Malformed:
  case VendorBootRepackErrorKind::SizeOverflow:
    status = KB_E_INVALID_ARGUMENT;
    break;
  case VendorBootRepackErrorKind::Unsupported:
    status = KB_E_NOT_SUPPORTED;
    break;
  case VendorBootRepackErrorKind::Cancelled:
    status = KB_E_CANCELLED;
    break;
  case VendorBootRepackErrorKind::Allocation:
    status = KB_E_OUT_OF_MEMORY;
    break;
  case VendorBootRepackErrorKind::Source:
    break;
  }
  return local_flash_error(status, error.message, device_identifier);
}

[[nodiscard]] kairosboot::api::OperationErrorPayload file_sink_error(
    const kairosboot::protocol::FileTransferSinkError &error,
    const std::string_view device_identifier) {
  auto payload = local_flash_error(KB_E_IO, error.message, device_identifier);
  payload.native_code = error.native_code;
  return payload;
}

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

using FileReceiveExecutor = std::function<std::expected<
    kairosboot::fastboot::FileReceiveResult,
    kairosboot::fastboot::FileReceiveError>(
    kairosboot::fastboot::FileReceiveService &,
    const std::filesystem::path &, std::uint64_t,
    const kairosboot::protocol::TransferProgressObserver &)>;

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

[[nodiscard]] kairosboot::fastboot::PrimitiveError primitive_validation_error(
    const kairosboot::fastboot::PrimitiveOperation operation,
    std::string message) {
  return kairosboot::fastboot::PrimitiveError{
      .code = kairosboot::fastboot::PrimitiveErrorCode::InvalidArgument,
      .operation = operation,
      .phase = kairosboot::protocol::ProtocolPhase::Validation,
      .message = std::move(message),
  };
}

[[nodiscard]] std::expected<kairosboot::fastboot::PrimitiveReply,
                            kairosboot::fastboot::PrimitiveError>
aosp_erase_partition(kairosboot::fastboot::PrimitiveService &service,
                     const std::string_view partition) {
  const auto separator = partition.find(':');
  const auto base = partition.substr(0U, separator);
  std::string resolved{partition};

  auto has_slot = service.getvar("has-slot:" + std::string{base});
  if (!has_slot && has_slot.error().code !=
                       kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
    return std::unexpected(std::move(has_slot.error()));
  }
  if (has_slot && has_slot->terminal.payload == "yes") {
    auto current_slot = service.getvar("current-slot");
    if (!current_slot) {
      return std::unexpected(std::move(current_slot.error()));
    }
    auto slot = std::move(current_slot->terminal.payload);
    if (!slot.empty() && slot.front() == '_') {
      slot.erase(slot.begin());
    }
    if (slot.empty()) {
      return std::unexpected(primitive_validation_error(
          kairosboot::fastboot::PrimitiveOperation::Erase,
          "device returned an empty current-slot for a slotted partition"));
    }
    resolved = std::string{base} + "_" + slot;
    if (separator != std::string_view::npos) {
      resolved.append(partition.substr(separator));
    }
  }

  auto partition_type = service.getvar("partition-type:" + resolved);
  if (!partition_type && partition_type.error().code !=
                             kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
    return std::unexpected(std::move(partition_type.error()));
  }
  return service.erase(resolved);
}

[[nodiscard]] std::expected<kairosboot::fastboot::PrimitiveReply,
                            kairosboot::fastboot::PrimitiveError>
aosp_set_active(kairosboot::fastboot::PrimitiveService &service,
                const std::string_view requested_slot) {
  auto count_reply = service.getvar("slot-count");
  if (!count_reply) {
    return std::unexpected(std::move(count_reply.error()));
  }
  unsigned int count = 0;
  const auto &payload = count_reply->terminal.payload;
  const auto parsed = std::from_chars(
      payload.data(), payload.data() + payload.size(), count, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != payload.data() + payload.size() ||
      count == 0U || count > 26U) {
    return std::unexpected(primitive_validation_error(
        kairosboot::fastboot::PrimitiveOperation::SetActive,
        "Fastboot slot-count must be a decimal integer in the range 1..26"));
  }

  std::string slot{requested_slot};
  if (slot == "all") {
    slot = "a";
  } else if (slot == "other") {
    auto current_reply = service.getvar("current-slot");
    if (!current_reply) {
      return std::unexpected(std::move(current_reply.error()));
    }
    auto current = std::move(current_reply->terminal.payload);
    if (!current.empty() && current.front() == '_') {
      current.erase(current.begin());
    }
    if (current.size() != 1U || current.front() < 'a' ||
        static_cast<unsigned int>(current.front() - 'a') >= count) {
      return std::unexpected(primitive_validation_error(
          kairosboot::fastboot::PrimitiveOperation::SetActive,
          "Fastboot current-slot is not present in the reported slot topology"));
    }
    slot.assign(1U, static_cast<char>(
                        (static_cast<unsigned int>(current.front() - 'a') + 1U) %
                            count +
                        static_cast<unsigned int>('a')));
  }

  if (slot.size() != 1U || slot.front() < 'a' ||
      static_cast<unsigned int>(slot.front() - 'a') >= count) {
    return std::unexpected(primitive_validation_error(
        kairosboot::fastboot::PrimitiveOperation::SetActive,
        "requested Fastboot slot does not exist on the device"));
  }
  return service.set_active(slot);
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
    std::shared_ptr<kairosboot::transport::LibusbRuntime> usb_runtime,
    const std::uint16_t usb_vendor_id = 0U) {
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

  auto devices = usb_runtime->enumerate(fastboot_usb_filter(usb_vendor_id));
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
  return bind_target(std::move(*parsed), std::move(*runtime),
                     context.usb_state->usb_vendor_id);
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
  result->received_bytes = result->data.size();
  result->device_identifier = identifier;
  return result;
}

[[nodiscard]] std::shared_ptr<const kairosboot::api::CommandResultPayload>
make_file_command_result(
    kairosboot::fastboot::FileReceiveResult execution,
    std::string output_path, const std::string &identifier) {
  auto result = std::make_shared<kairosboot::api::CommandResultPayload>();
  result->terminal_payload = std::move(execution.reply.terminal.payload);
  result->messages = command_messages(execution.reply.informational);
  result->output_path = std::move(output_path);
  result->received_bytes = execution.bytes_published;
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
    kb_device_t *device,
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
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  const auto *selector_text = device->selector.c_str();
  if (!valid_command_options(options)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "command options have an incompatible size, API version, or receive bound",
                selector_text);
  }

  try {
    auto operation_lease = try_acquire_device_operation(*device);
    if (!operation_lease) {
      return fail(error, KB_E_BUSY,
                  "device already has an active protocol operation",
                  device->identifier.c_str());
    }
    auto target = prepare_target(device->context, selector_text);
    if (!target) {
      return fail(error, target.error());
    }
    auto copied_options = command_options_or_default(options);
    auto identifier = target->selector.identifier;
    auto task = [operation_lease = std::move(operation_lease),
                 target = std::move(*target), copied_options,
                 executor = std::move(executor),
                 identifier = std::move(identifier)](
                    kairosboot::api::OperationState::TaskContext &task_context)
        mutable -> kairosboot::api::OperationOutcome {
      static_cast<void>(operation_lease);
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

kb_status_t start_file_receive_async(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options, const char *progress_stage,
    FileReceiveExecutor executor, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  const auto *selector_text = device->selector.c_str();
  if (output_path == nullptr || output_path[0] == '\0' ||
      !valid_utf8(output_path)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "receive output path must be non-empty UTF-8", selector_text);
  }
  if (!valid_command_options(options)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "command options have an incompatible size, API version, or receive bound",
                selector_text);
  }

  try {
    auto operation_lease = try_acquire_device_operation(*device);
    if (!operation_lease) {
      return fail(error, KB_E_BUSY,
                  "device already has an active protocol operation",
                  device->identifier.c_str());
    }
    auto target = prepare_target(device->context, selector_text);
    if (!target) {
      return fail(error, target.error());
    }
    auto copied_options = command_options_or_default(options);
    auto identifier = target->selector.identifier;
    std::string output_path_copy{output_path};
    auto destination = utf8_path(output_path_copy);
    std::string stage{progress_stage};
    auto task = [operation_lease = std::move(operation_lease),
                 target = std::move(*target), copied_options,
                 executor = std::move(executor),
                 identifier = std::move(identifier),
                 output_path_copy = std::move(output_path_copy),
                 destination = std::move(destination), stage = std::move(stage)](
                    kairosboot::api::OperationState::TaskContext &task_context)
        mutable -> kairosboot::api::OperationOutcome {
      static_cast<void>(operation_lease);
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
      kairosboot::fastboot::PrimitiveService primitives(session);
      kairosboot::fastboot::FileReceiveService files(primitives);
      auto cancellation = task_context.register_cancellation_hook(
          [&files] { files.request_cancel(); });
      const kairosboot::protocol::TransferProgressObserver observer =
          [&task_context, &copied_options, &identifier, &stage](
              const std::uint64_t completed, const std::uint64_t total) {
            return task_context.cancel_requested() ||
                           !report_command_progress(copied_options, completed,
                                                    total, stage.c_str(),
                                                    identifier)
                       ? kairosboot::protocol::TransferProgressAction::cancel
                       : kairosboot::protocol::TransferProgressAction::
                             continue_transfer;
          };
      auto executed = executor(files, destination,
                               copied_options.maximum_receive_bytes, observer);
      if (!executed) {
        return operation_failure(kairosboot::api::normalize_public_error(
            executed.error(), identifier));
      }
      return kairosboot::api::OperationOutcome::succeeded(
          make_file_command_result(std::move(*executed),
                                   std::move(output_path_copy), identifier));
    };
    auto result = std::make_unique<kb_operation>(std::move(task));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL,
                  "unable to start the Fastboot file receive operation",
                  selector_text);
    }
    *operation = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the Fastboot file receive operation",
                selector_text);
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_INVALID_ARGUMENT, exception.what(), selector_text,
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the Fastboot file receive operation",
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
  case UpdateExecutionEventKind::TaskSkipped:
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

[[nodiscard]] std::expected<void, kairosboot::api::OperationErrorPayload>
apply_update_slot_policy(
    kairosboot::fastboot::PreparedUpdatePackage &prepared,
    kairosboot::fastboot::PrimitiveService &service,
    const SlotPolicy &policy,
    const kairosboot::fastboot::UpdateOperationContext &context,
    const std::string_view identifier) {
  kairosboot::fastboot::SlotPlanner planner(service);
  if (policy.slot) {
    auto selection = kairosboot::fastboot::parse_slot_selection(*policy.slot);
    if (!selection) {
      return std::unexpected(slot_error(selection.error(), identifier));
    }
    std::vector<kairosboot::fastboot::PlannedUpdateTask> expanded;
    for (const auto &task : prepared.plan.tasks) {
      if (task.kind != kairosboot::fastboot::UpdateTaskKind::Flash) {
        expanded.push_back(task);
        continue;
      }

      std::string requested = *policy.slot;
      if (task.slot == kairosboot::fastboot::PlannedSlot::Other) {
        switch (selection->kind) {
        case kairosboot::fastboot::SlotSelectionKind::Explicit: {
          auto topology = planner.query_topology(context);
          if (!topology) {
            return std::unexpected(slot_error(topology.error(), identifier));
          }
          if (topology->slots.size() != 2U) {
            return std::unexpected(update_error(
                KB_E_INVALID_ARGUMENT,
                "secondary update slot is ambiguous unless the device has exactly two slots",
                identifier));
          }
          if (topology->slots.front() == selection->name) {
            requested = topology->slots.back();
          } else if (topology->slots.back() == selection->name) {
            requested = topology->slots.front();
          } else {
            return std::unexpected(update_error(
                KB_E_INVALID_ARGUMENT,
                "requested update slot does not exist on the device",
                identifier));
          }
          break;
        }
        case kairosboot::fastboot::SlotSelectionKind::Other:
          requested.clear();
          break;
        case kairosboot::fastboot::SlotSelectionKind::All:
        case kairosboot::fastboot::SlotSelectionKind::Current:
          requested = "other";
          break;
        }
      }

      auto partitions = plan_partition_slots(
          planner, task.partition, requested, context);
      if (!partitions) {
        return std::unexpected(slot_error(partitions.error(), identifier));
      }
      for (auto &partition : partitions->partition_names) {
        if (expanded.size() >=
            kairosboot::fastboot::UpdatePlanLimits{}.maximum_tasks) {
          return std::unexpected(update_error(
              KB_E_INVALID_ARGUMENT,
              "slot-expanded update plan exceeds the task limit",
              identifier));
        }
        auto resolved = task;
        resolved.partition = std::move(partition);
        resolved.slot = kairosboot::fastboot::PlannedSlot::Default;
        expanded.push_back(std::move(resolved));
      }
    }
    prepared.plan.tasks = std::move(expanded);
  }

  return {};
}

[[nodiscard]] std::expected<void, kairosboot::api::OperationErrorPayload>
activate_update_slot_policy(
    kairosboot::fastboot::PrimitiveService &service,
    const SlotPolicy &policy,
    const kairosboot::fastboot::UpdateOperationContext &context,
    const std::string_view identifier) {
  if (!policy.set_active) {
    return {};
  }
  kairosboot::fastboot::SlotPlanner planner(service);
  const std::string_view requested = policy.active_slot
      ? std::string_view{*policy.active_slot}
      : policy.slot ? std::string_view{*policy.slot} : std::string_view{};
  std::string active;
  if (requested == "all") {
    auto topology = planner.query_topology(context);
    if (!topology) {
      return std::unexpected(slot_error(topology.error(), identifier));
    }
    active = topology->slots.front();
  } else {
    auto resolved = planner.resolve_active_slot(requested, context);
    if (!resolved) {
      return std::unexpected(slot_error(resolved.error(), identifier));
    }
    active = std::move(*resolved);
  }
  auto activated = service.set_active(active);
  if (!activated) {
    return std::unexpected(kairosboot::api::normalize_public_error(
        activated.error(), identifier));
  }
  return {};
}

[[nodiscard]] std::optional<std::string> optional_update_getvar(
    kairosboot::fastboot::PrimitiveService &service,
    const std::string_view name) {
  auto value = service.getvar(name);
  if (!value) {
    return std::nullopt;
  }
  return value->terminal.payload;
}

[[nodiscard]] std::optional<std::uint64_t> parse_update_size(
    std::string_view text) noexcept {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2U);
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        result, 16);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
               ? std::optional<std::uint64_t>{result}
               : std::nullopt;
  }
  std::uint64_t result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      result, 10);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
             ? std::optional<std::uint64_t>{result}
             : std::nullopt;
}

[[nodiscard]] std::expected<void, kairosboot::api::OperationErrorPayload>
try_optimize_public_super(
    kairosboot::fastboot::PreparedUpdatePackage &prepared,
    kairosboot::fastboot::PrimitiveService &service,
    const kb_update_options_t &options,
    const SlotPolicy &slot_policy,
    const std::string_view identifier,
    const std::stop_token cancellation) {
  if (options.disable_super_optimization != 0 ||
      !kairosboot::fastboot::has_super_optimization_candidate(prepared) ||
      (slot_policy.slot && *slot_policy.slot == "all")) {
    return {};
  }
  auto current_slot = optional_update_getvar(service, "current-slot");
  if ((!current_slot || current_slot->empty()) && slot_policy.slot) {
    // Global slot expansion has already resolved every flash task to an exact
    // partition name; a non-empty marker avoids requiring current-slot again.
    current_slot = *slot_policy.slot;
  }
  auto super_name = optional_update_getvar(service, "super-partition-name");
  if (!super_name || super_name->empty()) {
    super_name = std::string{"super"};
  }
  auto reported = optional_update_getvar(
      service, "partition-size:" + *super_name);
  std::uint64_t super_size = 0U;
  if (reported) {
    const auto parsed = parse_update_size(*reported);
    if (!parsed || *parsed == 0U) {
      return std::unexpected(update_error(
          KB_E_INVALID_ARGUMENT,
          "device reported an invalid super partition size before optimization",
          identifier));
    }
    super_size = *parsed;
  }
  auto optimized = kairosboot::fastboot::optimize_prepared_super(
      prepared,
      kairosboot::fastboot::SuperOptimizationDeviceInfo{
          .super_partition = std::move(*super_name),
          .current_slot = current_slot ? std::move(*current_slot) : std::string{},
          .super_partition_size = super_size,
      },
      cancellation);
  if (!optimized) {
    return std::unexpected(update_error(
        optimized.error().kind ==
                kairosboot::fastboot::SuperOptimizationErrorKind::Cancelled
            ? KB_E_CANCELLED
            : KB_E_INVALID_ARGUMENT,
        "super optimization preflight failed: " + optimized.error().message,
        identifier));
  }
  return {};
}

[[nodiscard]] kb_transfer_state_t public_update_transfer_state(
    const kairosboot::protocol::TransferCertainty certainty) noexcept {
  switch (certainty) {
  case kairosboot::protocol::TransferCertainty::NotTransferred:
    return KB_TRANSFER_NOT_SENT;
  case kairosboot::protocol::TransferCertainty::PartialOrUnknown:
    return KB_TRANSFER_PARTIAL_OR_UNKNOWN;
  case kairosboot::protocol::TransferCertainty::FullyTransferred:
    return KB_TRANSFER_FULLY_TRANSFERRED;
  }
  return KB_TRANSFER_PARTIAL_OR_UNKNOWN;
}

[[nodiscard]] kairosboot::api::OperationErrorPayload
public_update_open_error(
    const kairosboot::fleet::DevicePreflightOpenError &error,
    const std::string_view identifier) {
  using kairosboot::fleet::DevicePreflightOpenErrorCode;
  kb_status_t status = KB_E_IO;
  switch (error.code) {
  case DevicePreflightOpenErrorCode::Cancelled:
    status = KB_E_CANCELLED;
    break;
  case DevicePreflightOpenErrorCode::DeadlineExceeded:
    status = KB_E_TIMEOUT;
    break;
  case DevicePreflightOpenErrorCode::NotFound:
    status = KB_E_NO_DEVICE;
    break;
  case DevicePreflightOpenErrorCode::Busy:
    status = KB_E_BUSY;
    break;
  case DevicePreflightOpenErrorCode::DriverUnavailable:
    status = KB_E_NOT_SUPPORTED;
    break;
  case DevicePreflightOpenErrorCode::ResourceExhausted:
    status = KB_E_OUT_OF_MEMORY;
    break;
  case DevicePreflightOpenErrorCode::PermissionDenied:
  case DevicePreflightOpenErrorCode::TransportFailure:
  case DevicePreflightOpenErrorCode::UnexpectedFailure:
    break;
  }
  auto result = update_error(status, error.message, identifier,
                             public_update_transfer_state(
                                 error.outbound_certainty));
  result.native_code = error.native_code;
  return result;
}

[[nodiscard]] kairosboot::api::OperationErrorPayload
public_update_probe_error(
    const kairosboot::fleet::DevicePreflightProbeError &error,
    const std::string_view identifier) {
  using kairosboot::fleet::DevicePreflightProbeErrorCode;
  kb_status_t status = KB_E_PROTOCOL;
  switch (error.code) {
  case DevicePreflightProbeErrorCode::Cancelled:
    status = KB_E_CANCELLED;
    break;
  case DevicePreflightProbeErrorCode::DeadlineExceeded:
    status = KB_E_TIMEOUT;
    break;
  case DevicePreflightProbeErrorCode::ResourceExhausted:
    status = KB_E_OUT_OF_MEMORY;
    break;
  case DevicePreflightProbeErrorCode::ProtocolFailure:
  case DevicePreflightProbeErrorCode::DeviceRejected:
  case DevicePreflightProbeErrorCode::InvalidResponse:
  case DevicePreflightProbeErrorCode::UnexpectedFailure:
    break;
  }
  auto result = update_error(status, error.message, identifier,
                             public_update_transfer_state(
                                 error.outbound_certainty));
  result.native_code = error.native_code;
  return result;
}

[[nodiscard]] kairosboot::api::OperationErrorPayload
public_update_device_error(
    const kairosboot::fastboot::UpdateDeviceError &error,
    const std::string_view identifier) {
  using kairosboot::fastboot::UpdateDeviceErrorKind;
  kb_status_t status = KB_E_PROTOCOL;
  switch (error.kind) {
  case UpdateDeviceErrorKind::Cancelled:
    status = KB_E_CANCELLED;
    break;
  case UpdateDeviceErrorKind::TimedOut:
    status = KB_E_TIMEOUT;
    break;
  case UpdateDeviceErrorKind::Unsupported:
    status = KB_E_NOT_SUPPORTED;
    break;
  case UpdateDeviceErrorKind::Failed:
    break;
  }
  auto result = update_error(status, error.message, identifier,
                             public_update_transfer_state(
                                 error.outbound_certainty));
  result.native_code = error.native_code;
  result.device_message = error.device_message;
  result.session_poisoned = error.session_poisoned;
  return result;
}

struct PreparedPublicUpdateDevice final {
  std::unique_ptr<kairosboot::protocol::FastbootSession> direct_session;
  std::unique_ptr<kairosboot::fastboot::PrimitiveService> direct_service;
  std::unique_ptr<kairosboot::fastboot::LibusbReconnectAdapter>
      reconnect_adapter;
  std::unique_ptr<kairosboot::fastboot::SteadyReconnectWaiter>
      reconnect_waiter;
  std::unique_ptr<kairosboot::fastboot::ReconnectCoordinator>
      reconnect_coordinator;
  std::unique_ptr<kairosboot::fastboot::PrimitiveUpdateDevice> update_device;
  kairosboot::fastboot::PrimitiveService *service{};
};

[[nodiscard]] std::expected<PreparedPublicUpdateDevice,
                            kairosboot::api::OperationErrorPayload>
prepare_public_update_device(
    const PreparedTarget &target, const std::uint32_t timeout_ms,
    const UpdateClock::time_point deadline,
    const std::stop_token cancellation,
    kairosboot::fastboot::PrimitiveUpdateDeviceOptions device_options) {
  try {
    PreparedPublicUpdateDevice result;
    if (target.selector.kind == kairosboot::api::DeviceSelectorKind::Tcp ||
        target.selector.kind == kairosboot::api::DeviceSelectorKind::Udp) {
      kb_command_options_t transport_options{};
      kb_command_options_init(&transport_options);
      transport_options.timeout_ms = timeout_ms;
      auto transport = open_target(target, transport_options, cancellation,
                                   deadline);
      if (!transport) {
        return std::unexpected(std::move(transport.error()));
      }
      kairosboot::protocol::SessionOptions session_options{};
      session_options.io_timeout = std::chrono::milliseconds{timeout_ms};
      result.direct_session =
          std::make_unique<kairosboot::protocol::FastbootSession>(
              std::move(*transport), session_options);
      result.direct_service =
          std::make_unique<kairosboot::fastboot::PrimitiveService>(
              *result.direct_session);
      result.update_device =
          std::make_unique<kairosboot::fastboot::PrimitiveUpdateDevice>(
              *result.direct_service, std::move(device_options));
      result.service = result.direct_service.get();
      return result;
    }

    if (target.usb_runtime == nullptr || !target.usb_device) {
      return std::unexpected(update_error(
          KB_E_INTERNAL, "prepared USB update target is incomplete",
          target.selector.identifier));
    }
    const auto budget = kairosboot::transport::process_usb_buffer_budget();
    const kairosboot::transport::TransferRingConfig data_ring{};
    kairosboot::protocol::SessionOptions session_options{};
    session_options.io_timeout = std::chrono::milliseconds{timeout_ms};
    auto opener =
        kairosboot::fleet::make_libusb_device_preflight_session_opener(
            target.usb_runtime, budget, data_ring, session_options);
    if (!opener) {
      return std::unexpected(
          public_update_open_error(opener.error(), target.selector.identifier));
    }
    auto opened = (*opener)->open(*target.usb_device, deadline, cancellation);
    if (!opened) {
      return std::unexpected(
          public_update_open_error(opened.error(), target.selector.identifier));
    }
    kairosboot::fleet::FastbootDevicePreflightProbe probe;
    auto probed = probe.probe(*opened->session, deadline, cancellation);
    if (!probed) {
      return std::unexpected(
          public_update_probe_error(probed.error(), target.selector.identifier));
    }
    if (probed->mode == kairosboot::fastboot::FastbootUsbMode::Fastbootd) {
      result.direct_session = std::move(opened->session);
      result.direct_service =
          std::make_unique<kairosboot::fastboot::PrimitiveService>(
              *result.direct_session);
      result.update_device =
          std::make_unique<kairosboot::fastboot::PrimitiveUpdateDevice>(
              *result.direct_service, std::move(device_options));
      result.service = result.direct_service.get();
      return result;
    }

    auto initial = kairosboot::fastboot::bind_initial_libusb_update_session(
        std::move(*opened), *probed);
    if (!initial) {
      return std::unexpected(public_update_device_error(
          initial.error(), target.selector.identifier));
    }
    kairosboot::fastboot::LibusbReconnectAdapterOptions adapter_options{};
    adapter_options.transport.bulk_out.timeout_ms = timeout_ms;
    adapter_options.transport.data_ring = data_ring;
    adapter_options.transport.buffer_budget = budget;
    adapter_options.protocol = session_options;
    auto adapter = kairosboot::fastboot::LibusbReconnectAdapter::create(
        target.usb_runtime, std::move(adapter_options));
    if (!adapter) {
      return std::unexpected(update_error(
          adapter.error().code ==
                  kairosboot::fastboot::
                      LibusbReconnectAdapterFactoryErrorCode::ResourceExhausted
              ? KB_E_OUT_OF_MEMORY
              : KB_E_INTERNAL,
          adapter.error().message, target.selector.identifier));
    }
    result.reconnect_adapter = std::move(*adapter);
    result.reconnect_waiter =
        std::make_unique<kairosboot::fastboot::SteadyReconnectWaiter>();
    result.reconnect_coordinator =
        std::make_unique<kairosboot::fastboot::ReconnectCoordinator>(
            *result.reconnect_adapter, *result.reconnect_adapter,
            *result.reconnect_waiter);
    auto device =
        kairosboot::fastboot::PrimitiveUpdateDevice::create_with_reconnect(
            std::move(*initial), *result.reconnect_coordinator, {},
            std::move(device_options));
    if (!device) {
      return std::unexpected(public_update_device_error(
          device.error(), target.selector.identifier));
    }
    result.update_device = std::move(*device);
    result.service =
        &result.update_device->current_service_for_fleet_actor();
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(update_error(
        KB_E_OUT_OF_MEMORY, "unable to allocate the public update device actor",
        target.selector.identifier));
  } catch (...) {
    return std::unexpected(update_error(
        KB_E_INTERNAL, "unable to prepare the public update device actor",
        target.selector.identifier));
  }
}

[[nodiscard]] kairosboot::api::OperationOutcome execute_prepared_public_update(
    const std::shared_ptr<kb_context_usb_state> &usb_state,
    kairosboot::api::DeviceSelector selector,
    kairosboot::fastboot::PreparedUpdatePackage prepared,
    const UpdateClock::time_point deadline,
    const kb_update_options_t &options,
    const SlotPolicy &slot_policy,
    kairosboot::api::OperationState::TaskContext &task_context) {
  auto identifier = selector.identifier;
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
          deadline, identifier, "device selection");
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
    target = bind_target(std::move(selector), std::move(*runtime),
                         usb_state->usb_vendor_id);
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
      deadline, identifier, "transport open");
  if (!open_timeout) {
    return operation_failure(std::move(open_timeout.error()));
  }
  kairosboot::fastboot::PrimitiveUpdateDeviceOptions device_options{
      .host_resparse_limit = kairosboot::image::kDefaultResparseLimitBytes,
      .explicit_sparse_limit = options.sparse_limit_bytes,
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
  auto public_device = prepare_public_update_device(
      *target, *open_timeout, deadline, task_context.cancellation_token(),
      std::move(device_options));
  if (!public_device) {
    return operation_failure(std::move(public_device.error()));
  }
  auto &service = *public_device->service;

  const kairosboot::fastboot::UpdateOperationContext slot_context{
      .cancellation = task_context.cancellation_token(),
      .deadline = deadline == UpdateClock::time_point::max()
                      ? std::optional<UpdateClock::time_point>{}
                      : std::optional<UpdateClock::time_point>{deadline},
  };
  auto slotted = apply_update_slot_policy(
      prepared, service, slot_policy, slot_context, identifier);
  if (!slotted) {
    return operation_failure(std::move(slotted.error()));
  }
  auto optimized = try_optimize_public_super(
      prepared, service, options, slot_policy, identifier,
      task_context.cancellation_token());
  if (!optimized) {
    return operation_failure(std::move(optimized.error()));
  }
  auto activated = activate_update_slot_policy(
      service, slot_policy, slot_context, identifier);
  if (!activated) {
    return operation_failure(std::move(activated.error()));
  }
  const auto total_tasks = prepared.plan.tasks.size();

  bool callback_cancelled = false;
  kairosboot::fastboot::UpdateExecutorOptions executor_options{
      .known_partitions =
          kairosboot::fastboot::frozen_update_known_partitions(),
      .deadline = deadline == UpdateClock::time_point::max()
                      ? std::optional<UpdateClock::time_point>{}
                      : std::optional<UpdateClock::time_point>{deadline},
      .force = options.force != 0,
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
      prepared, *public_device->update_device, executor_options,
      task_context.cancellation_token());
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
          deadline, identifier, "completion");
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
          deadline, identifier, "completion callback");
      !completion) {
    auto payload = std::move(completion.error());
    payload.transfer_state = completed_update_transfer_state(
        executed->completed_tasks, total_tasks);
    return operation_failure(std::move(payload));
  }
  return kairosboot::api::OperationOutcome::succeeded();
}

kb_status_t start_flash_source_async(
    kb_device_t &device,
    const std::string_view partition,
    std::shared_ptr<const kairosboot::image::IImageSource> image_source,
    const kb_flash_options_t flash_options, SlotPolicy slot_policy,
    const bool aosp_raw_profile,
    kb_operation_t **operation,
    kb_error_t **error) {
  const auto selector_text = device.selector.c_str();
  auto operation_lease = try_acquire_device_operation(device);
  if (!operation_lease) {
    return fail(error, KB_E_BUSY,
                "device already has an active protocol operation",
                device.identifier.c_str());
  }
  auto prepared_target = prepare_target(device.context, selector_text);
  if (!prepared_target) {
    return fail(error, prepared_target.error());
  }

  auto selected_identifier = prepared_target->selector.identifier;
  std::string partition_copy{partition};
  auto task = [operation_lease = std::move(operation_lease),
               target = std::move(*prepared_target),
               image_source = std::move(image_source),
               partition_copy = std::move(partition_copy), flash_options,
               slot_policy = std::move(slot_policy),
               aosp_raw_profile,
               selected_identifier = std::move(selected_identifier)](
                  kairosboot::api::OperationState::TaskContext
                      &task_context) mutable
      -> kairosboot::api::OperationOutcome {
    static_cast<void>(operation_lease);
    if (task_context.cancel_requested()) {
      return cancelled_operation(selected_identifier, KB_TRANSFER_NOT_SENT);
    }

    const auto flags = flash_vbmeta_flags(flash_options);
    if (flags.any() &&
        kairosboot::image::is_vbmeta_partition(partition_copy)) {
      auto mutated = kairosboot::image::apply_vbmeta_flags(
          std::move(image_source), flags,
          task_context.cancellation_token());
      if (!mutated) {
        return operation_failure(
            vbmeta_error(mutated.error(), selected_identifier));
      }
      image_source = std::move(*mutated);
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
    kb_command_options_init_sized(&transport_options,
                                  sizeof(transport_options));
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

    kairosboot::fastboot::SlotPlanner slot_planner(service);
    const kairosboot::fastboot::UpdateOperationContext slot_context{
        .cancellation = task_context.cancellation_token(),
        .deadline = std::nullopt,
    };
    std::vector<std::string> requested_slots;
    if (slot_policy.slot) {
      if (*slot_policy.slot == "all") {
        auto topology = slot_planner.query_topology(slot_context);
        if (!topology) {
          return operation_failure(
              slot_error(topology.error(), selected_identifier));
        }
        requested_slots = std::move(topology->slots);
      } else {
        auto resolved = slot_planner.resolve_active_slot(
            *slot_policy.slot, slot_context);
        if (!resolved) {
          return operation_failure(
              slot_error(resolved.error(), selected_identifier));
        }
        requested_slots.push_back(std::move(*resolved));
      }
    }

    std::optional<std::string> active_after_flash;
    if (slot_policy.set_active) {
      const std::string_view requested_active = slot_policy.active_slot
          ? std::string_view{*slot_policy.active_slot}
          : slot_policy.slot ? std::string_view{*slot_policy.slot}
                             : std::string_view{};
      if (requested_active == "all") {
        auto topology = slot_planner.query_topology(slot_context);
        if (!topology) {
          return operation_failure(
              slot_error(topology.error(), selected_identifier));
        }
        active_after_flash = topology->slots.front();
      } else {
        auto resolved = slot_planner.resolve_active_slot(
            requested_active, slot_context);
        if (!resolved) {
          return operation_failure(
              slot_error(resolved.error(), selected_identifier));
        }
        active_after_flash = std::move(*resolved);
      }
    }

    std::optional<std::string> is_userspace;
    std::optional<std::string> has_slot;
    std::optional<std::string> is_logical;
    const auto query_optional =
        [&service, &selected_identifier](
            const std::string &variable,
            std::optional<std::string> *captured = nullptr)
        -> std::expected<void, kairosboot::api::OperationErrorPayload> {
      auto value = service.getvar(variable);
      if (!value && value.error().code !=
                        kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
        return std::unexpected(kairosboot::api::normalize_public_error(
            value.error(), selected_identifier));
      }
      if (value && captured != nullptr) {
        *captured = value->terminal.payload;
      }
      return {};
    };
    if (!aosp_raw_profile) {
      if (auto queried = query_optional("is-userspace", &is_userspace);
          !queried) {
        return operation_failure(std::move(queried.error()));
      }
    }
    if (auto queried = query_optional(
            "has-slot:" + partition_copy, &has_slot);
        !queried) {
      return operation_failure(std::move(queried.error()));
    }

    std::vector<std::string> partitions{partition_copy};
    if (!requested_slots.empty()) {
      if (has_slot != "yes") {
        return operation_failure(update_error(
            KB_E_NOT_SUPPORTED,
            "an explicit Fastboot slot was requested for a non-slotted partition",
            selected_identifier));
      }
      partitions.clear();
      partitions.reserve(requested_slots.size());
      const auto separator = partition_copy.find(':');
      const auto base = partition_copy.substr(0U, separator);
      const auto suffix = separator == std::string::npos
          ? std::string_view{}
          : std::string_view{partition_copy}.substr(separator);
      for (const auto &slot : requested_slots) {
        auto resolved = base + "_" + slot;
        resolved.append(suffix);
        partitions.push_back(std::move(resolved));
      }
    } else if (aosp_raw_profile && has_slot == "yes") {
      auto current_slot = service.getvar("current-slot");
      if (!current_slot) {
        return operation_failure(kairosboot::api::normalize_public_error(
            current_slot.error(), selected_identifier));
      }
      auto slot = current_slot->terminal.payload;
      if (!slot.empty() && slot.front() == '_') {
        slot.erase(slot.begin());
      }
      if (slot.empty()) {
        return operation_failure(update_error(
            KB_E_PROTOCOL,
            "device returned an empty current-slot for a slotted partition",
            selected_identifier));
      }
      partitions.front() += "_" + slot;
    }

    if (!aosp_raw_profile) {
      if (auto queried = query_optional(
              "is-logical:" + partitions.front(), &is_logical);
          !queried) {
        return operation_failure(std::move(queried.error()));
      }
      if (is_logical == "yes" && is_userspace != "yes" &&
          flash_options.force == 0) {
        return operation_failure(update_error(
            KB_E_NOT_SUPPORTED,
            "logical partition must be flashed through fastbootd; set force only "
            "when intentionally overwriting a fixed bootloader partition",
            selected_identifier));
      }
    }

    if (flags.any() &&
        kairosboot::image::is_vbmeta_partition(partition_copy)) {
      if (auto queried = query_optional(
              "is-logical:" + partitions.front());
          !queried) {
        return operation_failure(std::move(queried.error()));
      }
      if (auto queried = query_optional(
              "partition-size:" + partitions.front());
          !queried) {
        return operation_failure(std::move(queried.error()));
      }
    }

    std::uint64_t target_max_download_size = 0;
    if (!aosp_raw_profile) {
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
    }

    const auto effective_max_download_size =
        kairosboot::image::effective_sparse_download_limit(
            target_max_download_size, flash_options.sparse_limit_bytes);
    auto plan = kairosboot::image::SparseFlashPlan::create(
        *artifact, effective_max_download_size,
        kairosboot::image::kDefaultResparseLimitBytes,
        task_context.cancellation_token());
    if (!plan) {
      return operation_failure(kairosboot::api::normalize_public_error(
          plan.error(), selected_identifier));
    }

    std::vector<std::shared_ptr<kairosboot::protocol::ITransferSource>>
        transfer_sources;
    transfer_sources.reserve(plan->parts().size());
    for (const auto &part : plan->parts()) {
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

    if (partitions.size() > 1U &&
        plan->transfer_size() >
            std::numeric_limits<std::uint64_t>::max() / partitions.size()) {
      return operation_failure(update_error(
          KB_E_INVALID_ARGUMENT,
          "slot-expanded flash transfer size overflows uint64",
          selected_identifier));
    }
    const auto total_transfer_size =
        plan->transfer_size() * partitions.size();
    if (task_context.cancel_requested() ||
        !report_progress(flash_options, 0, total_transfer_size, "download",
                         selected_identifier)) {
      return cancelled_operation(selected_identifier, KB_TRANSFER_NOT_SENT);
    }

    std::uint64_t completed_before_source = 0;
    for (const auto &resolved_partition : partitions) {
      for (const auto &source : transfer_sources) {
        const kairosboot::protocol::TransferProgressObserver observer =
            [&task_context, &flash_options, &selected_identifier,
             completed_before_source,
             total_transfer_size](const std::uint64_t completed,
                                  const std::uint64_t) {
              if (task_context.cancel_requested() ||
                  !report_progress(
                      flash_options, completed_before_source + completed,
                      total_transfer_size, "download", selected_identifier)) {
                return kairosboot::protocol::TransferProgressAction::cancel;
              }
              return kairosboot::protocol::TransferProgressAction::
                  continue_transfer;
            };

        auto flashed = service.download_and_flash_source(
            resolved_partition, source, observer);
        if (!flashed) {
          auto payload = kairosboot::api::normalize_public_error(
              flashed.error(), selected_identifier);
          kairosboot::api::accumulate_flash_transfer_state(
              payload, flashed.error().operation, completed_before_source,
              source->size(), total_transfer_size);
          return operation_failure(std::move(payload));
        }
        completed_before_source += source->size();
      }
    }

    if (task_context.cancel_requested()) {
      return cancelled_operation(
          selected_identifier, KB_TRANSFER_FULLY_TRANSFERRED,
          "operation cancelled after the flash completed");
    }
    if (active_after_flash) {
      auto activated = service.set_active(*active_after_flash);
      if (!activated) {
        return operation_failure(kairosboot::api::normalize_public_error(
            activated.error(), selected_identifier));
      }
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
    return fail(error, KB_E_INTERNAL, "unable to start the flash operation",
                selector_text);
  }
  *operation = result.release();
  return KB_OK;
}

kb_status_t start_vendor_boot_ramdisk_async(
    kb_device_t &device,
    const std::string_view partition, const std::string_view ramdisk_name,
    std::shared_ptr<const kairosboot::image::IImageSource> new_ramdisk,
    std::shared_ptr<const kairosboot::image::IImageSource> new_dtb,
    const kb_flash_options_t flash_options, SlotPolicy slot_policy,
    kb_operation_t **operation, kb_error_t **error) {
  const auto selector_text = device.selector.c_str();
  auto operation_lease = try_acquire_device_operation(device);
  if (!operation_lease) {
    return fail(error, KB_E_BUSY,
                "device already has an active protocol operation",
                device.identifier.c_str());
  }
  auto prepared_target = prepare_target(device.context, selector_text);
  if (!prepared_target) {
    return fail(error, prepared_target.error());
  }

  std::string partition_copy{partition};
  std::string ramdisk_name_copy{ramdisk_name};
  auto selected_identifier = prepared_target->selector.identifier;
  auto task = [operation_lease = std::move(operation_lease),
               target = std::move(*prepared_target),
               partition_copy = std::move(partition_copy),
               ramdisk_name_copy = std::move(ramdisk_name_copy),
               new_ramdisk = std::move(new_ramdisk),
               new_dtb = std::move(new_dtb), flash_options,
               slot_policy = std::move(slot_policy),
               selected_identifier = std::move(selected_identifier)](
                  kairosboot::api::OperationState::TaskContext &task_context)
      mutable -> kairosboot::api::OperationOutcome {
    static_cast<void>(operation_lease);
    if (task_context.cancel_requested()) {
      return cancelled_operation(selected_identifier, KB_TRANSFER_NOT_SENT);
    }

    kb_command_options_t transport_options;
    kb_command_options_init_sized(&transport_options,
                                  sizeof(transport_options));
    transport_options.timeout_ms = flash_options.timeout_ms;
    auto opened = open_target(target, transport_options,
                              task_context.cancellation_token());
    if (!opened) {
      return operation_failure(std::move(opened.error()));
    }
    kairosboot::protocol::SessionOptions session_options;
    session_options.io_timeout =
        std::chrono::milliseconds{flash_options.timeout_ms};
    kairosboot::protocol::FastbootSession session(std::move(*opened),
                                                   session_options);
    kairosboot::fastboot::PrimitiveService service(session);
    auto cancellation = task_context.register_cancellation_hook(
        [&service] { service.request_cancel(); });

    std::optional<std::string> is_userspace;
    std::optional<std::string> is_logical;
    const std::array<std::pair<std::string, std::optional<std::string> *>, 3>
        aosp_flash_preflight{{
            {"is-userspace", &is_userspace},
            {"has-slot:" + partition_copy, nullptr},
            {"is-logical:" + partition_copy, &is_logical},
        }};
    for (const auto &[variable, captured] : aosp_flash_preflight) {
      auto value = service.getvar(variable);
      if (!value && value.error().code !=
                        kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
        return operation_failure(kairosboot::api::normalize_public_error(
            value.error(), selected_identifier));
      }
      if (value && captured != nullptr) {
        *captured = value->terminal.payload;
      }
    }
    if (is_logical == "yes" && is_userspace != "yes" &&
        flash_options.force == 0) {
      return operation_failure(update_error(
          KB_E_NOT_SUPPORTED,
          "logical partition must be flashed through fastbootd; set force only "
          "when intentionally overwriting a fixed bootloader partition",
          selected_identifier));
    }

    std::vector<std::string> partitions{partition_copy};
    kairosboot::fastboot::SlotPlanner slot_planner(service);
    const kairosboot::fastboot::UpdateOperationContext slot_context{
        .cancellation = task_context.cancellation_token(),
        .deadline = std::nullopt,
    };
    if (slot_policy.slot) {
      auto resolved = plan_partition_slots(
          slot_planner, partition_copy, *slot_policy.slot, slot_context);
      if (!resolved) {
        return operation_failure(
            slot_error(resolved.error(), selected_identifier));
      }
      partitions = std::move(resolved->partition_names);
    }

    if (slot_policy.set_active) {
      const std::string_view requested_active = slot_policy.active_slot
          ? std::string_view{*slot_policy.active_slot}
          : slot_policy.slot ? std::string_view{*slot_policy.slot}
                             : std::string_view{};
      std::expected<std::string, kairosboot::fastboot::SlotError> active =
          std::unexpected(kairosboot::fastboot::SlotError{});
      if (requested_active == "all") {
        auto topology = slot_planner.query_topology(slot_context);
        if (!topology) {
          return operation_failure(
              slot_error(topology.error(), selected_identifier));
        }
        active = topology->slots.front();
      } else {
        active = slot_planner.resolve_active_slot(requested_active,
                                                  slot_context);
      }
      if (!active) {
        return operation_failure(slot_error(active.error(),
                                            selected_identifier));
      }
      auto activated = service.set_active(*active);
      if (!activated) {
        return operation_failure(kairosboot::api::normalize_public_error(
            activated.error(), selected_identifier));
      }
    }

    auto max_fetch_reply = service.getvar("max-fetch-size");
    if (!max_fetch_reply) {
      return operation_failure(kairosboot::api::normalize_public_error(
          max_fetch_reply.error(), selected_identifier));
    }
    auto max_fetch = kairosboot::fastboot::parse_unsigned_variable(
        max_fetch_reply->terminal.payload);
    if (!max_fetch.has_value() || *max_fetch == 0U) {
      return operation_failure(local_flash_error(
          KB_E_PROTOCOL,
          "Fastboot max-fetch-size must be a positive integer",
          selected_identifier));
    }
    *max_fetch = std::min<std::uint64_t>(
        *max_fetch, std::numeric_limits<std::uint32_t>::max());

    std::size_t flashed_partitions = 0U;
    for (const auto &resolved_partition : partitions) {
    auto partition_size_reply =
        service.getvar("partition-size:" + resolved_partition);
    if (!partition_size_reply) {
      return operation_failure(kairosboot::api::normalize_public_error(
          partition_size_reply.error(), selected_identifier));
    }
    auto partition_size = kairosboot::fastboot::parse_unsigned_variable(
        partition_size_reply->terminal.payload);
    if (!partition_size.has_value() || *partition_size == 0U ||
        *partition_size > std::numeric_limits<std::uint32_t>::max()) {
      return operation_failure(local_flash_error(
          KB_E_PROTOCOL,
          "Fastboot vendor_boot partition-size must be in 1..UINT32_MAX",
          selected_identifier));
    }

    auto temporary_path = vendor_boot_temporary_path(selected_identifier);
    if (!temporary_path) {
      return operation_failure(std::move(temporary_path.error()));
    }
    ScopedTemporaryFile temporary_file(std::move(*temporary_path));
    auto sink = kairosboot::protocol::FileTransferSink::create(
        temporary_file.path());
    if (!sink) {
      return operation_failure(
          file_sink_error(sink.error(), selected_identifier));
    }
    if (!report_progress(flash_options, 0U, *partition_size, "fetch",
                         selected_identifier)) {
      return cancelled_operation(selected_identifier, KB_TRANSFER_NOT_SENT);
    }

    std::uint64_t fetched = 0U;
    while (fetched < *partition_size) {
      if (task_context.cancel_requested()) {
        (*sink)->discard();
        return cancelled_operation(selected_identifier,
                                   KB_TRANSFER_NOT_SENT);
      }
      const auto chunk_size =
          std::min(*max_fetch, *partition_size - fetched);
      auto offset_sink =
          std::make_shared<OffsetTransferSink>(*sink, fetched);
      const kairosboot::protocol::TransferProgressObserver observer =
          [&task_context, &flash_options, &selected_identifier, fetched,
           partition_size](const std::uint64_t completed,
                           const std::uint64_t) {
            if (task_context.cancel_requested() ||
                !report_progress(flash_options, fetched + completed,
                                 *partition_size, "fetch",
                                 selected_identifier)) {
              return kairosboot::protocol::TransferProgressAction::cancel;
            }
            return kairosboot::protocol::TransferProgressAction::
                continue_transfer;
          };
      auto received = service.fetch_to_sink(
          resolved_partition,
          kairosboot::fastboot::FetchRange{.offset = fetched,
                                           .size = chunk_size},
          std::move(offset_sink), chunk_size, observer);
      if (!received) {
        (*sink)->discard();
        return operation_failure(kairosboot::api::normalize_public_error(
            received.error(), selected_identifier));
      }
      if (received->terminal.kind !=
              kairosboot::protocol::ResponseKind::Okay ||
          !received->inbound_expected.has_value() ||
          *received->inbound_expected != chunk_size ||
          received->inbound_transferred != chunk_size ||
          received->inbound_certainty !=
              kairosboot::protocol::TransferCertainty::FullyTransferred) {
        (*sink)->discard();
        auto payload = local_flash_error(
            KB_E_PROTOCOL,
            "Fastboot fetch did not return the exact requested vendor_boot chunk",
            selected_identifier);
        payload.inbound_expected = chunk_size;
        payload.inbound_transferred = received->inbound_transferred;
        payload.inbound_transfer_state = KB_TRANSFER_PARTIAL_OR_UNKNOWN;
        return operation_failure(std::move(payload));
      }
      fetched += chunk_size;
    }
    if (auto sealed = (*sink)->seal(*partition_size); !sealed) {
      (*sink)->discard();
      return operation_failure(
          file_sink_error(sealed.error(), selected_identifier));
    }

    auto fetched_source = kairosboot::image::FileImageSource::open(
        temporary_file.path());
    if (!fetched_source) {
      return operation_failure(kairosboot::api::normalize_public_error(
          fetched_source.error(), selected_identifier));
    }
    if (task_context.cancel_requested() ||
        !report_progress(flash_options, *partition_size, *partition_size,
                         "repack", selected_identifier)) {
      return cancelled_operation(selected_identifier, KB_TRANSFER_NOT_SENT);
    }
    auto repacked = kairosboot::image::repack_vendor_boot(
        std::move(*fetched_source), new_ramdisk, new_dtb,
        kairosboot::image::VendorBootRepackOptions{
            .ramdisk_name = ramdisk_name_copy,
            .maximum_image_bytes =
                std::numeric_limits<std::uint32_t>::max(),
        },
        task_context.cancellation_token());
    if (!repacked) {
      return operation_failure(
          vendor_boot_repack_error(repacked.error(), selected_identifier));
    }

    std::uint64_t target_max_download_size = 0U;
    auto maximum_reply = service.getvar("max-download-size");
    if (!maximum_reply && maximum_reply.error().code !=
                              kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
      return operation_failure(kairosboot::api::normalize_public_error(
          maximum_reply.error(), selected_identifier));
    }
    if (maximum_reply) {
      target_max_download_size =
          kairosboot::fastboot::parse_unsigned_variable(
              maximum_reply->terminal.payload)
              .value_or(0U);
    }

    std::shared_ptr<const kairosboot::image::IImageSource> repacked_source =
        *repacked;
    auto artifact = kairosboot::image::FlashArtifact::inspect(
        repacked_source, task_context.cancellation_token());
    if (!artifact) {
      return operation_failure(kairosboot::api::normalize_public_error(
          artifact.error(), selected_identifier));
    }
    const auto effective_max_download_size =
        kairosboot::image::effective_sparse_download_limit(
            target_max_download_size, flash_options.sparse_limit_bytes);
    auto plan = kairosboot::image::SparseFlashPlan::create(
        *artifact, effective_max_download_size,
        kairosboot::image::kDefaultResparseLimitBytes,
        task_context.cancellation_token());
    if (!plan) {
      return operation_failure(kairosboot::api::normalize_public_error(
          plan.error(), selected_identifier));
    }

    const auto transfer_size = plan->transfer_size();
    if (task_context.cancel_requested() ||
        !report_progress(flash_options, 0U, transfer_size, "download",
                         selected_identifier)) {
      return cancelled_operation(selected_identifier, KB_TRANSFER_NOT_SENT);
    }
    std::uint64_t completed_before_source = 0U;
    for (const auto &part : plan->parts()) {
      auto transfer_source =
          kairosboot::transport::ImageTransferSource::create(part.source);
      if (!transfer_source) {
        return operation_failure(kairosboot::api::normalize_public_error(
            transfer_source.error(), selected_identifier));
      }
      const auto source_size = (*transfer_source)->size();
      const kairosboot::protocol::TransferProgressObserver download_observer =
          [&task_context, &flash_options, &selected_identifier,
           completed_before_source,
           transfer_size](const std::uint64_t completed,
                          const std::uint64_t) {
            if (task_context.cancel_requested() ||
                !report_progress(flash_options,
                                 completed_before_source + completed,
                                 transfer_size, "download",
                                 selected_identifier)) {
              return kairosboot::protocol::TransferProgressAction::cancel;
            }
            return kairosboot::protocol::TransferProgressAction::
                continue_transfer;
          };
      auto flashed = service.download_and_flash_source(
          resolved_partition, std::move(*transfer_source), download_observer);
      if (!flashed) {
        auto payload = kairosboot::api::normalize_public_error(
            flashed.error(), selected_identifier);
        kairosboot::api::accumulate_flash_transfer_state(
            payload, flashed.error().operation, completed_before_source,
            source_size, transfer_size);
        return operation_failure(std::move(payload));
      }
      completed_before_source += source_size;
    }
    ++flashed_partitions;
    }
    if (task_context.cancel_requested() ||
        !report_progress(flash_options, flashed_partitions, partitions.size(),
                         "complete", selected_identifier)) {
      return cancelled_operation(
          selected_identifier, KB_TRANSFER_FULLY_TRANSFERRED,
          "operation cancelled after the vendor_boot flash completed");
    }
    return kairosboot::api::OperationOutcome::succeeded();
  };

  try {
    auto result = std::make_unique<kb_operation>(std::move(task));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL,
                  "unable to start the vendor_boot repack operation",
                  selector_text);
    }
    *operation = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the vendor_boot repack operation",
                selector_text);
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the vendor_boot repack operation",
                selector_text);
  }
}

[[nodiscard]] kairosboot::api::OperationOutcome run_prepared_public_update(
    const std::shared_ptr<kb_context_usb_state> &usb_state,
    kairosboot::api::DeviceSelector selector,
    const std::filesystem::path &package_path,
    const kb_update_options_t &options,
    const SlotPolicy &slot_policy,
    kairosboot::api::OperationState::TaskContext &task_context) {
  const auto identifier = selector.identifier;
  auto deadline =
      update_deadline(UpdateClock::now(), options.timeout_ms, identifier);
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
      kairosboot::fastboot::UpdatePackagePolicy{
          .append_final_reboot = options.skip_reboot == 0,
          .skip_secondary = options.skip_secondary != 0,
          .exclude_dynamic_partitions =
              options.exclude_dynamic_partitions != 0,
          .disable_fastboot_info = options.disable_fastboot_info != 0,
      },
      kairosboot::fastboot::UpdatePackagePreflightLimits{}, *deadline,
      task_context.cancellation_token());
  if (!prepared) {
    return operation_failure(kairosboot::api::normalize_public_error(
        prepared.error(), identifier));
  }
  if (auto transformed = prepare_update_vbmeta_flags(
          *prepared, update_vbmeta_flags(options), identifier,
          task_context.cancellation_token());
      !transformed) {
    return operation_failure(std::move(transformed.error()));
  }
  return execute_prepared_public_update(
      usb_state, std::move(selector), std::move(*prepared), *deadline, options,
      slot_policy, task_context);
}

[[nodiscard]] kairosboot::api::OperationOutcome run_public_wipe_super(
    const std::shared_ptr<kb_context_usb_state> &usb_state,
    kairosboot::api::DeviceSelector selector,
    const std::filesystem::path &super_empty_image,
    const kb_update_options_t &options,
    kairosboot::api::OperationState::TaskContext &task_context) {
  const auto identifier = selector.identifier;
  auto deadline =
      update_deadline(UpdateClock::now(), options.timeout_ms, identifier);
  if (!deadline) {
    return operation_failure(std::move(deadline.error()));
  }
  if (task_context.cancel_requested() ||
      !report_update_progress(options, 0U, 0U, "preflight", identifier)) {
    return cancelled_operation(identifier, KB_TRANSFER_NOT_SENT,
                               "wipe-super cancelled before image preflight");
  }

  kairosboot::image::ArtifactSourceResolver resolver;
  auto prepared = kairosboot::fastboot::preflight_wipe_super(
      resolver, super_empty_image, task_context.cancellation_token());
  if (!prepared) {
    return operation_failure(kairosboot::api::normalize_public_error(
        prepared.error(), identifier));
  }
  return execute_prepared_public_update(
      usb_state, std::move(selector), std::move(*prepared), *deadline, options,
      SlotPolicy{}, task_context);
}

kb_status_t start_boot_source_async(
    kb_device_t &device,
    std::shared_ptr<const kairosboot::image::IImageSource> image_source,
    const kb_flash_options_t boot_options, kb_operation_t **operation,
    kb_error_t **error) {
  const auto selector_text = device.selector.c_str();
  if (auto valid_size = kairosboot::fastboot::validate_download_size(
          image_source->size());
      !valid_size) {
    return fail(error, kairosboot::api::normalize_public_error(
                           valid_size.error(), device.identifier));
  }
  auto operation_lease = try_acquire_device_operation(device);
  if (!operation_lease) {
    return fail(error, KB_E_BUSY,
                "device already has an active protocol operation",
                device.identifier.c_str());
  }
  auto prepared_target = prepare_target(device.context, selector_text);
  if (!prepared_target) {
    return fail(error, prepared_target.error());
  }

  auto selected_identifier = prepared_target->selector.identifier;
  auto task = [operation_lease = std::move(operation_lease),
               target = std::move(*prepared_target),
               image_source = std::move(image_source), boot_options,
               selected_identifier = std::move(selected_identifier)](
                  kairosboot::api::OperationState::TaskContext
                      &task_context) mutable
      -> kairosboot::api::OperationOutcome {
    static_cast<void>(operation_lease);
    if (task_context.cancel_requested()) {
      return cancelled_operation(selected_identifier, KB_TRANSFER_NOT_SENT);
    }

    auto source =
        kairosboot::transport::ImageTransferSource::create(image_source);
    if (!source) {
      return operation_failure(kairosboot::api::normalize_public_error(
          source.error(), selected_identifier));
    }

    kb_command_options_t transport_options;
    kb_command_options_init_sized(&transport_options,
                                  sizeof(transport_options));
    transport_options.timeout_ms = boot_options.timeout_ms;
    auto opened = open_target(target, transport_options,
                              task_context.cancellation_token());
    if (!opened) {
      return operation_failure(std::move(opened.error()));
    }

    std::unique_ptr<kairosboot::protocol::ITransportSession>
        protocol_transport = std::move(*opened);
    kairosboot::protocol::SessionOptions session_options;
    session_options.io_timeout =
        std::chrono::milliseconds{boot_options.timeout_ms};
    kairosboot::protocol::FastbootSession session(
        std::move(protocol_transport), session_options);
    kairosboot::fastboot::PrimitiveService service(session);
    auto cancellation = task_context.register_cancellation_hook(
        [&service] { service.request_cancel(); });

    const auto total_size = (*source)->size();
    if (task_context.cancel_requested() ||
        !report_progress(boot_options, 0U, total_size, "download",
                         selected_identifier)) {
      return cancelled_operation(selected_identifier, KB_TRANSFER_NOT_SENT);
    }
    const kairosboot::protocol::TransferProgressObserver observer =
        [&task_context, &boot_options,
         &selected_identifier](const std::uint64_t completed,
                               const std::uint64_t total) {
          if (task_context.cancel_requested() ||
              !report_progress(boot_options, completed, total, "download",
                               selected_identifier)) {
            return kairosboot::protocol::TransferProgressAction::cancel;
          }
          return kairosboot::protocol::TransferProgressAction::
              continue_transfer;
        };
    auto booted = service.download_and_boot_source(*source, observer);
    if (!booted) {
      return operation_failure(kairosboot::api::normalize_public_error(
          booted.error(), selected_identifier));
    }
    if (task_context.cancel_requested() ||
        !report_progress(boot_options, total_size, total_size, "complete",
                         selected_identifier)) {
      return cancelled_operation(
          selected_identifier, KB_TRANSFER_FULLY_TRANSFERRED,
          "operation cancelled after the boot command completed");
    }
    return kairosboot::api::OperationOutcome::succeeded();
  };

  auto result = std::make_unique<kb_operation>(std::move(task));
  if (!result->state->start()) {
    return fail(error, KB_E_INTERNAL, "unable to start the boot operation",
                selector_text);
  }
  *operation = result.release();
  return KB_OK;
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

extern "C" {

void KB_CALL kb_context_options_init(kb_context_options_t *options) {
  kb_context_options_init_sized(options, KB_CONTEXT_OPTIONS_V1_SIZE);
}

void KB_CALL kb_context_options_init_sized(kb_context_options_t *options,
                                           const uint32_t struct_size) {
  kairosboot::api::detail::initialize_struct_header(options, struct_size);
}

void KB_CALL kb_flash_options_init(kb_flash_options_t *options) {
  kb_flash_options_init_sized(options, KB_FLASH_OPTIONS_V1_SIZE);
}

void KB_CALL kb_flash_options_init_sized(kb_flash_options_t *options,
                                         const uint32_t struct_size) {
  kairosboot::api::detail::initialize_struct_header(options, struct_size);
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_flash_options_t, timeout_ms),
      kDefaultTimeoutMs);
}

void KB_CALL kb_legacy_boot_options_init(
    kb_legacy_boot_options_t *options) {
  kb_legacy_boot_options_init_sized(options, KB_LEGACY_BOOT_OPTIONS_V1_SIZE);
}

void KB_CALL kb_legacy_boot_options_init_sized(
    kb_legacy_boot_options_t *options, const uint32_t struct_size) {
  kairosboot::api::detail::initialize_struct_header(options, struct_size);
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_legacy_boot_options_t, base),
      uint32_t{0x10000000U});
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_legacy_boot_options_t, page_size),
      uint32_t{2048U});
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_legacy_boot_options_t, kernel_offset),
      uint32_t{0x00008000U});
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_legacy_boot_options_t, ramdisk_offset),
      uint32_t{0x01000000U});
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_legacy_boot_options_t, second_offset),
      uint32_t{0x00f00000U});
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_legacy_boot_options_t, tags_offset),
      uint32_t{0x00000100U});
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_legacy_boot_options_t, dtb_offset),
      uint64_t{0x01100000ULL});
}

void KB_CALL kb_update_options_init(kb_update_options_t *options) {
  kb_update_options_init_sized(options, KB_UPDATE_OPTIONS_V1_SIZE);
}

void KB_CALL kb_update_options_init_sized(kb_update_options_t *options,
                                          const uint32_t struct_size) {
  kairosboot::api::detail::initialize_struct_header(options, struct_size);
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_update_options_t, timeout_ms),
      kDefaultTimeoutMs);
}

void KB_CALL kb_command_options_init(kb_command_options_t *options) {
  kb_command_options_init_sized(options, KB_COMMAND_OPTIONS_V1_SIZE);
}

void KB_CALL kb_command_options_init_sized(kb_command_options_t *options,
                                           const uint32_t struct_size) {
  kairosboot::api::detail::initialize_struct_header(options, struct_size);
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_command_options_t, timeout_ms),
      kDefaultTimeoutMs);
  kairosboot::api::detail::initialize_field(
      options, struct_size,
      offsetof(kb_command_options_t, maximum_receive_bytes),
      kDefaultMaximumReceiveBytes);
}

void KB_CALL kb_version_init(kb_version_t *version) {
  kb_version_init_sized(version, KB_VERSION_V1_SIZE);
}

void KB_CALL kb_version_init_sized(kb_version_t *version,
                                   const uint32_t struct_size) {
  kairosboot::api::detail::initialize_struct_header(version, struct_size);
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
    kb_context_options_init_sized(&result->options, sizeof(result->options));
    result->usb_state = std::make_shared<kb_context_usb_state>();
    if (options != nullptr) {
      result->options.log_callback = options->log_callback;
      result->options.log_user_data = options->log_user_data;
      if (options->struct_size >= KB_CONTEXT_OPTIONS_VENDOR_ID_SIZE) {
        result->options.usb_vendor_id = options->usb_vendor_id;
        result->usb_state->usb_vendor_id =
            static_cast<std::uint16_t>(options->usb_vendor_id);
      }
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

kb_status_t KB_CALL kb_device_open(kb_context_t *context,
                                   const char *selector_or_null,
                                   kb_device_t **device,
                                   kb_error_t **error) {
  clear_error(error);
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device output pointer must not be null");
  }
  *device = nullptr;
  if (context == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "context must not be null");
  }

  try {
    auto target = prepare_target(*context, selector_or_null);
    if (!target) {
      return fail(error, target.error());
    }
    auto result = std::make_unique<kb_device>();
    result->context.options = context->options;
    result->context.usb_state = context->usb_state;
    result->operation_state = std::make_shared<kb_device_operation_state>();
    result->identifier = target->selector.identifier;
    if (target->usb_device.has_value()) {
      result->usb_path = physical_usb_path(*target->usb_device);
      if (result->usb_path.empty()) {
        return fail(error, KB_E_NOT_SUPPORTED,
                    "USB device has no stable physical port path",
                    result->identifier.c_str());
      }
      result->selector = result->usb_path;
      result->serial = target->usb_device->serial_utf8;
    } else {
      result->selector = target->selector.identifier;
    }
    *device = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate a device handle", selector_or_null);
  } catch (...) {
    return fail(error, KB_E_INTERNAL, "unable to open a device handle",
                selector_or_null);
  }
}

const char *KB_CALL kb_device_identifier(const kb_device_t *device) {
  return device == nullptr ? nullptr : device->identifier.c_str();
}

const char *KB_CALL kb_device_serial(const kb_device_t *device) {
  return device == nullptr ? nullptr : device->serial.c_str();
}

const char *KB_CALL kb_device_usb_path(const kb_device_t *device) {
  return device == nullptr ? nullptr : device->usb_path.c_str();
}

kb_status_t KB_CALL kb_device_retain(kb_device_t *device) {
  if (device == nullptr) {
    return KB_E_INVALID_ARGUMENT;
  }
  device->reference_count.fetch_add(1U, std::memory_order_relaxed);
  return KB_OK;
}

void KB_CALL kb_device_release(kb_device_t *device) {
  if (device != nullptr &&
      device->reference_count.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
    delete device;
  }
}

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

  auto enumerated = (*runtime)->enumerate(
      fastboot_usb_filter(context->usb_state->usb_vendor_id));
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
    kb_device_t *device, const char *partition, const char *file_path,
    const kb_flash_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (partition == nullptr || partition[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "partition must not be empty", device_error_identifier(device));
  }
  if (file_path == nullptr || file_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "file path must not be empty", device_error_identifier(device));
  }
  if (!valid_flash_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flash options have an incompatible size or API version",
                device_error_identifier(device));
  }

  try {
    const std::string_view partition_view{partition};
    if (!valid_fastboot_parameter(partition_view,
                                  std::string_view{"flash:"}.size())) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "partition must be printable ASCII and fit the Fastboot "
                  "command limit",
                  device_error_identifier(device));
    }

    const std::string_view file_path_view{file_path};
    if (!valid_utf8(file_path_view)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "file path must be valid UTF-8",
                  device_error_identifier(device));
    }

    auto file_source =
        kairosboot::image::FileImageSource::open(utf8_path(file_path_view));
    if (!file_source) {
      return fail(error, kairosboot::api::normalize_public_error(
                             file_source.error(), device->identifier));
    }

    const auto flash_options = flash_options_or_default(options_or_null);
    auto slot_policy = copy_flash_slot_policy(options_or_null);
    if (!slot_policy) {
      return fail(error, KB_E_INVALID_ARGUMENT, slot_policy.error().c_str(),
                  device_error_identifier(device));
    }

    std::shared_ptr<const kairosboot::image::IImageSource> image_source =
        std::move(*file_source);
    return start_flash_source_async(
        *device, partition_view, std::move(image_source),
        flash_options, std::move(*slot_policy), false, operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the flash operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the flash operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_flash_file(
    kb_device_t *device, const char *partition, const char *file_path,
    const kb_flash_options_t *options_or_null,
    kb_error_t **error) {
  kb_operation_t *operation = nullptr;
  const kb_status_t start = kb_flash_file_async(
      device, partition, file_path, options_or_null, &operation, error);
  if (start != KB_OK) {
    return start;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_flash_vendor_boot_ramdisk_async(
    kb_device_t *device, const char *partition,
    const char *ramdisk_name_or_null,
    const char *ramdisk_path, const char *dtb_path_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (partition == nullptr || partition[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "vendor_boot partition must not be empty",
                device_error_identifier(device));
  }
  const std::string_view partition_view{partition};
  if (partition_view != "vendor_boot" && partition_view != "vendor_boot_a" &&
      partition_view != "vendor_boot_b") {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "vendor_boot partition must be vendor_boot, vendor_boot_a, or vendor_boot_b",
                device_error_identifier(device));
  }
  if (ramdisk_path == nullptr || ramdisk_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "vendor ramdisk path must not be empty",
                device_error_identifier(device));
  }
  if (dtb_path_or_null != nullptr && dtb_path_or_null[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "replacement DTB path must not be empty",
                device_error_identifier(device));
  }
  if (!valid_flash_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flash options have an incompatible size or API version",
                device_error_identifier(device));
  }

  try {
    const std::string_view ramdisk_name =
        ramdisk_name_or_null == nullptr
            ? std::string_view{"default"}
            : std::string_view{ramdisk_name_or_null};
    if (ramdisk_name.empty() || ramdisk_name.size() > 32U ||
        !valid_fastboot_parameter(ramdisk_name, 0U) ||
        ramdisk_name.find(':') != std::string_view::npos) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "vendor ramdisk name must be 1..32 printable ASCII bytes without ':'",
                  device_error_identifier(device));
    }
    const std::string_view ramdisk_path_view{ramdisk_path};
    if (!valid_utf8(ramdisk_path_view)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "vendor ramdisk path must be valid UTF-8",
                  device_error_identifier(device));
    }
    if (dtb_path_or_null != nullptr &&
        !valid_utf8(std::string_view{dtb_path_or_null})) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "replacement DTB path must be valid UTF-8",
                  device_error_identifier(device));
    }
    const std::string &identifier = device->identifier;
    auto ramdisk = open_raw_boot_part(
        ramdisk_path_view, "vendor ramdisk", true, identifier);
    if (!ramdisk) {
      return fail(error, ramdisk.error());
    }
    std::shared_ptr<const kairosboot::image::IImageSource> dtb;
    if (dtb_path_or_null != nullptr) {
      auto loaded = open_raw_boot_part(
          std::string_view{dtb_path_or_null}, "replacement DTB", true,
          identifier);
      if (!loaded) {
        return fail(error, loaded.error());
      }
      dtb = std::move(*loaded);
    }

    const auto flash_options = flash_options_or_default(options_or_null);
    auto slot_policy = copy_flash_slot_policy(options_or_null);
    if (!slot_policy) {
      return fail(error, KB_E_INVALID_ARGUMENT, slot_policy.error().c_str(),
                  device_error_identifier(device));
    }
    return start_vendor_boot_ramdisk_async(
        *device, partition_view, ramdisk_name,
        std::move(*ramdisk), std::move(dtb), flash_options,
        std::move(*slot_policy), operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the vendor_boot repack operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the vendor_boot repack operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_flash_vendor_boot_ramdisk(
    kb_device_t *device, const char *partition,
    const char *ramdisk_name_or_null,
    const char *ramdisk_path, const char *dtb_path_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error) {
  kb_operation_t *operation = nullptr;
  const auto started = kb_flash_vendor_boot_ramdisk_async(
      device, partition, ramdisk_name_or_null, ramdisk_path, dtb_path_or_null,
      options_or_null, &operation, error);
  if (started != KB_OK) {
    return started;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_flash_raw_with_boot_options_async(
    kb_device_t *device, const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_legacy_boot_options_t *legacy_options_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (partition == nullptr || partition[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT, "partition must not be empty",
                device_error_identifier(device));
  }
  if (kernel_path == nullptr || kernel_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flash:raw kernel path must not be empty",
                device_error_identifier(device));
  }
  if (ramdisk_path_or_null != nullptr && ramdisk_path_or_null[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flash:raw ramdisk path must not be empty",
                device_error_identifier(device));
  }
  if (second_stage_path_or_null != nullptr &&
      second_stage_path_or_null[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flash:raw second-stage path must not be empty",
                device_error_identifier(device));
  }
  if (second_stage_path_or_null != nullptr && ramdisk_path_or_null == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flash:raw second-stage file requires a ramdisk file",
                device_error_identifier(device));
  }
  if (!valid_flash_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "flash options have an incompatible size or API version",
                device_error_identifier(device));
  }
  if (!valid_legacy_boot_options(legacy_options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "legacy boot options have an incompatible size or API version",
                device_error_identifier(device));
  }
  if (!valid_legacy_boot_option_strings(legacy_options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot construction option strings must be valid UTF-8",
                device_error_identifier(device));
  }

  try {
    const std::string_view partition_view{partition};
    if (!valid_fastboot_parameter(partition_view,
                                  std::string_view{"flash:"}.size())) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "partition must be printable ASCII and fit the Fastboot "
                  "command limit",
                  device_error_identifier(device));
    }

    if (!valid_utf8(kernel_path)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "flash:raw kernel path must be valid UTF-8",
                  device_error_identifier(device));
    }
    if (ramdisk_path_or_null != nullptr &&
        !valid_utf8(ramdisk_path_or_null)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "flash:raw ramdisk path must be valid UTF-8",
                  device_error_identifier(device));
    }
    if (second_stage_path_or_null != nullptr &&
        !valid_utf8(second_stage_path_or_null)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "flash:raw second-stage path must be valid UTF-8",
                  device_error_identifier(device));
    }

    const auto ramdisk_path =
        ramdisk_path_or_null == nullptr
            ? std::nullopt
            : std::optional<std::string_view>{ramdisk_path_or_null};
    const auto second_stage_path =
        second_stage_path_or_null == nullptr
            ? std::nullopt
            : std::optional<std::string_view>{second_stage_path_or_null};
    auto image_source = make_flash_raw_source(
        kernel_path, ramdisk_path, second_stage_path,
        legacy_boot_options_or_default(legacy_options_or_null),
        "flash:raw", device->identifier);
    if (!image_source) {
      return fail(error, image_source.error());
    }

    const auto flash_options = flash_options_or_default(options_or_null);
    auto slot_policy = copy_flash_slot_policy(options_or_null);
    if (!slot_policy) {
      return fail(error, KB_E_INVALID_ARGUMENT, slot_policy.error().c_str(),
                  device_error_identifier(device));
    }
    return start_flash_source_async(
        *device, partition_view, std::move(*image_source),
        flash_options, std::move(*slot_policy), true, operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the flash:raw operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the flash:raw operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_flash_raw_async(
    kb_device_t *device, const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  return kb_flash_raw_with_boot_options_async(
      device, partition, kernel_path, ramdisk_path_or_null,
      second_stage_path_or_null, nullptr,
      options_or_null, operation, error);
}

kb_status_t KB_CALL kb_flash_raw_with_boot_options(
    kb_device_t *device, const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_legacy_boot_options_t *legacy_options_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error) {
  kb_operation_t *operation = nullptr;
  const auto start = kb_flash_raw_with_boot_options_async(
      device, partition, kernel_path, ramdisk_path_or_null,
      second_stage_path_or_null,
      legacy_options_or_null, options_or_null, &operation, error);
  if (start != KB_OK) {
    return start;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_flash_raw(
    kb_device_t *device, const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error) {
  kb_operation_t *operation = nullptr;
  const auto start = kb_flash_raw_async(
      device, partition, kernel_path, ramdisk_path_or_null,
      second_stage_path_or_null, options_or_null,
      &operation, error);
  if (start != KB_OK) {
    return start;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_boot_raw_async(
    kb_device_t *device, const char *kernel_path,
    const char *ramdisk_path_or_null,
    const char *second_stage_path_or_null,
    const kb_legacy_boot_options_t *legacy_options_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (kernel_path == nullptr || kernel_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot kernel path must not be empty",
                device_error_identifier(device));
  }
  if (ramdisk_path_or_null != nullptr && ramdisk_path_or_null[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot ramdisk path must not be empty",
                device_error_identifier(device));
  }
  if (second_stage_path_or_null != nullptr &&
      second_stage_path_or_null[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot second-stage path must not be empty",
                device_error_identifier(device));
  }
  if (second_stage_path_or_null != nullptr && ramdisk_path_or_null == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot second-stage file requires a ramdisk file",
                device_error_identifier(device));
  }
  if (!valid_flash_options(options_or_null) ||
      !valid_legacy_boot_options(legacy_options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot options have an incompatible size or API version",
                device_error_identifier(device));
  }
  if (!valid_legacy_boot_option_strings(legacy_options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot construction option strings must be valid UTF-8",
                device_error_identifier(device));
  }

  try {
    if (!valid_utf8(kernel_path) ||
        (ramdisk_path_or_null != nullptr &&
         !valid_utf8(ramdisk_path_or_null)) ||
        (second_stage_path_or_null != nullptr &&
         !valid_utf8(second_stage_path_or_null))) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "boot component paths must be valid UTF-8",
                  device_error_identifier(device));
    }

    const auto ramdisk_path =
        ramdisk_path_or_null == nullptr
            ? std::nullopt
            : std::optional<std::string_view>{ramdisk_path_or_null};
    const auto second_stage_path =
        second_stage_path_or_null == nullptr
            ? std::nullopt
            : std::optional<std::string_view>{second_stage_path_or_null};
    auto image_source = make_flash_raw_source(
        kernel_path, ramdisk_path, second_stage_path,
        legacy_boot_options_or_default(legacy_options_or_null), "boot",
        device->identifier);
    if (!image_source) {
      return fail(error, image_source.error());
    }

    const auto boot_options = flash_options_or_default(options_or_null);
    return start_boot_source_async(*device, std::move(*image_source), boot_options,
                                   operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the boot operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the boot operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_boot_raw(
    kb_device_t *device, const char *kernel_path,
    const char *ramdisk_path_or_null,
    const char *second_stage_path_or_null,
    const kb_legacy_boot_options_t *legacy_options_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error) {
  kb_operation_t *operation = nullptr;
  const kb_status_t start = kb_boot_raw_async(
      device, kernel_path, ramdisk_path_or_null, second_stage_path_or_null,
      legacy_options_or_null, options_or_null,
      &operation, error);
  if (start != KB_OK) {
    return start;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_boot_file_async(
    kb_device_t *device, const char *file_path,
    const kb_flash_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (file_path == nullptr || file_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot file path must not be empty",
                device_error_identifier(device));
  }
  if (!valid_flash_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "boot options have an incompatible size or API version",
                device_error_identifier(device));
  }

  try {
    const std::string_view file_path_view{file_path};
    if (!valid_utf8(file_path_view)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "boot file path must be valid UTF-8",
                  device_error_identifier(device));
    }

    auto file_source =
        kairosboot::image::FileImageSource::open(utf8_path(file_path_view));
    if (!file_source) {
      return fail(error, kairosboot::api::normalize_public_error(
                             file_source.error(), device->identifier));
    }
    if (auto valid_size = kairosboot::fastboot::validate_download_size(
            (*file_source)->size());
        !valid_size) {
      return fail(error, kairosboot::api::normalize_public_error(
                             valid_size.error(), device->identifier));
    }

    const auto boot_options = flash_options_or_default(options_or_null);

    std::shared_ptr<const kairosboot::image::IImageSource> image_source =
        std::move(*file_source);
    return start_boot_source_async(*device, std::move(image_source), boot_options,
                                   operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the boot operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the boot operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_boot_file(
    kb_device_t *device, const char *file_path,
    const kb_flash_options_t *options_or_null,
    kb_error_t **error) {
  kb_operation_t *operation = nullptr;
  const kb_status_t start = kb_boot_file_async(
      device, file_path, options_or_null, &operation, error);
  if (start != KB_OK) {
    return start;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_signature_file_async(
    kb_device_t *device, const char *file_path,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (file_path == nullptr || file_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "signature file path must not be empty",
                device_error_identifier(device));
  }
  if (!valid_command_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "command options have an incompatible size, API version, or receive bound",
                device_error_identifier(device));
  }

  try {
    const std::string_view file_path_view{file_path};
    if (!valid_utf8(file_path_view)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "signature file path must be valid UTF-8",
                  device_error_identifier(device));
    }
    auto file_source =
        kairosboot::image::FileImageSource::open(utf8_path(file_path_view));
    if (!file_source) {
      return fail(error, kairosboot::api::normalize_public_error(
                             file_source.error(), device->identifier));
    }
    if ((*file_source)->size() != 256U) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "signature file must be exactly 256 bytes",
                  device_error_identifier(device));
    }
    std::shared_ptr<const kairosboot::image::IImageSource> image_source =
        std::move(*file_source);
    auto transfer_source =
        kairosboot::transport::ImageTransferSource::create(
            std::move(image_source));
    if (!transfer_source) {
      return fail(error, kairosboot::api::normalize_public_error(
                             transfer_source.error(), device->identifier));
    }

    std::shared_ptr<kairosboot::protocol::ITransferSource> source =
        std::move(*transfer_source);
    return start_primitive_async(
        device, options_or_null,
        [source = std::move(source)](
            kairosboot::fastboot::PrimitiveService &service,
            kairosboot::api::OperationState::TaskContext &task_context,
            const kb_command_options_t &options,
            const std::string &identifier)
            -> std::expected<PrimitiveExecution,
                             kairosboot::fastboot::PrimitiveError> {
          const kairosboot::protocol::TransferProgressObserver observer =
              [&task_context, &options, &identifier](
                  const std::uint64_t completed, const std::uint64_t total) {
                return task_context.cancel_requested() ||
                               !report_command_progress(
                                   options, completed, total, "download",
                                   identifier)
                           ? kairosboot::protocol::TransferProgressAction::cancel
                           : kairosboot::protocol::TransferProgressAction::
                                 continue_transfer;
              };
          auto reply = service.signature_source(source, observer);
          if (!reply) {
            return std::unexpected(std::move(reply.error()));
          }
          return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
        },
        operation, error);
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the signature operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the signature operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_signature_file(
    kb_device_t *device, const char *file_path,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_signature_file_async, result, error, device, file_path,
      options_or_null);
}

kb_status_t KB_CALL kb_update_package_async(
    kb_device_t *device, const char *package_path,
    const kb_update_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (package_path == nullptr || package_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "update package path must not be empty",
                device_error_identifier(device));
  }
  if (!valid_update_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "update options have an incompatible size, API version, or "
                "boolean policy value",
                device_error_identifier(device));
  }

  try {
    const std::string_view package_path_view{package_path};
    if (!valid_utf8(package_path_view)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "update package path must be valid UTF-8",
                  device_error_identifier(device));
    }
    auto operation_lease = try_acquire_device_operation(*device);
    if (!operation_lease) {
      return fail(error, KB_E_BUSY,
                  "device already has an active protocol operation",
                  device_error_identifier(device));
    }
    auto selector = parse_target_selector(device->selector.c_str());
    if (!selector) {
      return fail(error, selector.error());
    }

    auto copied_options = update_options_or_default(options_or_null);
    if (!copied_options) {
      return fail(error, KB_E_INVALID_ARGUMENT, copied_options.error().c_str(),
                  device_error_identifier(device));
    }
    auto native_package_path =
        std::filesystem::absolute(utf8_path(package_path_view));
    auto usb_state = device->context.usb_state;
    auto task = [operation_lease = std::move(operation_lease),
                 usb_state = std::move(usb_state),
                 selector = std::move(*selector),
                 package = std::move(native_package_path),
                 copied_options = std::move(*copied_options)](
                    kairosboot::api::OperationState::TaskContext &task_context)
        mutable -> kairosboot::api::OperationOutcome {
      static_cast<void>(operation_lease);
      return run_prepared_public_update(
          usb_state, std::move(selector), package, copied_options.native,
          copied_options.slot_policy, task_context);
    };
    auto result = std::make_unique<kb_operation>(std::move(task));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL,
                  "unable to start the update package operation",
                  device_error_identifier(device));
    }
    *operation = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the update package operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the update package operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_update_package(
    kb_device_t *device, const char *package_path,
    const kb_update_options_t *options_or_null,
    kb_error_t **error) {
  clear_error(error);
  kb_operation_t *operation = nullptr;
  const auto started = kb_update_package_async(
      device, package_path, options_or_null, &operation, error);
  if (started != KB_OK) {
    return started;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_wipe_super_async(
    kb_device_t *device, const char *super_empty_image_or_null,
    const kb_update_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (!valid_update_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "wipe-super options have an incompatible size, API version, "
                "or wipe value",
                device_error_identifier(device));
  }

  try {
    std::string image_path;
    if (super_empty_image_or_null == nullptr) {
      const auto product_out =
          kairosboot::image::detail::environment_value("ANDROID_PRODUCT_OUT");
      if (!product_out.has_value()) {
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "ANDROID_PRODUCT_OUT is not set; pass an explicit "
                    "super_empty image",
                    device_error_identifier(device));
      }
      image_path = *product_out;
      if (!image_path.ends_with('/') && !image_path.ends_with('\\')) {
        image_path.push_back(std::filesystem::path::preferred_separator);
      }
      image_path += "super_empty.img";
    } else {
      image_path = super_empty_image_or_null;
    }
    if (image_path.empty() || !valid_utf8(image_path)) {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "super_empty image path must be non-empty UTF-8",
                  device_error_identifier(device));
    }

    auto operation_lease = try_acquire_device_operation(*device);
    if (!operation_lease) {
      return fail(error, KB_E_BUSY,
                  "device already has an active protocol operation",
                  device_error_identifier(device));
    }
    auto selector = parse_target_selector(device->selector.c_str());
    if (!selector) {
      return fail(error, selector.error());
    }
    auto copied_options = update_options_or_default(options_or_null);
    if (!copied_options) {
      return fail(error, KB_E_INVALID_ARGUMENT, copied_options.error().c_str(),
                  device_error_identifier(device));
    }
    copied_options->native.wipe = 1;
    auto native_image_path =
        std::filesystem::absolute(utf8_path(image_path));
    auto usb_state = device->context.usb_state;
    auto task = [operation_lease = std::move(operation_lease),
                 usb_state = std::move(usb_state),
                 selector = std::move(*selector),
                 image = std::move(native_image_path),
                 copied_options = std::move(*copied_options)](
                    kairosboot::api::OperationState::TaskContext &task_context)
        mutable -> kairosboot::api::OperationOutcome {
      static_cast<void>(operation_lease);
      return run_public_wipe_super(usb_state, std::move(selector), image,
                                   copied_options.native, task_context);
    };
    auto result = std::make_unique<kb_operation>(std::move(task));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL,
                  "unable to start the wipe-super operation",
                  device_error_identifier(device));
    }
    *operation = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the wipe-super operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the wipe-super operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_wipe_super(
    kb_device_t *device, const char *super_empty_image_or_null,
    const kb_update_options_t *options_or_null, kb_error_t **error) {
  clear_error(error);
  kb_operation_t *operation = nullptr;
  const auto started = kb_wipe_super_async(
      device, super_empty_image_or_null, options_or_null, &operation, error);
  if (started != KB_OK) {
    return started;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_getvar_async(
    kb_device_t *device, const char *variable,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (variable == nullptr ||
      !valid_fastboot_parameter(variable, std::string_view{"getvar:"}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "getvar name must be printable ASCII and fit the Fastboot command limit",
                device_error_identifier(device));
  }
  std::string variable_copy{variable};
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const char *variable,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_getvar_async, result, error, device, variable, options_or_null);
}

kb_status_t KB_CALL kb_erase_async(
    kb_device_t *device, const char *partition,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (partition == nullptr ||
      !valid_fastboot_parameter(partition, std::string_view{"erase:"}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "erase partition must be printable ASCII and fit the Fastboot command limit",
                device_error_identifier(device));
  }
  std::string partition_copy{partition};
  return start_primitive_async(
      device, options_or_null,
      [partition_copy = std::move(partition_copy)](
          kairosboot::fastboot::PrimitiveService &service,
          kairosboot::api::OperationState::TaskContext &,
          const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = aosp_erase_partition(service, partition_copy);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_erase(
    kb_device_t *device, const char *partition,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_erase_async, result, error, device, partition, options_or_null);
}

kb_status_t KB_CALL kb_format_partition_async(
    kb_device_t *device, const char *partition,
    const char *filesystem_type_override_or_null,
    const uint64_t partition_size_override_or_zero,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (operation == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "operation output pointer must not be null");
  }
  *operation = nullptr;
  if (device == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT, "device must not be null");
  }
  if (partition == nullptr ||
      !valid_fastboot_parameter(
          partition, std::string_view{"getvar:partition-type:"}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "format partition must be printable ASCII and fit the Fastboot command limit",
                device_error_identifier(device));
  }
  if (!valid_flash_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "format options have an incompatible size or API version",
                device_error_identifier(device));
  }
  std::optional<std::string> type_override;
  if (filesystem_type_override_or_null != nullptr) {
    const std::string_view type{filesystem_type_override_or_null};
    if (type != "ext4" && type != "f2fs") {
      return fail(error, KB_E_INVALID_ARGUMENT,
                  "format filesystem type must be ext4 or f2fs",
                  device_error_identifier(device));
    }
    type_override = std::string{type};
  }
  if (partition_size_override_or_zero >
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "format partition size must fit a signed 64-bit byte count",
                device_error_identifier(device));
  }

  try {
    auto operation_lease = try_acquire_device_operation(*device);
    if (!operation_lease) {
      return fail(error, KB_E_BUSY,
                  "device already has an active protocol operation",
                  device_error_identifier(device));
    }
    auto prepared_target =
        prepare_target(device->context, device->selector.c_str());
    if (!prepared_target) {
      return fail(error, prepared_target.error());
    }

    const auto format_options = flash_options_or_default(options_or_null);
    auto selected_identifier = prepared_target->selector.identifier;
    std::string partition_copy{partition};

    auto task = [operation_lease = std::move(operation_lease),
                 target = std::move(*prepared_target),
                 selected_identifier = std::move(selected_identifier),
                 partition_copy = std::move(partition_copy),
                 type_override = std::move(type_override),
                 partition_size_override_or_zero,
                 format_options](
                    kairosboot::api::OperationState::TaskContext
                        &task_context) mutable
        -> kairosboot::api::OperationOutcome {
      static_cast<void>(operation_lease);
      if (task_context.cancel_requested()) {
        return cancelled_operation(selected_identifier,
                                   KB_TRANSFER_NOT_SENT);
      }
      kb_command_options_t transport_options;
      kb_command_options_init_sized(&transport_options,
                                    sizeof(transport_options));
      transport_options.timeout_ms = format_options.timeout_ms;
      auto opened = open_target(target, transport_options,
                                task_context.cancellation_token());
      if (!opened) {
        return operation_failure(std::move(opened.error()));
      }
      std::unique_ptr<kairosboot::protocol::ITransportSession>
          protocol_transport = std::move(*opened);
      kairosboot::protocol::SessionOptions session_options;
      session_options.io_timeout =
          std::chrono::milliseconds{format_options.timeout_ms};
      kairosboot::protocol::FastbootSession session(
          std::move(protocol_transport), session_options);
      kairosboot::fastboot::PrimitiveService service(session);
      auto cancellation = task_context.register_cancellation_hook(
          [&service] { service.request_cancel(); });

      std::string resolved_partition = partition_copy;
      auto has_slot = service.getvar("has-slot:" + partition_copy);
      if (!has_slot) {
        return operation_failure(kairosboot::api::normalize_public_error(
            has_slot.error(), selected_identifier));
      }
      if (has_slot->terminal.payload == "yes") {
        auto current_slot = service.getvar("current-slot");
        if (!current_slot) {
          return operation_failure(kairosboot::api::normalize_public_error(
              current_slot.error(), selected_identifier));
        }
        auto slot = current_slot->terminal.payload;
        if (!slot.empty() && slot.front() == '_') {
          slot.erase(slot.begin());
        }
        if (slot.empty()) {
          return operation_failure(update_error(
              KB_E_PROTOCOL,
              "device returned an empty current-slot for a slotted partition",
              selected_identifier));
        }
        resolved_partition += "_" + slot;
      }

      auto is_userspace = service.getvar("is-userspace");
      if (!is_userspace &&
          is_userspace.error().code !=
              kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
        return operation_failure(kairosboot::api::normalize_public_error(
            is_userspace.error(), selected_identifier));
      }
      auto is_logical = service.getvar("is-logical:" + resolved_partition);
      if (!is_logical &&
          is_logical.error().code !=
              kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
        return operation_failure(kairosboot::api::normalize_public_error(
            is_logical.error(), selected_identifier));
      }
      const bool userspace =
          is_userspace && is_userspace->terminal.payload == "yes";
      const bool logical =
          is_logical && is_logical->terminal.payload == "yes";
      if (logical && !userspace && format_options.force == 0) {
        return operation_failure(update_error(
            KB_E_NOT_SUPPORTED,
            "logical partition must be formatted through fastbootd; set force "
            "only when intentionally formatting a fixed bootloader partition",
            selected_identifier));
      }

      std::string filesystem_type;
      if (type_override.has_value()) {
        filesystem_type = *type_override;
      } else {
        auto reported_type =
            service.getvar("partition-type:" + resolved_partition);
        if (!reported_type) {
          return operation_failure(kairosboot::api::normalize_public_error(
              reported_type.error(), selected_identifier));
        }
        filesystem_type = std::move(reported_type->terminal.payload);
      }

      uint64_t partition_size = partition_size_override_or_zero;
      if (partition_size == 0U) {
        auto reported_size =
            service.getvar("partition-size:" + resolved_partition);
        if (!reported_size) {
          return operation_failure(kairosboot::api::normalize_public_error(
              reported_size.error(), selected_identifier));
        }
        const auto parsed_size =
            kairosboot::fastboot::parse_unsigned_variable(
                reported_size->terminal.payload);
        if (!parsed_size.has_value() || *parsed_size == 0U ||
            *parsed_size >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
          return operation_failure(update_error(
              KB_E_PROTOCOL,
              "device returned an invalid partition-size value",
              selected_identifier));
        }
        partition_size = *parsed_size;
      }

      const auto optional_block_size =
          [&service, &selected_identifier](const std::string_view variable)
          -> std::expected<uint32_t,
                           kairosboot::api::OperationErrorPayload> {
        auto value = service.getvar(variable);
        if (!value) {
          if (value.error().code ==
              kairosboot::fastboot::PrimitiveErrorCode::DeviceFail) {
            return uint32_t{0};
          }
          return std::unexpected(kairosboot::api::normalize_public_error(
              value.error(), selected_identifier));
        }
        const auto parsed = kairosboot::fastboot::parse_unsigned_variable(
            value->terminal.payload);
        if (!parsed.has_value() || *parsed > std::numeric_limits<uint32_t>::max() ||
            (*parsed != 0U && (*parsed & (*parsed - 1U)) != 0U)) {
          return uint32_t{0};
        }
        return static_cast<uint32_t>(*parsed);
      };
      auto erase_block_size = optional_block_size("erase-block-size");
      if (!erase_block_size) {
        return operation_failure(std::move(erase_block_size.error()));
      }
      auto logical_block_size = optional_block_size("logical-block-size");
      if (!logical_block_size) {
        return operation_failure(std::move(logical_block_size.error()));
      }

      if (!report_progress(format_options, 0, partition_size, "format",
                           selected_identifier)) {
        return cancelled_operation(selected_identifier,
                                   KB_TRANSFER_NOT_SENT);
      }
      auto generated = kairosboot::image::generate_empty_filesystem_image(
          filesystem_type, partition_size, *erase_block_size,
          *logical_block_size, format_options.filesystem_options);
      if (!generated) {
        return operation_failure(
            format_host_failure(generated.error(), selected_identifier));
      }
      auto file_source = kairosboot::image::FileImageSource::open(
          generated->path());
      if (!file_source) {
        return operation_failure(kairosboot::api::normalize_public_error(
            file_source.error(), selected_identifier));
      }
      std::shared_ptr<const kairosboot::image::IImageSource> image_source =
          std::move(*file_source);
      auto artifact = kairosboot::image::FlashArtifact::inspect(
          image_source, task_context.cancellation_token());
      if (!artifact) {
        return operation_failure(kairosboot::api::normalize_public_error(
            artifact.error(), selected_identifier));
      }

      uint64_t target_max_download_size = 0U;
      auto maximum = service.getvar("max-download-size");
      if (maximum) {
        target_max_download_size =
            kairosboot::fastboot::parse_unsigned_variable(
                maximum->terminal.payload)
                .value_or(0U);
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
      const auto total_transfer_size = plan->transfer_size();
      uint64_t completed_before_part = 0U;
      for (const auto &part : plan->parts()) {
        auto source = kairosboot::transport::ImageTransferSource::create(
            part.source);
        if (!source) {
          return operation_failure(kairosboot::api::normalize_public_error(
              source.error(), selected_identifier));
        }
        const kairosboot::protocol::TransferProgressObserver observer =
            [&task_context, &format_options, &selected_identifier,
             completed_before_part,
             total_transfer_size](const uint64_t completed, const uint64_t) {
              if (task_context.cancel_requested() ||
                  !report_progress(
                      format_options, completed_before_part + completed,
                      total_transfer_size, "download", selected_identifier)) {
                return kairosboot::protocol::TransferProgressAction::cancel;
              }
              return kairosboot::protocol::TransferProgressAction::
                  continue_transfer;
            };
        auto flashed = service.download_and_flash_source(
            resolved_partition, *source, observer);
        if (!flashed) {
          auto payload = kairosboot::api::normalize_public_error(
              flashed.error(), selected_identifier);
          kairosboot::api::accumulate_flash_transfer_state(
              payload, flashed.error().operation, completed_before_part,
              (*source)->size(), total_transfer_size);
          return operation_failure(std::move(payload));
        }
        completed_before_part += (*source)->size();
      }
      if (task_context.cancel_requested() ||
          !report_progress(format_options, total_transfer_size,
                           total_transfer_size, "complete",
                           selected_identifier)) {
        return cancelled_operation(
            selected_identifier, KB_TRANSFER_FULLY_TRANSFERRED,
            "operation cancelled after the format completed");
      }
      return kairosboot::api::OperationOutcome::succeeded();
    };

    auto result = std::make_unique<kb_operation>(std::move(task));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL,
                  "unable to start the format operation",
                  device_error_identifier(device));
    }
    *operation = result.release();
    return KB_OK;
  } catch (const std::bad_alloc &) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the format operation",
                device_error_identifier(device));
  } catch (const std::filesystem::filesystem_error &exception) {
    return fail(error, KB_E_IO, exception.what(), device_error_identifier(device),
                exception.code().value());
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the format operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_format_partition(
    kb_device_t *device, const char *partition,
    const char *filesystem_type_override_or_null,
    const uint64_t partition_size_override_or_zero,
    const kb_flash_options_t *options_or_null, kb_error_t **error) {
  kb_operation_t *operation = nullptr;
  const kb_status_t start = kb_format_partition_async(
      device, partition, filesystem_type_override_or_null,
      partition_size_override_or_zero,
      options_or_null, &operation, error);
  if (start != KB_OK) {
    return start;
  }
  return finish_blocking_operation(operation, error);
}

kb_status_t KB_CALL kb_set_active_async(
    kb_device_t *device, const char *slot,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (slot == nullptr ||
      !valid_fastboot_parameter(slot, std::string_view{"set_active:"}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "slot must be printable ASCII and fit the Fastboot command limit",
                device_error_identifier(device));
  }
  std::string slot_copy{slot};
  return start_primitive_async(
      device, options_or_null,
      [slot_copy = std::move(slot_copy)](
          kairosboot::fastboot::PrimitiveService &service,
          kairosboot::api::OperationState::TaskContext &,
          const kb_command_options_t &, const std::string &)
          -> std::expected<PrimitiveExecution,
                           kairosboot::fastboot::PrimitiveError> {
        auto reply = aosp_set_active(service, slot_copy);
        if (!reply) {
          return std::unexpected(std::move(reply.error()));
        }
        return PrimitiveExecution{.reply = std::move(*reply), .data = {}};
      },
      operation, error);
}

kb_status_t KB_CALL kb_set_active(
    kb_device_t *device, const char *slot,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_set_active_async, result, error, device, slot, options_or_null);
}

kb_status_t KB_CALL kb_flashing_async(
    kb_device_t *device, const kb_flashing_command_t command,
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
                "flashing command is invalid", device_error_identifier(device));
  }
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const kb_flashing_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_flashing_async, result, error, device, command, options_or_null);
}

kb_status_t KB_CALL kb_gsi_async(
    kb_device_t *device, const kb_gsi_command_t command,
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
                device_error_identifier(device));
  }
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const kb_gsi_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_gsi_async, result, error, device, command, options_or_null);
}

kb_status_t KB_CALL kb_snapshot_update_async(
    kb_device_t *device, const kb_snapshot_update_command_t command,
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
                device_error_identifier(device));
  }
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const kb_snapshot_update_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_snapshot_update_async, result, error, device, command,
      options_or_null);
}

kb_status_t KB_CALL kb_create_logical_partition_async(
    kb_device_t *device, const char *partition_name, const uint64_t size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  const auto overhead =
      std::string_view{"create-logical-partition:"}.size() + 1U +
      decimal_digit_count(size);
  if (const auto valid = validate_logical_partition_name(
          partition_name, overhead, device_error_identifier(device), error);
      valid != KB_OK) {
    return valid;
  }
  try {
    std::string name_copy{partition_name};
    return start_primitive_async(
        device, options_or_null,
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
                device_error_identifier(device));
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the create-logical-partition operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_create_logical_partition(
    kb_device_t *device, const char *partition_name, const uint64_t size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_create_logical_partition_async, result, error, device,
      partition_name, size, options_or_null);
}

kb_status_t KB_CALL kb_delete_logical_partition_async(
    kb_device_t *device, const char *partition_name,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  const auto overhead =
      std::string_view{"delete-logical-partition:"}.size();
  if (const auto valid = validate_logical_partition_name(
          partition_name, overhead, device_error_identifier(device), error);
      valid != KB_OK) {
    return valid;
  }
  try {
    std::string name_copy{partition_name};
    return start_primitive_async(
        device, options_or_null,
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
                device_error_identifier(device));
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the delete-logical-partition operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_delete_logical_partition(
    kb_device_t *device, const char *partition_name,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_delete_logical_partition_async, result, error, device,
      partition_name, options_or_null);
}

kb_status_t KB_CALL kb_resize_logical_partition_async(
    kb_device_t *device, const char *partition_name, const uint64_t size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  const auto overhead =
      std::string_view{"resize-logical-partition:"}.size() + 1U +
      decimal_digit_count(size);
  if (const auto valid = validate_logical_partition_name(
          partition_name, overhead, device_error_identifier(device), error);
      valid != KB_OK) {
    return valid;
  }
  try {
    std::string name_copy{partition_name};
    return start_primitive_async(
        device, options_or_null,
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
                device_error_identifier(device));
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to create the resize-logical-partition operation",
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_resize_logical_partition(
    kb_device_t *device, const char *partition_name, const uint64_t size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_resize_logical_partition_async, result, error, device,
      partition_name, size, options_or_null);
}

kb_status_t KB_CALL kb_reboot_async(
    kb_device_t *device, const kb_reboot_target_t target,
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
                device_error_identifier(device));
  }
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const kb_reboot_target_t target,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_reboot_async, result, error, device, target, options_or_null);
}

kb_status_t KB_CALL kb_continue_boot_async(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_operation_t **operation,
    kb_error_t **error) {
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_continue_boot_async, result, error, device, options_or_null);
}

kb_status_t KB_CALL kb_oem_async(
    kb_device_t *device, const char *command_suffix,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (command_suffix == nullptr ||
      !valid_fastboot_parameter(command_suffix, std::string_view{"oem "}.size())) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "OEM suffix must be printable ASCII and fit the Fastboot command limit",
                device_error_identifier(device));
  }
  std::string suffix_copy{command_suffix};
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const char *command_suffix,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_oem_async, result, error, device, command_suffix, options_or_null);
}

kb_status_t KB_CALL kb_raw_command_async(
    kb_device_t *device, const char *command,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  clear_error(error);
  if (command == nullptr || !valid_fastboot_parameter(command, 0)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "raw command must be printable ASCII and fit the Fastboot command limit",
                device_error_identifier(device));
  }
  std::string command_copy{command};
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const char *command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_raw_command_async, result, error, device, command, options_or_null);
}

kb_status_t KB_CALL kb_boot_async(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_operation_t **operation,
    kb_error_t **error) {
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_boot_async, result, error, device, options_or_null);
}

kb_status_t KB_CALL kb_stage_async(
    kb_device_t *device, const void *data, const size_t data_size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  if (data == nullptr || data_size == 0 ||
      data_size > std::numeric_limits<std::uint32_t>::max()) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "stage data must be non-empty and fit the Fastboot 32-bit length",
                device_error_identifier(device));
  }
  try {
    const auto *first = static_cast<const std::byte *>(data);
    std::vector<std::byte> copied(first, first + data_size);
    return start_primitive_async(
        device, options_or_null,
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
                device_error_identifier(device));
  }
}

kb_status_t KB_CALL kb_stage(
    kb_device_t *device, const void *data, const size_t data_size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_stage_async, result, error, device, data, data_size, options_or_null);
}

kb_status_t KB_CALL kb_upload_async(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_operation_t **operation,
    kb_error_t **error) {
  return start_primitive_async(
      device, options_or_null,
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
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_upload_async, result, error, device, options_or_null);
}

kb_status_t KB_CALL kb_fetch_async(
    kb_device_t *device, const char *partition,
    const uint64_t offset_or_unspecified,
    const uint64_t size_or_unspecified,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  constexpr auto maximum_range =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (partition == nullptr || !valid_fetch_partition(partition)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch partition must use letters, digits, '_', '-', or '.'",
                device_error_identifier(device));
  }
  if (size_or_unspecified != KB_FETCH_UNSPECIFIED &&
      offset_or_unspecified == KB_FETCH_UNSPECIFIED) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch size requires an explicit offset",
                device_error_identifier(device));
  }
  if ((offset_or_unspecified != KB_FETCH_UNSPECIFIED &&
       offset_or_unspecified > maximum_range) ||
      (size_or_unspecified != KB_FETCH_UNSPECIFIED &&
       size_or_unspecified > maximum_range)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch offset and size must fit signed 64-bit values",
                device_error_identifier(device));
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
                device_error_identifier(device));
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
      device, options_or_null,
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
    kb_device_t *device, const char *partition,
    const uint64_t offset_or_unspecified,
    const uint64_t size_or_unspecified,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_fetch_async, result, error, device, partition, offset_or_unspecified,
      size_or_unspecified, options_or_null);
}

kb_status_t KB_CALL kb_upload_file_async(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  return start_file_receive_async(
      device, output_path, options_or_null, "upload",
      [](kairosboot::fastboot::FileReceiveService &files,
         const std::filesystem::path &destination,
         const std::uint64_t maximum_bytes,
         const kairosboot::protocol::TransferProgressObserver &observer) {
        return files.upload(destination, maximum_bytes, observer);
      },
      operation, error);
}

kb_status_t KB_CALL kb_upload_file(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_upload_file_async, result, error, device, output_path,
      options_or_null);
}

kb_status_t KB_CALL kb_get_staged_file_async(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error) {
  return start_file_receive_async(
      device, output_path, options_or_null, "get-staged",
      [](kairosboot::fastboot::FileReceiveService &files,
         const std::filesystem::path &destination,
         const std::uint64_t maximum_bytes,
         const kairosboot::protocol::TransferProgressObserver &observer) {
        return files.get_staged(destination, maximum_bytes, observer);
      },
      operation, error);
}

kb_status_t KB_CALL kb_get_staged_file(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_get_staged_file_async, result, error, device, output_path,
      options_or_null);
}

kb_status_t KB_CALL kb_fetch_file_async(
    kb_device_t *device, const char *partition,
    const uint64_t offset_or_unspecified,
    const uint64_t size_or_unspecified, const char *output_path,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error) {
  clear_error(error);
  constexpr auto maximum_range =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (partition == nullptr || !valid_fetch_partition(partition)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch partition must use letters, digits, '_', '-', or '.'",
                device_error_identifier(device));
  }
  if (size_or_unspecified != KB_FETCH_UNSPECIFIED &&
      offset_or_unspecified == KB_FETCH_UNSPECIFIED) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch size requires an explicit offset",
                device_error_identifier(device));
  }
  if ((offset_or_unspecified != KB_FETCH_UNSPECIFIED &&
       offset_or_unspecified > maximum_range) ||
      (size_or_unspecified != KB_FETCH_UNSPECIFIED &&
       size_or_unspecified > maximum_range)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "fetch offset and size must fit signed 64-bit values",
                device_error_identifier(device));
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
                device_error_identifier(device));
  }
  kairosboot::fastboot::FetchRange range;
  if (offset_or_unspecified != KB_FETCH_UNSPECIFIED) {
    range.offset = offset_or_unspecified;
  }
  if (size_or_unspecified != KB_FETCH_UNSPECIFIED) {
    range.size = size_or_unspecified;
  }
  std::string partition_copy{partition};
  return start_file_receive_async(
      device, output_path, options_or_null, "fetch",
      [partition_copy = std::move(partition_copy), range](
          kairosboot::fastboot::FileReceiveService &files,
          const std::filesystem::path &destination,
          const std::uint64_t maximum_bytes,
          const kairosboot::protocol::TransferProgressObserver &observer) {
        return files.fetch(partition_copy, range, destination, maximum_bytes,
                           observer);
      },
      operation, error);
}

kb_status_t KB_CALL kb_fetch_file(
    kb_device_t *device, const char *partition,
    const uint64_t offset_or_unspecified,
    const uint64_t size_or_unspecified, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error) {
  return run_blocking_command(
      kb_fetch_file_async, result, error, device, partition,
      offset_or_unspecified, size_or_unspecified, output_path,
      options_or_null);
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

const char *KB_CALL kb_command_result_output_path(
    const kb_command_result_t *result) {
  const auto *payload = command_result_payload(result);
  return payload == nullptr ? "" : payload->output_path.c_str();
}

uint64_t KB_CALL kb_command_result_received_bytes(
    const kb_command_result_t *result) {
  const auto *payload = command_result_payload(result);
  return payload == nullptr ? 0U : payload->received_bytes;
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
