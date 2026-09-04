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

typedef uint32_t kb_filesystem_options_t;
enum {
  KB_FILESYSTEM_OPTION_NONE = 0,
  KB_FILESYSTEM_OPTION_CASEFOLD = UINT32_C(1) << 0,
  KB_FILESYSTEM_OPTION_PROJID = UINT32_C(1) << 1,
  KB_FILESYSTEM_OPTION_COMPRESS = UINT32_C(1) << 2
};
#define KB_FILESYSTEM_OPTIONS_ALL                                            \
  ((kb_filesystem_options_t)(KB_FILESYSTEM_OPTION_CASEFOLD |                 \
                             KB_FILESYSTEM_OPTION_PROJID |                   \
                             KB_FILESYSTEM_OPTION_COMPRESS))

typedef struct kb_context kb_context_t;
typedef struct kb_device kb_device_t;
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
  /* Zero accepts every Fastboot USB vendor. A non-zero value filters USB
   * enumeration and selection; TCP and UDP selectors are unaffected. */
  uint64_t usb_vendor_id;
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
  /* AOSP-compatible AVB header flags. These affect only partitions whose name
   * ends in vbmeta, vbmeta_a, or vbmeta_b. */
  int32_t disable_verity;
  int32_t disable_verification;
  /* NULL selects the device's current slot. Otherwise accepts an explicit
   * slot (for example "a" or "b"), "other", or "all". The library copies
   * this UTF-8/ASCII value before an asynchronous call returns. */
  const char *slot;
  /* When one, issue set_active before executing the prepared flash tasks. */
  int32_t set_active;
  /* Optional explicit target for set_active. NULL uses slot when present,
   * otherwise the device's current slot. Requires set_active == 1. */
  const char *active_slot;
  /* AOSP -S host sparse-part limit in bytes. Zero selects automatic device
   * max-download-size behavior. Non-zero values are safely capped by both
   * the device limit and AOSP's 1 GiB resparse ceiling. */
  uint64_t sparse_limit_bytes;
  /* Permit an otherwise rejected unsafe flash/format path. */
  int32_t force;
  /* Bitwise OR of KB_FILESYSTEM_OPTION_*. Used by format operations. */
  kb_filesystem_options_t filesystem_options;
} kb_flash_options_t;

/* Android boot image construction options used with KERNEL and optional
 * RAMDISK/SECOND components. The historic name is retained for ABI stability.
 * header_version accepts 0..4. DTB is supported only by v2; v3/v4 use a fixed
 * 4096-byte layout and reject legacy address/page options that differ from the
 * initialized defaults. All strings are copied before an async call returns. */
typedef struct kb_legacy_boot_options {
  uint32_t struct_size;
  uint32_t api_version;
  const char *command_line;
  uint32_t base;
  uint32_t page_size;
  uint32_t kernel_offset;
  uint32_t ramdisk_offset;
  uint32_t second_offset;
  uint32_t tags_offset;
  uint32_t header_version;
  /* NULL means 0.0.0; otherwise MAJOR[.MINOR[.PATCH]], each in [0, 127]. */
  const char *os_version;
  /* NULL means unset; otherwise an encodable YYYY-MM-DD calendar date. */
  const char *os_patch_level;
  /* Optional UTF-8 DTB file path. Requires header_version == 2. */
  const char *dtb_path;
  /* Relative to base for v2. The initialized default is 0x01100000. */
  uint64_t dtb_offset;
} kb_legacy_boot_options_t;

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
  /* Skip the final system reboot normally appended by update/flashall. */
  int32_t skip_reboot;
  /* Do not execute flash tasks that target a secondary slot. */
  int32_t skip_secondary;
  /* Execute only flash tasks proven to target static partitions, followed by
   * the optional final reboot. */
  int32_t exclude_dynamic_partitions;
  /* Ignore fastboot-info.txt and build the frozen AOSP image-list plan. */
  int32_t disable_fastboot_info;
  /* Applied to vbmeta artifacts before any device is opened. */
  int32_t disable_verity;
  int32_t disable_verification;
  /* Global slot policy for every package flash task. NULL preserves the
   * package plan; otherwise accepts an explicit slot, "other", or "all". */
  const char *slot;
  /* When one, issue set_active on the selected session before executing the
   * prepared update/flashall task plan. */
  int32_t set_active;
  /* Optional explicit set_active target. NULL uses slot when present,
   * otherwise the device's current slot. Requires set_active == 1. */
  const char *active_slot;
  /* AOSP -S host sparse-part limit in bytes. Zero selects automatic device
   * max-download-size behavior. Non-zero values are safely capped by both
   * the device limit and AOSP's 1 GiB resparse ceiling. */
  uint64_t sparse_limit_bytes;
  /* Continue update/flashall when ordinary android-info requirements do not
   * match. Structural/package integrity and device I/O errors remain fatal. */
  int32_t force;
  /* Reserved for update plans that generate a filesystem image. */
  kb_filesystem_options_t filesystem_options;
  /* Preserve the frozen reboot-fastboot/update-super/logical-flash path even
   * when LP metadata proves a direct physical-super flash is safe. */
  int32_t disable_super_optimization;
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

