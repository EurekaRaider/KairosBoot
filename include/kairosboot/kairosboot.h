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
  KB_E_INTERNAL = 10
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

typedef struct kb_context kb_context_t;
typedef struct kb_device_list kb_device_list_t;
typedef struct kb_error kb_error_t;
typedef struct kb_operation kb_operation_t;

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

typedef kb_progress_action_t(KB_CALL *kb_progress_callback_t)(
    const kb_progress_t *progress, void *user_data);

typedef struct kb_flash_options {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
} kb_flash_options_t;

KB_API void KB_CALL kb_context_options_init(kb_context_options_t *options);
KB_API void KB_CALL kb_flash_options_init(kb_flash_options_t *options);
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

KB_API kb_status_t KB_CALL kb_flash_file_async(
    kb_context_t *context, const char *serial_or_null, const char *partition,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flash_file(
    kb_context_t *context, const char *serial_or_null, const char *partition,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_error_t **error);

KB_API kb_status_t KB_CALL kb_operation_wait(kb_operation_t *operation,
                                              uint32_t timeout_ms);
KB_API kb_status_t KB_CALL kb_operation_cancel(kb_operation_t *operation);
KB_API kb_operation_state_t KB_CALL
kb_operation_state(const kb_operation_t *operation);
KB_API const kb_error_t *KB_CALL
kb_operation_error(const kb_operation_t *operation);
KB_API void KB_CALL kb_operation_release(kb_operation_t *operation);

KB_API kb_status_t KB_CALL kb_error_status(const kb_error_t *error);
KB_API const char *KB_CALL kb_error_message(const kb_error_t *error);
KB_API const char *KB_CALL
kb_error_device_identifier(const kb_error_t *error);
KB_API int32_t KB_CALL kb_error_native_code(const kb_error_t *error);
KB_API kb_transfer_state_t KB_CALL
kb_error_transfer_state(const kb_error_t *error);
KB_API void KB_CALL kb_error_release(kb_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
