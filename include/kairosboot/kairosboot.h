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

typedef int32_t kb_flashing_command_t;
enum {
  KB_FLASHING_LOCK = 0,
  KB_FLASHING_UNLOCK = 1,
  KB_FLASHING_LOCK_CRITICAL = 2,
  KB_FLASHING_UNLOCK_CRITICAL = 3,
  KB_FLASHING_GET_UNLOCK_ABILITY = 4
};

typedef int32_t kb_gsi_command_t;
enum {
  KB_GSI_WIPE = 0,
  KB_GSI_DISABLE = 1,
  KB_GSI_STATUS = 2
};

typedef int32_t kb_snapshot_update_command_t;
enum {
  KB_SNAPSHOT_UPDATE_CANCEL = 0,
  KB_SNAPSHOT_UPDATE_MERGE = 1
};

typedef struct kb_context kb_context_t;
typedef struct kb_device_list kb_device_list_t;
typedef struct kb_error kb_error_t;
typedef struct kb_operation kb_operation_t;
typedef struct kb_command_result kb_command_result_t;
typedef struct kb_job_plan kb_job_plan_t;
typedef struct kb_job kb_job_t;
typedef struct kb_job_report kb_job_report_t;

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

typedef struct kb_update_options {
  uint32_t struct_size;
  uint32_t api_version;
  /* Whole-operation timeout in milliseconds, including package preflight,
   * device selection, open, validation and every update task. The initialized
   * default is infinite. */
  uint32_t timeout_ms;
  /* Zero preserves userdata; one applies wipe-conditioned package tasks. */
  int32_t wipe;
  /* Update stages are preflight, select, open, validate, getvar, prepare,
   * execute, download and complete. Byte totals are non-zero only while an
   * immutable image payload is being transferred. */
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
} kb_update_options_t;

typedef struct kb_command_options {
  uint32_t struct_size;
  uint32_t api_version;
  /* Per-I/O deadline in milliseconds. The initialized default is infinite. */
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
  /* Hard receive bound for upload/get-staged/fetch. The default is 64 MiB. */
  uint64_t maximum_receive_bytes;
} kb_command_options_t;

typedef struct kb_job_options {
  uint32_t struct_size;
  uint32_t api_version;
  /* Whole-job deadline in milliseconds, including manifest parsing,
   * planning, preflight, device execution, cancellation drain and report
   * publication. The initialized default is infinite. */
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
} kb_job_options_t;

#define KB_VERSION_V1_SIZE                                                   \
  ((uint32_t)(offsetof(kb_version_t, string) +                               \
              sizeof(((kb_version_t *)0)->string)))
#define KB_CONTEXT_OPTIONS_V1_SIZE                                           \
  ((uint32_t)(offsetof(kb_context_options_t, log_user_data) +                \
              sizeof(((kb_context_options_t *)0)->log_user_data)))
#define KB_FLASH_OPTIONS_V1_SIZE                                             \
  ((uint32_t)(offsetof(kb_flash_options_t, progress_user_data) +             \
              sizeof(((kb_flash_options_t *)0)->progress_user_data)))
#define KB_UPDATE_OPTIONS_V1_SIZE                                            \
  ((uint32_t)(offsetof(kb_update_options_t, progress_user_data) +            \
              sizeof(((kb_update_options_t *)0)->progress_user_data)))
#define KB_COMMAND_OPTIONS_V1_SIZE                                           \
  ((uint32_t)(offsetof(kb_command_options_t, maximum_receive_bytes) +        \
              sizeof(((kb_command_options_t *)0)->maximum_receive_bytes)))
#define KB_JOB_OPTIONS_V1_SIZE                                               \
  ((uint32_t)(offsetof(kb_job_options_t, progress_user_data) +               \
              sizeof(((kb_job_options_t *)0)->progress_user_data)))

KB_API void KB_CALL kb_context_options_init(kb_context_options_t *options);
KB_API void KB_CALL kb_flash_options_init(kb_flash_options_t *options);
KB_API void KB_CALL kb_update_options_init(kb_update_options_t *options);
KB_API void KB_CALL kb_command_options_init(kb_command_options_t *options);
KB_API void KB_CALL kb_job_options_init(kb_job_options_t *options);
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