#define KB_VERSION_V1_SIZE                                                   \
  ((uint32_t)(offsetof(kb_version_t, string) +                               \
              sizeof(((kb_version_t *)0)->string)))
#define KB_CONTEXT_OPTIONS_V1_SIZE                                           \
  ((uint32_t)(offsetof(kb_context_options_t, log_user_data) +                \
              sizeof(((kb_context_options_t *)0)->log_user_data)))
#define KB_CONTEXT_OPTIONS_VENDOR_ID_SIZE                                    \
  ((uint32_t)(offsetof(kb_context_options_t, usb_vendor_id) +                \
              sizeof(((kb_context_options_t *)0)->usb_vendor_id)))
#define KB_FLASH_OPTIONS_V1_SIZE                                             \
  ((uint32_t)(offsetof(kb_flash_options_t, progress_user_data) +             \
              sizeof(((kb_flash_options_t *)0)->progress_user_data)))
#define KB_FLASH_OPTIONS_AVB_FLAGS_SIZE                                      \
  ((uint32_t)(offsetof(kb_flash_options_t, disable_verification) +           \
              sizeof(((kb_flash_options_t *)0)->disable_verification)))
#define KB_FLASH_OPTIONS_SLOT_POLICY_SIZE                                    \
  ((uint32_t)(offsetof(kb_flash_options_t, active_slot) +                    \
              sizeof(((kb_flash_options_t *)0)->active_slot)))
#define KB_FLASH_OPTIONS_FORCE_FS_SIZE                                       \
  ((uint32_t)(offsetof(kb_flash_options_t, filesystem_options) +             \
              sizeof(((kb_flash_options_t *)0)->filesystem_options)))
#define KB_LEGACY_BOOT_OPTIONS_V1_SIZE                                       \
  ((uint32_t)(offsetof(kb_legacy_boot_options_t, tags_offset) +              \
              sizeof(((kb_legacy_boot_options_t *)0)->tags_offset)))
#define KB_LEGACY_BOOT_OPTIONS_MODERN_SIZE                                   \
  ((uint32_t)(offsetof(kb_legacy_boot_options_t, dtb_offset) +               \
              sizeof(((kb_legacy_boot_options_t *)0)->dtb_offset)))
#define KB_UPDATE_OPTIONS_V1_SIZE                                            \
  ((uint32_t)(offsetof(kb_update_options_t, progress_user_data) +            \
              sizeof(((kb_update_options_t *)0)->progress_user_data)))
#define KB_UPDATE_OPTIONS_AVB_FLAGS_SIZE                                     \
  ((uint32_t)(offsetof(kb_update_options_t, disable_verification) +          \
              sizeof(((kb_update_options_t *)0)->disable_verification)))
#define KB_UPDATE_OPTIONS_SLOT_POLICY_SIZE                                   \
  ((uint32_t)(offsetof(kb_update_options_t, active_slot) +                   \
              sizeof(((kb_update_options_t *)0)->active_slot)))
