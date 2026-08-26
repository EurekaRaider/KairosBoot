#ifndef KAIROSBOOT_KAIROSBOOT_H
#define KAIROSBOOT_KAIROSBOOT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(KAIROSBOOT_BUILDING_LIBRARY)
#define KB_API __declspec(dllexport)
#else
#define KB_API __declspec(dllimport)
#endif
#define KB_CALL __cdecl
#elif defined(__GNUC__) || defined(__clang__)
#define KB_API __attribute__((visibility("default")))
#define KB_CALL
#else
#define KB_API
#define KB_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KB_API_VERSION UINT32_C(1)
#define KB_WAIT_INFINITE UINT32_MAX
#define KB_FETCH_UNSPECIFIED UINT64_MAX

typedef int32_t kb_status_t;
enum {
  KB_OK = 0,
  KB_E_INVALID_ARGUMENT = 1,
  KB_E_OUT_OF_MEMORY = 2,
  KB_E_NOT_SUPPORTED = 3,
  KB_E_NO_DEVICE = 4,
  KB_E_AMBIGUOUS_DEVICE = 5,
  KB_E_BUSY = 6,
  KB_E_TIMEOUT = 7,
  KB_E_CANCELLED = 8,
  KB_E_IO = 9,
  KB_E_INTERNAL = 10,
  KB_E_PROTOCOL = 11,
  KB_E_DEVICE_FAIL = 12
};

typedef int32_t kb_transfer_state_t;
enum {
  KB_TRANSFER_NOT_SENT = 0,
  KB_TRANSFER_PARTIAL_OR_UNKNOWN = 1,
  KB_TRANSFER_FULLY_TRANSFERRED = 2
};

typedef int32_t kb_operation_state_t;
enum {
  KB_OPERATION_CREATED = 0,
  KB_OPERATION_RUNNING = 1,
  KB_OPERATION_SUCCEEDED = 2,
  KB_OPERATION_FAILED = 3,
  KB_OPERATION_CANCELLED = 4
};

typedef int32_t kb_progress_action_t;
enum {
  KB_PROGRESS_CONTINUE = 0,
  KB_PROGRESS_CANCEL = 1
};

typedef int32_t kb_command_message_kind_t;
enum {
  KB_COMMAND_MESSAGE_INFO = 0,
  KB_COMMAND_MESSAGE_TEXT = 1
};

typedef int32_t kb_reboot_target_t;
enum {
  KB_REBOOT_SYSTEM = 0,
  KB_REBOOT_BOOTLOADER = 1,
  KB_REBOOT_RECOVERY = 2,
  KB_REBOOT_FASTBOOT = 3
};

typedef struct kb_context kb_context_t;
typedef struct kb_device_list kb_device_list_t;
typedef struct kb_error kb_error_t;
typedef struct kb_operation kb_operation_t;
typedef struct kb_command_result kb_command_result_t;

typedef struct kb_version {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  const char *string;
} kb_version_t;

typedef void(KB_CALL *kb_log_callback_t)(int32_t level, const char *message,
                                         void *user_data);

typedef struct kb_context_options {
  uint32_t struct_size;
  uint32_t api_version;
  kb_log_callback_t log_callback;
  void *log_user_data;
} kb_context_options_t;

typedef struct kb_progress {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t bytes_completed;
  uint64_t bytes_total;
  const char *stage;
  const char *device_identifier;
} kb_progress_t;

/* Progress callbacks are serialized for each operation and never run on the
 * libusb event thread. Return KB_PROGRESS_CANCEL to request cancellation.
 * progress_user_data must remain valid until the operation is released. Do not
 * release the same operation from its callback; a different thread may release
 * it and will wait until the callback and transport drain complete. */
typedef kb_progress_action_t(KB_CALL *kb_progress_callback_t)(
    const kb_progress_t *progress, void *user_data);

typedef struct kb_flash_options {
  uint32_t struct_size;
  uint32_t api_version;
  /* Per-I/O deadline in milliseconds. The initialized default is infinite. */
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
} kb_flash_options_t;