/* serial_or_null selects the sole USB device when NULL. Values beginning with
 * tcp: or udp: use the typed network selector grammar documented below; every
 * other non-NULL value retains the legacy exact USB serial behavior. */
KB_API kb_status_t KB_CALL kb_flash_file_async(
    kb_context_t *context, const char *serial_or_null, const char *partition,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flash_file(
    kb_context_t *context, const char *serial_or_null, const char *partition,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_error_t **error);

/* Builds the default Android boot image used by AOSP fastboot flash:raw from
 * KERNEL and optional RAMDISK/SECOND files, then flashes it to PARTITION. If
 * KERNEL is already an Android boot image, it is flashed unchanged and both
 * optional paths must be NULL. SECOND requires a non-NULL RAMDISK. */
KB_API kb_status_t KB_CALL kb_flash_raw_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flash_raw(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error);

/* Streams one immutable file through Fastboot download and then issues boot on
 * the same selected session. The image must be non-empty and fit the protocol's
 * 32-bit download length. The blocking entry point starts the same operation
 * and waits for its terminal state. */
KB_API kb_status_t KB_CALL kb_boot_file_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_boot_file(
    kb_context_t *context, const char *device_selector_or_null,
    const char *file_path, const kb_flash_options_t *options_or_null,
    kb_error_t **error);

/* Performs complete package preflight before USB enumeration or any transport
 * open. device_selector_or_null uses the typed selector grammar documented
 * below. The selected target is bound exactly once for this operation.
 * Packages that require bootloader-to-fastbootd re-enumeration fail with
 * KB_E_NOT_SUPPORTED before any destructive task until the production USB
 * opener can provide a verified physical-port reconnect binding. An already
 * open fastbootd target remains supported. The blocking entry point starts the
 * same async operation and waits for its terminal state. */
KB_API kb_status_t KB_CALL kb_update_package_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *package_path, const kb_update_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_update_package(
    kb_context_t *context, const char *device_selector_or_null,
    const char *package_path, const kb_update_options_t *options_or_null,
    kb_error_t **error);

/* Resets dynamic partitions from an immutable super_empty image through the
 * existing fastbootd update-super transaction. When
 * super_empty_image_or_null is NULL, ANDROID_PRODUCT_OUT/super_empty.img is
 * used, matching AOSP Fastboot's command-line lookup rule. */
KB_API kb_status_t KB_CALL kb_wipe_super_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *super_empty_image_or_null,
    const kb_update_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_wipe_super(
    kb_context_t *context, const char *device_selector_or_null,
    const char *super_empty_image_or_null,
    const kb_update_options_t *options_or_null, kb_error_t **error);

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
/* Creates an empty ext4 or f2fs Android sparse image using the matching AOSP
 * host tool, then downloads and flashes it in the same device session. A NULL
 * filesystem_type_override queries partition-type:<partition>; a zero
 * partition_size_override queries partition-size:<partition>. The generator
 * is resolved beside the process executable or on PATH. KAIROSBOOT_MKE2FS and
 * KAIROSBOOT_MAKE_F2FS may name explicit tool paths. */
KB_API kb_status_t KB_CALL kb_format_partition_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const char *filesystem_type_override_or_null,
    uint64_t partition_size_override_or_zero,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_format_partition(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, const char *filesystem_type_override_or_null,
    uint64_t partition_size_override_or_zero,
    const kb_flash_options_t *options_or_null, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_set_active_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *slot, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_set_active(
    kb_context_t *context, const char *device_selector_or_null,
    const char *slot, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flashing_async(
    kb_context_t *context, const char *device_selector_or_null,
    kb_flashing_command_t command,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flashing(
    kb_context_t *context, const char *device_selector_or_null,
    kb_flashing_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_gsi_async(
    kb_context_t *context, const char *device_selector_or_null,
    kb_gsi_command_t command, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_gsi(
    kb_context_t *context, const char *device_selector_or_null,
    kb_gsi_command_t command, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_snapshot_update_async(
    kb_context_t *context, const char *device_selector_or_null,
    kb_snapshot_update_command_t command,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_snapshot_update(
    kb_context_t *context, const char *device_selector_or_null,
    kb_snapshot_update_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_create_logical_partition_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name, uint64_t size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_create_logical_partition(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name, uint64_t size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_delete_logical_partition_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_delete_logical_partition(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_resize_logical_partition_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name, uint64_t size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_resize_logical_partition(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition_name, uint64_t size,
    const kb_command_options_t *options_or_null,
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

/* Bounded, constant-memory device-to-host file operations. The destination is
 * replaced atomically only after the exact DATA payload and terminal OKAY have
 * been received and synchronized. On every failure or cancellation, an
 * existing destination is preserved. output_path is UTF-8. upload-file and
 * get-staged-file intentionally use the same AOSP "upload" wire command. */
KB_API kb_status_t KB_CALL kb_upload_file_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *output_path, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_upload_file(
    kb_context_t *context, const char *device_selector_or_null,
    const char *output_path, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_get_staged_file_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *output_path, const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_get_staged_file(
    kb_context_t *context, const char *device_selector_or_null,
    const char *output_path, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_fetch_file_async(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, uint64_t offset_or_unspecified,
    uint64_t size_or_unspecified, const char *output_path,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_fetch_file(
    kb_context_t *context, const char *device_selector_or_null,
    const char *partition, uint64_t offset_or_unspecified,
    uint64_t size_or_unspecified, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);

/* Context-free Fleet manifest entry points. Neither function needs a
 * kb_context_t: parsing and validation never initialize libusb, enumerate
 * devices, or open artifact paths; only the manifest file itself is read.
 * kb_validate_job_file stops after full semantic validation. Failures use
 * the standard error handle: the manifest source path and, when known, its
 * line and column appear inside the stable UTF-8 message and the platform
 * native code is preserved in kb_error_native_code(). */
KB_API kb_status_t KB_CALL kb_validate_job_file(const char *file_path,
                                                kb_error_t **error);
KB_API kb_status_t KB_CALL kb_plan_job_file(
    const char *file_path, kb_job_plan_t **plan, kb_error_t **error);

/* Planning returns an immutable snapshot owned by the caller. The borrowed
 * canonical JSON is NUL-terminated UTF-8 without a trailing LF; *size, when
 * requested, excludes the terminator. Both borrows stay valid until the
 * plan is released. */
KB_API const char *KB_CALL kb_job_plan_canonical_json(
    const kb_job_plan_t *plan, size_t *size);
KB_API const char *KB_CALL kb_job_plan_sha256_hex(const kb_job_plan_t *plan);
KB_API void KB_CALL kb_job_plan_release(kb_job_plan_t *plan);

/* Runs a versioned Fleet manifest. The blocking entry point always starts the
 * matching asynchronous job and waits for it. A terminal report is returned
 * for success, cancellation and execution/preflight failure whenever report
 * construction itself succeeds. The report is owned by the caller and stays
 * valid independently of the job handle. */
KB_API kb_status_t KB_CALL kb_run_job_file_async(
    kb_context_t *context, const char *file_path,
    const kb_job_options_t *options_or_null, kb_job_t **job,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_run_job_file(
    kb_context_t *context, const char *file_path,
    const kb_job_options_t *options_or_null, kb_job_report_t **report,
    kb_error_t **error);

KB_API kb_status_t KB_CALL kb_job_wait(kb_job_t *job, uint32_t timeout_ms);
KB_API kb_status_t KB_CALL kb_job_cancel(kb_job_t *job);
KB_API kb_operation_state_t KB_CALL kb_job_state(const kb_job_t *job);
KB_API const kb_error_t *KB_CALL kb_job_error(const kb_job_t *job);
KB_API kb_status_t KB_CALL kb_job_get_report(
    const kb_job_t *job, kb_job_report_t **report, kb_error_t **error);
KB_API void KB_CALL kb_job_release(kb_job_t *job);

/* Borrowed NUL-terminated UTF-8 canonical JSON. *size excludes the terminator;
 * the borrow remains valid until kb_job_report_release(). */
KB_API const char *KB_CALL kb_job_report_json(
    const kb_job_report_t *report, size_t *size);
KB_API void KB_CALL kb_job_report_release(kb_job_report_t *report);

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
/* Borrowed NUL-terminated UTF-8 destination used by a successful file receive,
 * or an empty string for commands that did not publish a file. */
KB_API const char *KB_CALL kb_command_result_output_path(
    const kb_command_result_t *result);
/* Exact DATA byte count received into memory or atomically published to file. */
KB_API uint64_t KB_CALL kb_command_result_received_bytes(
    const kb_command_result_t *result);
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