#define KB_UPDATE_OPTIONS_FORCE_FS_SIZE                                      \
  ((uint32_t)(offsetof(kb_update_options_t, filesystem_options) +            \
              sizeof(((kb_update_options_t *)0)->filesystem_options)))
#define KB_UPDATE_OPTIONS_SUPER_OPTIMIZATION_SIZE                            \
  ((uint32_t)sizeof(kb_update_options_t))
#define KB_COMMAND_OPTIONS_V1_SIZE                                           \
  ((uint32_t)(offsetof(kb_command_options_t, maximum_receive_bytes) +        \
              sizeof(((kb_command_options_t *)0)->maximum_receive_bytes)))
KB_API void KB_CALL kb_context_options_init(kb_context_options_t *options);
KB_API void KB_CALL kb_flash_options_init(kb_flash_options_t *options);
KB_API void KB_CALL kb_legacy_boot_options_init(
    kb_legacy_boot_options_t *options);
KB_API void KB_CALL kb_update_options_init(kb_update_options_t *options);
KB_API void KB_CALL kb_command_options_init(kb_command_options_t *options);
KB_API void KB_CALL kb_version_init(kb_version_t *version);

/* The legacy initializer symbols above initialize only their frozen v1 prefix
 * so an older, smaller caller-owned structure cannot be overrun by a newer
 * library. The source-level macros below route ordinary calls to these sized
 * forms with the current type size. Bytes beyond the library's known layout
 * are preserved. */
KB_API void KB_CALL kb_context_options_init_sized(
    kb_context_options_t *options, uint32_t struct_size);
KB_API void KB_CALL kb_flash_options_init_sized(kb_flash_options_t *options,
                                                uint32_t struct_size);
KB_API void KB_CALL kb_legacy_boot_options_init_sized(
    kb_legacy_boot_options_t *options, uint32_t struct_size);
KB_API void KB_CALL kb_update_options_init_sized(kb_update_options_t *options,
                                                 uint32_t struct_size);
KB_API void KB_CALL kb_command_options_init_sized(kb_command_options_t *options,
                                                  uint32_t struct_size);
KB_API void KB_CALL kb_version_init_sized(kb_version_t *version,
                                          uint32_t struct_size);

/* Keep existing source calls and function pointers easy and complete while
 * retaining the legacy one-argument symbols for already-built binaries. The
 * object-like aliases deliberately cover direct calls, parenthesized function
 * designators, and address-taking. Define
 * KAIROSBOOT_DISABLE_SIZED_INITIALIZER_MACROS before including this header to
 * call or take the address of the frozen-prefix symbols directly. */
#if !defined(KAIROSBOOT_BUILDING_LIBRARY) &&                                  \
    !defined(KAIROSBOOT_DISABLE_SIZED_INITIALIZER_MACROS)
static inline void KB_CALL
kb_context_options_init_current(kb_context_options_t *options) {
  kb_context_options_init_sized(options,
                                (uint32_t)sizeof(kb_context_options_t));
}
static inline void KB_CALL
kb_flash_options_init_current(kb_flash_options_t *options) {
  kb_flash_options_init_sized(options, (uint32_t)sizeof(kb_flash_options_t));
}
static inline void KB_CALL
kb_legacy_boot_options_init_current(kb_legacy_boot_options_t *options) {
  kb_legacy_boot_options_init_sized(
      options, (uint32_t)sizeof(kb_legacy_boot_options_t));
}
static inline void KB_CALL
kb_update_options_init_current(kb_update_options_t *options) {
  kb_update_options_init_sized(options, (uint32_t)sizeof(kb_update_options_t));
}
static inline void KB_CALL
kb_command_options_init_current(kb_command_options_t *options) {
  kb_command_options_init_sized(options,
                                (uint32_t)sizeof(kb_command_options_t));
}
static inline void KB_CALL kb_version_init_current(kb_version_t *version) {
  kb_version_init_sized(version, (uint32_t)sizeof(kb_version_t));
}
#define kb_context_options_init kb_context_options_init_current
#define kb_flash_options_init kb_flash_options_init_current
#define kb_legacy_boot_options_init kb_legacy_boot_options_init_current
#define kb_update_options_init kb_update_options_init_current
#define kb_command_options_init kb_command_options_init_current
#define kb_version_init kb_version_init_current
#endif