typedef struct kb_command_options {
  uint32_t struct_size;
  uint32_t api_version;
  /* Per-I/O deadline in milliseconds. The initialized default is infinite. */
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
  /* Hard in-memory bound for upload/fetch. The default is 64 MiB. */
  uint64_t maximum_receive_bytes;
} kb_command_options_t;

#define KB_VERSION_V1_SIZE                                                   \
  ((uint32_t)(offsetof(kb_version_t, string) +                               \
              sizeof(((kb_version_t *)0)->string)))
#define KB_CONTEXT_OPTIONS_V1_SIZE                                           \
  ((uint32_t)(offsetof(kb_context_options_t, log_user_data) +                \
              sizeof(((kb_context_options_t *)0)->log_user_data)))
#define KB_FLASH_OPTIONS_V1_SIZE                                             \
  ((uint32_t)(offsetof(kb_flash_options_t, progress_user_data) +             \
              sizeof(((kb_flash_options_t *)0)->progress_user_data)))
#define KB_COMMAND_OPTIONS_V1_SIZE                                           \
  ((uint32_t)(offsetof(kb_command_options_t, maximum_receive_bytes) +        \
              sizeof(((kb_command_options_t *)0)->maximum_receive_bytes)))

KB_API void KB_CALL kb_context_options_init(kb_context_options_t *options);
KB_API void KB_CALL kb_flash_options_init(kb_flash_options_t *options);
KB_API void KB_CALL kb_command_options_init(kb_command_options_t *options);
KB_API void KB_CALL kb_version_init(kb_version_t *version);

KB_API kb_status_t KB_CALL kb_get_version(kb_version_t *version);
KB_API const char *KB_CALL kb_status_string(kb_status_t status);

KB_API kb_status_t KB_CALL kb_context_create(
    const kb_context_options_t *options, kb_context_t **context,
    kb_error_t **error);
KB_API void KB_CALL kb_context_release(kb_context_t *context);

KB_API kb_status_t KB_CALL kb_enumerate_devices(
    kb_context_t *context, kb_device_list_t **devices, kb_error_t **error);
KB_API size_t KB_CALL kb_device_list_count(const kb_device_list_t *devices);
KB_API const char *KB_CALL kb_device_list_serial(const kb_device_list_t *devices,
                                                  size_t index);
KB_API const char *KB_CALL kb_device_list_usb_path(
    const kb_device_list_t *devices, size_t index);
KB_API const char *KB_CALL kb_device_list_product(
    const kb_device_list_t *devices, size_t index);
KB_API void KB_CALL kb_device_list_release(kb_device_list_t *devices);

/* For backward compatibility, serial_or_null is always an exact USB serial;
 * values beginning with tcp:, udp:, or usb: are not interpreted as selectors. */
