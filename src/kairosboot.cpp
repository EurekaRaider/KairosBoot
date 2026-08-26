#include <kairosboot/kairosboot.h>

#include <atomic>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct kb_context {
  kb_context_options_t options{};
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
  std::atomic<kb_operation_state_t> state{KB_OPERATION_CREATED};
  std::unique_ptr<kb_error> error;
};

namespace {

constexpr uint32_t kDefaultTimeoutMs = 30'000;
constexpr char kNoTransportMessage[] =
    "No device transport is compiled into this build.";

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

const char *device_field(const kb_device_list_t *devices, size_t index,
                         const std::string kb_device_info::*field) noexcept {
  if (devices == nullptr || index >= devices->devices.size()) {
    return nullptr;
  }
  return (devices->devices[index].*field).c_str();
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
  return fail(error, KB_E_NOT_SUPPORTED, kNoTransportMessage);
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
  return fail(error, KB_E_NOT_SUPPORTED, kNoTransportMessage, serial_or_null,
              0, KB_TRANSFER_NOT_SENT);
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

  const uint32_t timeout =
      options_or_null == nullptr ? kDefaultTimeoutMs : options_or_null->timeout_ms;
  const kb_status_t result = kb_operation_wait(operation, timeout);
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
                                      uint32_t /*timeout_ms*/) {
  if (operation == nullptr) {
    return KB_E_INVALID_ARGUMENT;
  }
  switch (operation->state.load()) {
  case KB_OPERATION_SUCCEEDED:
    return KB_OK;
  case KB_OPERATION_CANCELLED:
    return KB_E_CANCELLED;
  case KB_OPERATION_FAILED:
    return operation->error == nullptr ? KB_E_INTERNAL
                                       : operation->error->status;
  default:
    return KB_E_TIMEOUT;
  }
}

kb_status_t KB_CALL kb_operation_cancel(kb_operation_t *operation) {
  if (operation == nullptr) {
    return KB_E_INVALID_ARGUMENT;
  }
  const kb_operation_state_t state = operation->state.load();
  if (state == KB_OPERATION_SUCCEEDED || state == KB_OPERATION_FAILED ||
      state == KB_OPERATION_CANCELLED) {
    return KB_OK;
  }
  operation->state.store(KB_OPERATION_CANCELLED);
  return KB_OK;
}

kb_operation_state_t KB_CALL
kb_operation_state(const kb_operation_t *operation) {
  return operation == nullptr ? KB_OPERATION_FAILED : operation->state.load();
}

const kb_error_t *KB_CALL
kb_operation_error(const kb_operation_t *operation) {
  return operation == nullptr ? nullptr : operation->error.get();
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