KB_API kb_status_t KB_CALL kb_get_version(kb_version_t *version);
KB_API const char *KB_CALL kb_status_string(kb_status_t status);

KB_API kb_status_t KB_CALL kb_context_create(
    const kb_context_options_t *options, kb_context_t **context,
    kb_error_t **error);
KB_API void KB_CALL kb_context_release(kb_context_t *context);

/* Opens one stable DUT handle. selector_or_null uses the grammar documented
 * below; NULL requires exactly one USB Fastboot device. USB devices are bound
 * to their physical bus/port path so later operations cannot drift to another
 * DUT with the same or a changed serial. The returned handle owns its context
 * resources and remains valid after kb_context_release(). */
KB_API kb_status_t KB_CALL kb_device_open(
    kb_context_t *context, const char *selector_or_null, kb_device_t **device,
    kb_error_t **error);
KB_API const char *KB_CALL kb_device_identifier(const kb_device_t *device);
KB_API const char *KB_CALL kb_device_serial(const kb_device_t *device);
KB_API const char *KB_CALL kb_device_usb_path(const kb_device_t *device);
/* Adds one ownership reference. Every successful retain must be paired with
 * kb_device_release(). This lets caller-owned asynchronous orchestration keep
 * one Device alive independently of another Device. */
KB_API kb_status_t KB_CALL kb_device_retain(kb_device_t *device);
KB_API void KB_CALL kb_device_release(kb_device_t *device);

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
    kb_device_t *device, const char *partition, const char *file_path,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flash_file(
    kb_device_t *device, const char *partition, const char *file_path,
    const kb_flash_options_t *options_or_null, kb_error_t **error);

/* Implements AOSP `flash vendor_boot:RAMDISK FILE` without materializing the
 * fetched partition in memory. PARTITION must be vendor_boot, vendor_boot_a,
 * or vendor_boot_b. A NULL ramdisk_name_or_null selects `default`, replacing
 * the complete v3/v4 vendor ramdisk; any other name replaces one unique v4
 * table fragment. dtb_path_or_null optionally replaces the DTB in the same
 * repack. Fetch, repack, download and flash use one selected Fastboot session.
 */