KB_API kb_status_t KB_CALL kb_flash_file_async(
    kb_context_t *context, const char *serial_or_null, const char *partition,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flash_file(
    kb_context_t *context, const char *serial_or_null, const char *partition,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_error_t **error);

/* Typed primitive selectors:
 *   NULL                         sole USB Fastboot device
 *   SERIAL                       exact legacy USB serial
 *   usb:serial:<percent-encoded> exact UTF-8 USB serial
 *   usb:<bus>-<port>[.<port>...] physical USB path
 *   tcp:<host>[:port]            Fastboot TCP (default port 5554)
 *   udp:<host>[:port]            Fastboot UDP (default port 5554)
 * IPv6 network hosts use brackets. Blocking calls start the matching async
 * operation, wait, and extract its immutable result. A successful result is
 * owned by the caller and must be released with kb_command_result_release(). */
KB_API kb_status_t KB_CALL kb_getvar_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *variable, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_getvar(
    kb_context_t *context, const char *device_selector_or_null,
    const char *variable, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_erase_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_erase(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_set_active_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *slot, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_set_active(
    kb_context_t *context, const char *device_selector_or_null,
    const char *slot, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_reboot_async(
    kb_context_t *context, const char *device_selector_or_null,
    kb_reboot_target_t target,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_reboot(
    kb_context_t *context, const char *device_selector_or_null,
    kb_reboot_target_t target,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_continue_boot_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_continue_boot(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_oem_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *command_suffix,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_oem(
    kb_context_t *context, const char *device_selector_or_null,
    const char *command_suffix,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_raw_command_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *command, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_raw_command(
    kb_context_t *context, const char *device_selector_or_null,
    const char *command, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_boot_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_boot(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_stage_async(
    kb_context_t *context, const char *device_selector_or_null,
    const void *data, size_t data_size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_stage(
    kb_context_t *context, const char *device_selector_or_null,
    const void *data, size_t data_size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_upload_async(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_upload(
    kb_context_t *context, const char *device_selector_or_null,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_fetch_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, uint64_t offset_or_unspecified,
    uint64_t size_or_unspecified,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_fetch(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, uint64_t offset_or_unspecified,
    uint64_t size_or_unspecified,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);

KB_API kb_status_t KB_CALL kb_operation_wait(kb_operation_t *operation,
                                              uint32_t timeout_ms);
KB_API kb_status_t KB_CALL kb_operation_cancel(kb_operation_t *operation);
KB_API kb_operation_state_t KB_CALL
kb_operation_state(const kb_operation_t *operation);
KB_API const kb_error_t *KB_CALL
kb_operation_error(const kb_operation_t *operation);
KB_API kb_status_t KB_CALL kb_operation_command_result(
    const kb_operation_t *operation, kb_command_result_t **result,
    kb_error_t **error);
KB_API void KB_CALL kb_operation_release(kb_operation_t *operation);

KB_API const uint8_t *KB_CALL kb_command_result_terminal_payload(
    const kb_command_result_t *result, size_t *size);
KB_API size_t KB_CALL kb_command_result_message_count(
    const kb_command_result_t *result);
KB_API kb_command_message_kind_t KB_CALL kb_command_result_message_kind(
    const kb_command_result_t *result, size_t index);
KB_API const uint8_t *KB_CALL kb_command_result_message_payload(
    const kb_command_result_t *result, size_t index, size_t *size);
KB_API const uint8_t *KB_CALL kb_command_result_data(
    const kb_command_result_t *result, size_t *size);
KB_API const char *KB_CALL kb_command_result_device_identifier(
    const kb_command_result_t *result);
KB_API void KB_CALL kb_command_result_release(kb_command_result_t *result);

KB_API kb_status_t KB_CALL kb_error_status(const kb_error_t *error);
KB_API const char *KB_CALL kb_error_message(const kb_error_t *error);
KB_API const char *KB_CALL
kb_error_device_identifier(const kb_error_t *error);
KB_API int32_t KB_CALL kb_error_native_code(const kb_error_t *error);
KB_API kb_transfer_state_t KB_CALL
kb_error_transfer_state(const kb_error_t *error);
KB_API const uint8_t *KB_CALL kb_error_device_message(
    const kb_error_t *error, size_t *size);
KB_API size_t KB_CALL kb_error_command_message_count(const kb_error_t *error);
KB_API kb_command_message_kind_t KB_CALL kb_error_command_message_kind(
    const kb_error_t *error, size_t index);
KB_API const uint8_t *KB_CALL kb_error_command_message_payload(
    const kb_error_t *error, size_t index, size_t *size);
KB_API uint64_t KB_CALL kb_error_inbound_expected_bytes(
    const kb_error_t *error);
KB_API uint64_t KB_CALL kb_error_inbound_transferred_bytes(
    const kb_error_t *error);
KB_API kb_transfer_state_t KB_CALL kb_error_inbound_transfer_state(
    const kb_error_t *error);
KB_API int32_t KB_CALL kb_error_session_poisoned(const kb_error_t *error);
KB_API void KB_CALL kb_error_release(kb_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