KB_API kb_status_t KB_CALL kb_flash_vendor_boot_ramdisk_async(
    kb_device_t *device, const char *partition, const char *ramdisk_name_or_null,
    const char *ramdisk_path, const char *dtb_path_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flash_vendor_boot_ramdisk(
    kb_device_t *device, const char *partition, const char *ramdisk_name_or_null,
    const char *ramdisk_path, const char *dtb_path_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error);

/* Builds the default Android boot image used by AOSP fastboot flash:raw from
 * KERNEL and optional RAMDISK/SECOND files, then flashes it to PARTITION. If
 * KERNEL is already an Android boot image, it is flashed unchanged and both
 * optional paths must be NULL. SECOND requires a non-NULL RAMDISK. */
KB_API kb_status_t KB_CALL kb_flash_raw_async(
    kb_device_t *device, const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flash_raw(
    kb_device_t *device, const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error);

/* Configured counterpart of kb_flash_raw. Construction options are used only
 * when KERNEL is a raw kernel component; prebuilt Android boot images are sent
 * unchanged and reject component files. */
KB_API kb_status_t KB_CALL kb_flash_raw_with_boot_options_async(
    kb_device_t *device, const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_legacy_boot_options_t *legacy_options_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flash_raw_with_boot_options(
    kb_device_t *device, const char *partition, const char *kernel_path,
    const char *ramdisk_path_or_null, const char *second_stage_path_or_null,
    const kb_legacy_boot_options_t *legacy_options_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error);

/* Constructs a legacy Android boot v0 image from component files and boots it
 * on the selected session. If KERNEL is a prebuilt Android boot image it is
 * sent unchanged and RAMDISK/SECOND must both be NULL. */
KB_API kb_status_t KB_CALL kb_boot_raw_async(
    kb_device_t *device, const char *kernel_path,
    const char *ramdisk_path_or_null,
    const char *second_stage_path_or_null,
    const kb_legacy_boot_options_t *legacy_options_or_null,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_boot_raw(
    kb_device_t *device, const char *kernel_path,
    const char *ramdisk_path_or_null,
    const char *second_stage_path_or_null,
    const kb_legacy_boot_options_t *legacy_options_or_null,
    const kb_flash_options_t *options_or_null, kb_error_t **error);

/* Streams one immutable file through Fastboot download and then issues boot on
 * the same selected session. The image must be non-empty and fit the protocol's
 * 32-bit download length. The blocking entry point starts the same operation
 * and waits for its terminal state. */
KB_API kb_status_t KB_CALL kb_boot_file_async(
    kb_device_t *device, const char *file_path,
    const kb_flash_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_boot_file(
    kb_device_t *device, const char *file_path,
    const kb_flash_options_t *options_or_null,
    kb_error_t **error);

/* Requires the AOSP-defined 256-byte signature file, streams it through
 * Fastboot download, then sends the exact `signature` wire command on the same
 * selected session. This transports an already-created signature blob; it
 * does not create or mutate AVB data. */
KB_API kb_status_t KB_CALL kb_signature_file_async(
    kb_device_t *device, const char *file_path,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_signature_file(
    kb_device_t *device, const char *file_path,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);

/* Performs complete package preflight before any transport open. The device
 * handle supplies the already-bound target identity.
 * USB packages may transition from bootloader Fastboot to fastbootd through a
 * fail-closed reconnect bound to the verified physical port and live device
 * identity. The blocking entry point starts the same async operation and waits
 * for its terminal state. */
KB_API kb_status_t KB_CALL kb_update_package_async(
    kb_device_t *device, const char *package_path,
    const kb_update_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_update_package(
    kb_device_t *device, const char *package_path,
    const kb_update_options_t *options_or_null,
    kb_error_t **error);

/* Resets dynamic partitions from an immutable super_empty image through the
 * existing fastbootd update-super transaction. When
 * super_empty_image_or_null is NULL, ANDROID_PRODUCT_OUT/super_empty.img is
 * used, matching AOSP Fastboot's command-line lookup rule. */
KB_API kb_status_t KB_CALL kb_wipe_super_async(
    kb_device_t *device, const char *super_empty_image_or_null,
    const kb_update_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_wipe_super(
    kb_device_t *device, const char *super_empty_image_or_null,
    const kb_update_options_t *options_or_null, kb_error_t **error);

/* kb_device_open selectors:
 *   NULL                         sole USB Fastboot device
 *   SERIAL                       exact legacy USB serial
 *   usb:serial:<percent-encoded> exact UTF-8 USB serial
 *   usb:<bus>-<port>[.<port>...] physical USB path
 *   tcp:<host>[:port]            Fastboot TCP (default port 5554)
 *   udp:<host>[:port]            Fastboot UDP (default port 5554)
 * IPv6 network hosts use brackets. Each device handle admits one active
 * protocol operation; starting another before the first reaches a terminal
 * state returns KB_E_BUSY. Use separate device handles for separate DUTs.
 * Device-scoped blocking calls start the matching async operation, wait, and
 * extract its immutable result. A successful result is owned by the caller and
 * must be released with kb_command_result_release(). */
KB_API kb_status_t KB_CALL kb_getvar_async(
    kb_device_t *device, const char *variable,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_getvar(
    kb_device_t *device, const char *variable,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_erase_async(
    kb_device_t *device, const char *partition,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_erase(
    kb_device_t *device, const char *partition,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
/* Creates an empty ext4 or f2fs Android sparse image using the matching AOSP
 * host tool, then downloads and flashes it in the same device session. A NULL
 * filesystem_type_override queries partition-type:<partition>; a zero
 * partition_size_override queries partition-size:<partition>. The generator
 * is resolved beside the process executable or on PATH. KAIROSBOOT_MKE2FS and
 * KAIROSBOOT_MAKE_F2FS may name explicit tool paths. */
KB_API kb_status_t KB_CALL kb_format_partition_async(
    kb_device_t *device, const char *partition,
    const char *filesystem_type_override_or_null,
    uint64_t partition_size_override_or_zero,
    const kb_flash_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_format_partition(
    kb_device_t *device, const char *partition,
    const char *filesystem_type_override_or_null,
    uint64_t partition_size_override_or_zero,
    const kb_flash_options_t *options_or_null, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_set_active_async(
    kb_device_t *device, const char *slot,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_set_active(
    kb_device_t *device, const char *slot,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flashing_async(
    kb_device_t *device, kb_flashing_command_t command,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_flashing(
    kb_device_t *device, kb_flashing_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_gsi_async(
    kb_device_t *device, kb_gsi_command_t command,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_gsi(
    kb_device_t *device, kb_gsi_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_snapshot_update_async(
    kb_device_t *device, kb_snapshot_update_command_t command,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_snapshot_update(
    kb_device_t *device, kb_snapshot_update_command_t command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_create_logical_partition_async(
    kb_device_t *device, const char *partition_name, uint64_t size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_create_logical_partition(
    kb_device_t *device, const char *partition_name, uint64_t size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_delete_logical_partition_async(
    kb_device_t *device, const char *partition_name,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_delete_logical_partition(
    kb_device_t *device, const char *partition_name,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_resize_logical_partition_async(
    kb_device_t *device, const char *partition_name, uint64_t size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_resize_logical_partition(
    kb_device_t *device, const char *partition_name, uint64_t size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_reboot_async(
    kb_device_t *device, kb_reboot_target_t target,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_reboot(
    kb_device_t *device, kb_reboot_target_t target,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_continue_boot_async(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_continue_boot(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_oem_async(
    kb_device_t *device, const char *command_suffix,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_oem(
    kb_device_t *device, const char *command_suffix,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_raw_command_async(
    kb_device_t *device, const char *command,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_raw_command(
    kb_device_t *device, const char *command,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_boot_async(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_boot(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_stage_async(
    kb_device_t *device, const void *data, size_t data_size,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_stage(
    kb_device_t *device, const void *data, size_t data_size,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_upload_async(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_upload(
    kb_device_t *device, const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_fetch_async(
    kb_device_t *device, const char *partition, uint64_t offset_or_unspecified,
    uint64_t size_or_unspecified,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_fetch(
    kb_device_t *device, const char *partition, uint64_t offset_or_unspecified,
    uint64_t size_or_unspecified,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);

/* Bounded, constant-memory device-to-host file operations. The destination is
 * replaced atomically only after the exact DATA payload and terminal OKAY have
 * been received and synchronized. On every failure or cancellation, an
 * existing destination is preserved. output_path is UTF-8. upload-file and
 * get-staged-file intentionally use the same AOSP "upload" wire command. */
KB_API kb_status_t KB_CALL kb_upload_file_async(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_upload_file(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_get_staged_file_async(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_operation_t **operation, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_get_staged_file(
    kb_device_t *device, const char *output_path,
    const kb_command_options_t *options_or_null,
    kb_command_result_t **result, kb_error_t **error);
KB_API kb_status_t KB_CALL kb_fetch_file_async(
    kb_device_t *device, const char *partition, uint64_t offset_or_unspecified,
    uint64_t size_or_unspecified, const char *output_path,
    const kb_command_options_t *options_or_null, kb_operation_t **operation,
    kb_error_t **error);
KB_API kb_status_t KB_CALL kb_fetch_file(
    kb_device_t *device, const char *partition, uint64_t offset_or_unspecified,
    uint64_t size_or_unspecified, const char *output_path,
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
