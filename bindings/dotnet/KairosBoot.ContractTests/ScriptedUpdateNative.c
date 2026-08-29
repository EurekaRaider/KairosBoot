#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define KB_TEST_API __declspec(dllexport)
#define KB_TEST_CALL __cdecl
#else
#define KB_TEST_API __attribute__((visibility("default")))
#define KB_TEST_CALL
#endif

enum {
  KB_OK = 0,
  KB_E_INVALID_ARGUMENT = 1,
  KB_E_BUSY = 6,
  KB_E_TIMEOUT = 7,
  KB_E_CANCELLED = 8,
  KB_E_IO = 9,
};

typedef struct kb_progress {
  uint32_t struct_size;
  uint32_t api_version;
  uint64_t bytes_completed;
  uint64_t bytes_total;
  const char *stage;
  const char *device_identifier;
} kb_progress_t;

typedef int32_t(KB_TEST_CALL *kb_progress_callback_t)(
    const kb_progress_t *progress, void *user_data);

typedef struct kb_context_options {
  uint32_t struct_size;
  uint32_t api_version;
  void *log_callback;
  void *log_user_data;
  uint64_t usb_vendor_id;
} kb_context_options_t;

typedef struct kb_device {
  char *identifier;
} kb_device_t;

typedef struct kb_update_options {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t timeout_ms;
  int32_t wipe;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
  int32_t skip_reboot;
  int32_t skip_secondary;
  int32_t exclude_dynamic_partitions;
  int32_t disable_fastboot_info;
  int32_t disable_verity;
  int32_t disable_verification;
  const char *slot;
  int32_t set_active;
  const char *active_slot;
  uint64_t sparse_limit_bytes;
  int32_t force;
  uint32_t filesystem_options;
  int32_t disable_super_optimization;
} kb_update_options_t;

typedef struct kb_flash_options {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
  int32_t disable_verity;
  int32_t disable_verification;
  const char *slot;
  int32_t set_active;
  const char *active_slot;
  uint64_t sparse_limit_bytes;
  int32_t force;
  uint32_t filesystem_options;
} kb_flash_options_t;

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
  const char *os_version;
  const char *os_patch_level;
  const char *dtb_path;
  uint64_t dtb_offset;
} kb_legacy_boot_options_t;

typedef struct kb_operation {
  int32_t kind;
  int32_t polls;
  int32_t cancelled;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
  char *device_identifier;
} kb_operation_t;


typedef struct kb_command_options {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
  uint64_t maximum_receive_bytes;
} kb_command_options_t;

typedef struct kb_version {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  const char *string;
} kb_version_t;



typedef struct kb_error {
  int32_t status;
  const char *message;
} kb_error_t;

KB_TEST_API void KB_TEST_CALL kb_context_options_init_sized(
    kb_context_options_t *options, uint32_t struct_size);
KB_TEST_API void KB_TEST_CALL kb_update_options_init_sized(
    kb_update_options_t *options, uint32_t struct_size);
KB_TEST_API void KB_TEST_CALL kb_flash_options_init_sized(
    kb_flash_options_t *options, uint32_t struct_size);
KB_TEST_API void KB_TEST_CALL kb_legacy_boot_options_init_sized(
    kb_legacy_boot_options_t *options, uint32_t struct_size);
KB_TEST_API void KB_TEST_CALL kb_command_options_init_sized(
    kb_command_options_t *options, uint32_t struct_size);
KB_TEST_API void KB_TEST_CALL kb_version_init_sized(
    kb_version_t *version, uint32_t struct_size);

static int32_t failure_code;
static int32_t options_init_count;
static int32_t async_start_count;
static int32_t blocking_count;
static int32_t flash_options_init_count;
static int32_t legacy_boot_options_init_count;
static int32_t flash_raw_async_start_count;
static int32_t flash_raw_blocking_count;
static int32_t boot_async_start_count;
static int32_t boot_blocking_count;
static int32_t cancel_count;
static int32_t operation_release_count;
static int32_t context_release_count;
static int32_t device_release_count;
static int32_t context_sentinel;

static const char expected_unicode_package[] = {
    'i', 'm', 'a', 'g', 'e', 's', '/', (char)0xe5, (char)0x8d,
    (char)0x87, (char)0xe7, (char)0xba, (char)0xa7, '.', 'z', 'i', 'p', 0};

static const char expected_unicode_kernel[] = {
    'i', 'm', 'a', 'g', 'e', 's', '/', (char)0xe5, (char)0x86,
    (char)0x85, (char)0xe6, (char)0xa0, (char)0xb8, '.', 'b', 'i', 'n', 0};

static const char expected_unicode_boot_image[] = {
    'i', 'm', 'a', 'g', 'e', 's', '/', (char)0xe5, (char)0x90,
    (char)0xaf, (char)0xe5, (char)0x8a, (char)0xa8, '.', 'i', 'm', 'g', 0};

static const char expected_unicode_command_line[] = {
    'c', 'o', 'n', 's', 'o', 'l', 'e', '=', (char)0xe5, (char)0x90,
    (char)0xaf, (char)0xe5, (char)0x8a, (char)0xa8, 0};

static int same_string(const char *actual, const char *expected) {
  return actual != NULL && strcmp(actual, expected) == 0;
}

static const char *device_identifier(const kb_device_t *device) {
  if (device == NULL || device == (const kb_device_t *)(uintptr_t)1 ||
      device->identifier == NULL) {
    return "";
  }
  return device->identifier;
}

static size_t initialize_known_prefix(void *value,
                                      uint32_t struct_size,
                                      size_t known_size) {
  const size_t writable =
      (size_t)struct_size < known_size ? (size_t)struct_size : known_size;
  if (value != NULL) {
    memset(value, 0, writable);
  }
  return writable;
}

static void initialize_u32_field(void *value,
                                 size_t writable,
                                 size_t offset,
                                 uint32_t field) {
  if (value != NULL && offset <= writable && sizeof(field) <= writable - offset) {
    memcpy((unsigned char *)value + offset, &field, sizeof(field));
  }
}

static void initialize_u64_field(void *value,
                                 size_t writable,
                                 size_t offset,
                                 uint64_t field) {
  if (value != NULL && offset <= writable && sizeof(field) <= writable - offset) {
    memcpy((unsigned char *)value + offset, &field, sizeof(field));
  }
}

static int valid_options(const kb_update_options_t *options) {
  return options != NULL &&
         options->struct_size == (uint32_t)sizeof(*options) &&
         options->api_version == 1 &&
         (options->wipe == 0 || options->wipe == 1) &&
         (options->skip_reboot == 0 || options->skip_reboot == 1) &&
         (options->skip_secondary == 0 || options->skip_secondary == 1) &&
         (options->exclude_dynamic_partitions == 0 ||
          options->exclude_dynamic_partitions == 1) &&
         (options->disable_fastboot_info == 0 ||
          options->disable_fastboot_info == 1) &&
         (options->force == 0 || options->force == 1) &&
         (options->filesystem_options & ~UINT32_C(7)) == 0U &&
         (options->disable_super_optimization == 0 ||
          options->disable_super_optimization == 1) &&
         ((options->progress_callback == NULL &&
           options->progress_user_data == NULL) ||
          (options->progress_callback != NULL &&
           options->progress_user_data != NULL));
}

static int valid_flash_options(const kb_flash_options_t *options) {
  return options != NULL &&
         options->struct_size == (uint32_t)sizeof(*options) &&
         options->api_version == 1 &&
         (options->force == 0 || options->force == 1) &&
         (options->filesystem_options & ~UINT32_C(7)) == 0U &&
         ((options->progress_callback == NULL &&
           options->progress_user_data == NULL) ||
          (options->progress_callback != NULL &&
           options->progress_user_data != NULL));
}

static int valid_legacy_boot_options(
    const kb_legacy_boot_options_t *options) {
  return options != NULL &&
         options->struct_size == (uint32_t)sizeof(*options) &&
         options->api_version == 1;
}

static int valid_default_legacy_boot_options(
    const kb_legacy_boot_options_t *options) {
  return valid_legacy_boot_options(options) && options->command_line == NULL &&
         options->base == 0x10000000U && options->page_size == 2048U &&
         options->kernel_offset == 0x00008000U &&
         options->ramdisk_offset == 0x01000000U &&
         options->second_offset == 0x00f00000U &&
         options->tags_offset == 0x00000100U &&
         options->header_version == 0U && options->os_version == NULL &&
         options->os_patch_level == NULL && options->dtb_path == NULL &&
         options->dtb_offset == 0x01100000ULL;
}

static int valid_custom_legacy_boot_layout(
    const kb_legacy_boot_options_t *options) {
  return valid_legacy_boot_options(options) &&
         options->base == 0x12000000U && options->page_size == 4096U &&
         options->kernel_offset == 0x00010000U &&
         options->ramdisk_offset == 0x02000000U &&
         options->second_offset == 0x01f00000U &&
         options->tags_offset == 0x00000200U;
}

static int valid_custom_legacy_boot_options(
    const kb_legacy_boot_options_t *options) {
  return valid_custom_legacy_boot_layout(options) &&
         same_string(options->command_line, expected_unicode_command_line) &&
         options->header_version == 2U &&
         same_string(options->os_version, "15.0.1") &&
         same_string(options->os_patch_level, "2025-02-05") &&
         same_string(options->dtb_path, "board.dtb") &&
         options->dtb_offset == 0x01200000ULL;
}

static void record_failure(const int32_t code) {
  if (failure_code == 0) {
    failure_code = code;
  }
}

static void emit_progress(kb_operation_t *operation, const char *stage,
                          const uint64_t completed, const uint64_t total) {
  kb_progress_t progress;
  memset(&progress, 0, sizeof(progress));
  progress.struct_size = (uint32_t)sizeof(progress);
  progress.api_version = 1;
  progress.bytes_completed = completed;
  progress.bytes_total = total;
  progress.stage = stage;
  progress.device_identifier = operation->device_identifier;
  if (operation->progress_callback(&progress, operation->progress_user_data) !=
      0) {
    operation->cancelled = 1;
  }
}

KB_TEST_API void KB_TEST_CALL kb_test_reset(void) {
  failure_code = 0;
  options_init_count = 0;
  async_start_count = 0;
  blocking_count = 0;
  flash_options_init_count = 0;
  legacy_boot_options_init_count = 0;
  flash_raw_async_start_count = 0;
  flash_raw_blocking_count = 0;
  boot_async_start_count = 0;
  boot_blocking_count = 0;
  cancel_count = 0;
  operation_release_count = 0;
  context_release_count = 0;
  device_release_count = 0;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_failure_code(void) {
  return failure_code;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_options_init_count(void) {
  return options_init_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_async_start_count(void) {
  return async_start_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_blocking_count(void) {
  return blocking_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_flash_options_init_count(void) {
  return flash_options_init_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_legacy_boot_options_init_count(void) {
  return legacy_boot_options_init_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_flash_raw_async_start_count(void) {
  return flash_raw_async_start_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_flash_raw_blocking_count(void) {
  return flash_raw_blocking_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_boot_async_start_count(void) {
  return boot_async_start_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_boot_blocking_count(void) {
  return boot_blocking_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_cancel_count(void) {
  return cancel_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_operation_release_count(void) {
  return operation_release_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_context_release_count(void) {
  return context_release_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_device_release_count(void) {
  return device_release_count;
}

KB_TEST_API const char *KB_TEST_CALL kb_status_string(int32_t status) {
  (void)status;
  return "scripted update native validation failed";
}

KB_TEST_API void KB_TEST_CALL kb_context_options_init(
    kb_context_options_t *options) {
  kb_context_options_init_sized(
      options,
      (uint32_t)(offsetof(kb_context_options_t, log_user_data) +
                 sizeof(options->log_user_data)));
}

KB_TEST_API void KB_TEST_CALL kb_context_options_init_sized(
    kb_context_options_t *options, uint32_t struct_size) {
  if (options == NULL) {
    record_failure(9);
    return;
  }
  const size_t writable =
      initialize_known_prefix(options, struct_size, sizeof(*options));
  initialize_u32_field(options, writable,
                       offsetof(kb_context_options_t, struct_size),
                       struct_size);
  initialize_u32_field(options, writable,
                       offsetof(kb_context_options_t, api_version), 1);
}

KB_TEST_API int32_t KB_TEST_CALL kb_context_create(
    const void *options, void **context, void **error) {
  if (context == NULL || error == NULL) {
    record_failure(10);
    return KB_E_INVALID_ARGUMENT;
  }
  if (options != NULL) {
    const kb_context_options_t *typed =
        (const kb_context_options_t *)options;
    if (typed->struct_size != (uint32_t)sizeof(*typed) ||
        typed->api_version != 1 || typed->usb_vendor_id != 0x18D1U) {
      record_failure(10);
      return KB_E_INVALID_ARGUMENT;
    }
  }
  *context = &context_sentinel;
  *error = NULL;
  return KB_OK;
}

KB_TEST_API void KB_TEST_CALL kb_context_release(void *context) {
  if (context != &context_sentinel && context != (void *)(uintptr_t)1) {
    record_failure(11);
  }
  ++context_release_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_device_open(
    void *context, const char *selector, kb_device_t **device, void **error) {
  if (context != &context_sentinel || device == NULL || error == NULL) {
    record_failure(12);
    return KB_E_INVALID_ARGUMENT;
  }
  kb_device_t *created = (kb_device_t *)calloc(1, sizeof(*created));
  if (created == NULL) {
    return KB_E_INVALID_ARGUMENT;
  }
  const char *identifier = selector == NULL ? "" : selector;
  const size_t size = strlen(identifier) + 1U;
  created->identifier = (char *)malloc(size);
  if (created->identifier == NULL) {
    free(created);
    return KB_E_INVALID_ARGUMENT;
  }
  memcpy(created->identifier, identifier, size);
  *device = created;
  *error = NULL;
  return KB_OK;
}

KB_TEST_API const char *KB_TEST_CALL kb_device_identifier(
    const kb_device_t *device) {
  return device_identifier(device);
}

KB_TEST_API const char *KB_TEST_CALL kb_device_serial(
    const kb_device_t *device) {
  (void)device;
  return "";
}

KB_TEST_API const char *KB_TEST_CALL kb_device_usb_path(
    const kb_device_t *device) {
  (void)device;
  return "";
}

KB_TEST_API void KB_TEST_CALL kb_device_release(kb_device_t *device) {
  if (device == NULL || device == (kb_device_t *)(uintptr_t)1) {
    return;
  }
  ++device_release_count;
  free(device->identifier);
  free(device);
}

KB_TEST_API void KB_TEST_CALL kb_update_options_init(
    kb_update_options_t *options) {
  kb_update_options_init_sized(
      options,
      (uint32_t)(offsetof(kb_update_options_t, progress_user_data) +
                 sizeof(options->progress_user_data)));
}

KB_TEST_API void KB_TEST_CALL kb_update_options_init_sized(
    kb_update_options_t *options, uint32_t struct_size) {
  if (options == NULL) {
    record_failure(20);
    return;
  }
  const size_t writable =
      initialize_known_prefix(options, struct_size, sizeof(*options));
  initialize_u32_field(options, writable,
                       offsetof(kb_update_options_t, struct_size),
                       struct_size);
  initialize_u32_field(options, writable,
                       offsetof(kb_update_options_t, api_version), 1);
  initialize_u32_field(options, writable,
                       offsetof(kb_update_options_t, timeout_ms), UINT32_MAX);
  ++options_init_count;
}

KB_TEST_API void KB_TEST_CALL kb_flash_options_init(
    kb_flash_options_t *options) {
  kb_flash_options_init_sized(
      options,
      (uint32_t)(offsetof(kb_flash_options_t, progress_user_data) +
                 sizeof(options->progress_user_data)));
}

KB_TEST_API void KB_TEST_CALL kb_flash_options_init_sized(
    kb_flash_options_t *options, uint32_t struct_size) {
  if (options == NULL) {
    record_failure(21);
    return;
  }
  const size_t writable =
      initialize_known_prefix(options, struct_size, sizeof(*options));
  initialize_u32_field(options, writable,
                       offsetof(kb_flash_options_t, struct_size), struct_size);
  initialize_u32_field(options, writable,
                       offsetof(kb_flash_options_t, api_version), 1);
  initialize_u32_field(options, writable,
                       offsetof(kb_flash_options_t, timeout_ms), UINT32_MAX);
  ++flash_options_init_count;
}

KB_TEST_API void KB_TEST_CALL kb_legacy_boot_options_init(
    kb_legacy_boot_options_t *options) {
  kb_legacy_boot_options_init_sized(
      options,
      (uint32_t)(offsetof(kb_legacy_boot_options_t, tags_offset) +
                 sizeof(options->tags_offset)));
}

KB_TEST_API void KB_TEST_CALL kb_legacy_boot_options_init_sized(
    kb_legacy_boot_options_t *options, uint32_t struct_size) {
  if (options == NULL) {
    record_failure(22);
    return;
  }
  const size_t writable =
      initialize_known_prefix(options, struct_size, sizeof(*options));
  initialize_u32_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, struct_size),
                       struct_size);
  initialize_u32_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, api_version), 1);
  initialize_u32_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, base), 0x10000000U);
  initialize_u32_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, page_size), 2048U);
  initialize_u32_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, kernel_offset),
                       0x00008000U);
  initialize_u32_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, ramdisk_offset),
                       0x01000000U);
  initialize_u32_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, second_offset),
                       0x00f00000U);
  initialize_u32_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, tags_offset),
                       0x00000100U);
  initialize_u64_field(options, writable,
                       offsetof(kb_legacy_boot_options_t, dtb_offset),
                       UINT64_C(0x01100000));
  ++legacy_boot_options_init_count;
}

KB_TEST_API void KB_TEST_CALL kb_command_options_init(
    kb_command_options_t *options) {
  kb_command_options_init_sized(options, (uint32_t)sizeof(*options));
}

KB_TEST_API void KB_TEST_CALL kb_command_options_init_sized(
    kb_command_options_t *options, uint32_t struct_size) {
  const size_t writable =
      initialize_known_prefix(options, struct_size, sizeof(*options));
  initialize_u32_field(options, writable,
                       offsetof(kb_command_options_t, struct_size), struct_size);
  initialize_u32_field(options, writable,
                       offsetof(kb_command_options_t, api_version), 1);
  initialize_u32_field(options, writable,
                       offsetof(kb_command_options_t, timeout_ms), UINT32_MAX);
  initialize_u64_field(
      options, writable,
      offsetof(kb_command_options_t, maximum_receive_bytes),
      UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024));
}

KB_TEST_API void KB_TEST_CALL kb_version_init(kb_version_t *version) {
  kb_version_init_sized(version, (uint32_t)sizeof(*version));
}

KB_TEST_API void KB_TEST_CALL kb_version_init_sized(kb_version_t *version,
                                                     uint32_t struct_size) {
  const size_t writable =
      initialize_known_prefix(version, struct_size, sizeof(*version));
  initialize_u32_field(version, writable, offsetof(kb_version_t, struct_size),
                       struct_size);
  initialize_u32_field(version, writable, offsetof(kb_version_t, api_version),
                       1);
}

KB_TEST_API int32_t KB_TEST_CALL kb_update_package_async(
    kb_device_t *device, const char *package_path,
    const kb_update_options_t *options, kb_operation_t **operation,
    void **error) {
  if (device == NULL || operation == NULL || error == NULL ||
      !valid_options(options)) {
    record_failure(30);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(package_path, expected_unicode_package)) {
    kind = 1;
    if (!same_string(device_identifier(device), "usb:serial:device") ||
        options->timeout_ms != 2 || options->wipe != 1) {
      record_failure(31);
      return KB_E_INVALID_ARGUMENT;
    }
    if (options->skip_reboot != 1 || options->skip_secondary != 1 ||
        options->exclude_dynamic_partitions != 1 ||
        options->disable_fastboot_info != 1 ||
        options->sparse_limit_bytes != 8U * 1024U * 1024U ||
        options->force != 1 || options->filesystem_options != 3U ||
        options->disable_super_optimization != 1) {
      record_failure(37);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(package_path, "cancel.zip")) {
    kind = 2;
    if (!same_string(device_identifier(device), "tcp:127.0.0.1:5554") ||
        options->timeout_ms != UINT32_MAX || options->wipe != 0) {
      record_failure(32);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(package_path, "default.zip")) {
    kind = 3;
    if (device_identifier(device)[0] != '\0' ||
        options->timeout_ms != UINT32_MAX ||
        options->wipe != 0 || options->progress_callback != NULL ||
        options->progress_user_data != NULL || options->skip_reboot != 0 ||
        options->skip_secondary != 0 ||
        options->exclude_dynamic_partitions != 0 ||
        options->disable_fastboot_info != 0 ||
        options->sparse_limit_bytes != 0 || options->force != 0 ||
        options->filesystem_options != 0U ||
        options->disable_super_optimization != 0) {
      record_failure(36);
      return KB_E_INVALID_ARGUMENT;
    }
  } else {
    record_failure(33);
    return KB_E_INVALID_ARGUMENT;
  }

  kb_operation_t *created = (kb_operation_t *)calloc(1, sizeof(*created));
  if (created == NULL) {
    record_failure(34);
    return KB_E_INVALID_ARGUMENT;
  }
  created->kind = kind;
  created->progress_callback = options->progress_callback;
  created->progress_user_data = options->progress_user_data;
  const char *identifier = device_identifier(device);
  const size_t identifier_size = strlen(identifier) + 1;
  created->device_identifier = (char *)malloc(identifier_size);
  if (created->device_identifier == NULL) {
    free(created);
    record_failure(35);
    return KB_E_INVALID_ARGUMENT;
  }
  memcpy(created->device_identifier, identifier, identifier_size);
  *operation = created;
  *error = NULL;
  ++async_start_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_update_package(
    kb_device_t *device, const char *package_path,
    const kb_update_options_t *options, void **error) {
  if (device != (kb_device_t *)(uintptr_t)1 || error == NULL || options == NULL ||
      options->struct_size != (uint32_t)sizeof(*options) ||
      options->api_version != 1 ||
      options->timeout_ms != 17 || options->wipe != 1 ||
      options->disable_super_optimization != 0 ||
      options->progress_callback != NULL || options->progress_user_data != NULL ||
      !same_string(package_path, "blocking.zip")) {
    record_failure(40);
    return KB_E_INVALID_ARGUMENT;
  }
  *error = NULL;
  ++blocking_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_flash_raw_async(
    kb_device_t *device, const char *partition,
    const char *kernel_path, const char *ramdisk_path,
    const char *second_stage_path, const kb_flash_options_t *options,
    kb_operation_t **operation, void **error) {
  if (device == NULL || operation == NULL || error == NULL ||
      !valid_flash_options(options) || !same_string(partition, "boot")) {
    record_failure(41);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(kernel_path, expected_unicode_kernel)) {
    kind = 4;
    if (!same_string(device_identifier(device), "usb:serial:raw") ||
        !same_string(ramdisk_path, "ramdisk.img") ||
        second_stage_path != NULL || options->timeout_ms != 2 ||
        options->sparse_limit_bytes != 64U * 1024U ||
        options->force != 1 || options->filesystem_options != 7U ||
        options->progress_callback == NULL) {
      record_failure(42);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(kernel_path, "cancel-kernel.bin")) {
    kind = 5;
    if (!same_string(device_identifier(device), "tcp:127.0.0.1:5554") ||
        ramdisk_path != NULL || second_stage_path != NULL ||
        options->timeout_ms != UINT32_MAX ||
        options->progress_callback == NULL) {
      record_failure(43);
      return KB_E_INVALID_ARGUMENT;
    }
  } else {
    record_failure(44);
    return KB_E_INVALID_ARGUMENT;
  }

  kb_operation_t *created = (kb_operation_t *)calloc(1, sizeof(*created));
  if (created == NULL) {
    record_failure(45);
    return KB_E_INVALID_ARGUMENT;
  }
  created->kind = kind;
  created->progress_callback = options->progress_callback;
  created->progress_user_data = options->progress_user_data;
  const size_t identifier_size = strlen(device_identifier(device)) + 1;
  created->device_identifier = (char *)malloc(identifier_size);
  if (created->device_identifier == NULL) {
    free(created);
    record_failure(46);
    return KB_E_INVALID_ARGUMENT;
  }
  memcpy(created->device_identifier, device_identifier(device), identifier_size);
  *operation = created;
  *error = NULL;
  ++flash_raw_async_start_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_flash_raw(
    kb_device_t *device, const char *partition,
    const char *kernel_path, const char *ramdisk_path,
    const char *second_stage_path, const kb_flash_options_t *options,
    void **error) {
  if (device != (kb_device_t *)(uintptr_t)1 || error == NULL ||
      !valid_flash_options(options) || options->timeout_ms != 17 ||
      options->progress_callback != NULL ||
      !same_string(partition, "boot") ||
      !same_string(kernel_path, "blocking-kernel.bin") ||
      ramdisk_path != NULL || second_stage_path != NULL) {
    record_failure(47);
    return KB_E_INVALID_ARGUMENT;
  }
  *error = NULL;
  ++flash_raw_blocking_count;
  return KB_OK;
}

static int32_t start_legacy_operation(
    const char *device_selector, const int32_t kind,
    const kb_flash_options_t *options, kb_operation_t **operation,
    void **error, const int32_t allocation_failure) {
  kb_operation_t *created = (kb_operation_t *)calloc(1, sizeof(*created));
  if (created == NULL) {
    record_failure(allocation_failure);
    return KB_E_INVALID_ARGUMENT;
  }
  created->kind = kind;
  created->progress_callback = options->progress_callback;
  created->progress_user_data = options->progress_user_data;
  const char *identifier = device_selector == NULL ? "" : device_selector;
  const size_t identifier_size = strlen(identifier) + 1;
  created->device_identifier = (char *)malloc(identifier_size);
  if (created->device_identifier == NULL) {
    free(created);
    record_failure(allocation_failure);
    return KB_E_INVALID_ARGUMENT;
  }
  memcpy(created->device_identifier, identifier, identifier_size);
  *operation = created;
  *error = NULL;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_flash_raw_with_boot_options_async(
    kb_device_t *device, const char *partition,
    const char *kernel_path, const char *ramdisk_path,
    const char *second_stage_path,
    const kb_legacy_boot_options_t *legacy_options,
    const kb_flash_options_t *options, kb_operation_t **operation,
    void **error) {
  if (device == NULL || operation == NULL || error == NULL ||
      !valid_flash_options(options) || !same_string(partition, "boot")) {
    record_failure(110);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(kernel_path, "configured-kernel.bin")) {
    kind = 4;
    if (!same_string(device_identifier(device), "usb:serial:configured-raw") ||
        !same_string(ramdisk_path, "configured-ramdisk.img") ||
        !same_string(second_stage_path, "configured-second.bin") ||
        !valid_custom_legacy_boot_options(legacy_options) ||
        options->timeout_ms != 2 || options->progress_callback == NULL) {
      record_failure(111);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(kernel_path, "cancel-configured-kernel.bin")) {
    kind = 5;
    if (!same_string(device_identifier(device), "tcp:127.0.0.1:5554") ||
        ramdisk_path != NULL || second_stage_path != NULL ||
        !valid_default_legacy_boot_options(legacy_options) ||
        options->timeout_ms != UINT32_MAX || options->progress_callback == NULL) {
      record_failure(112);
      return KB_E_INVALID_ARGUMENT;
    }
  } else {
    record_failure(113);
    return KB_E_INVALID_ARGUMENT;
  }

  const int32_t status = start_legacy_operation(
      device_identifier(device), kind, options, operation, error, 114);
  if (status == KB_OK) {
    ++flash_raw_async_start_count;
  }
  return status;
}

KB_TEST_API int32_t KB_TEST_CALL kb_flash_raw_with_boot_options(
    kb_device_t *device, const char *partition,
    const char *kernel_path, const char *ramdisk_path,
    const char *second_stage_path,
    const kb_legacy_boot_options_t *legacy_options,
    const kb_flash_options_t *options, void **error) {
  if (device != (kb_device_t *)(uintptr_t)1 || error == NULL ||
      !valid_flash_options(options) || options->timeout_ms != 17 ||
      options->progress_callback != NULL ||
      !same_string(partition, "boot") ||
      !same_string(kernel_path, "blocking-configured-kernel.bin") ||
      ramdisk_path != NULL || second_stage_path != NULL ||
      !valid_custom_legacy_boot_layout(legacy_options) ||
      !same_string(legacy_options->command_line, "console=blocking")) {
    record_failure(115);
    return KB_E_INVALID_ARGUMENT;
  }
  *error = NULL;
  ++flash_raw_blocking_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_boot_raw_async(
    kb_device_t *device, const char *kernel_path,
    const char *ramdisk_path, const char *second_stage_path,
    const kb_legacy_boot_options_t *legacy_options,
    const kb_flash_options_t *options, kb_operation_t **operation,
    void **error) {
  if (device == NULL || operation == NULL || error == NULL ||
      !valid_flash_options(options)) {
    record_failure(120);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(kernel_path, "boot-raw-kernel.bin")) {
    kind = 1;
    if (!same_string(device_identifier(device), "usb:serial:boot-raw") ||
        !same_string(ramdisk_path, "boot-raw-ramdisk.img") ||
        !same_string(second_stage_path, "boot-raw-second.bin") ||
        !valid_custom_legacy_boot_options(legacy_options) ||
        options->timeout_ms != 2 || options->progress_callback == NULL) {
      record_failure(121);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(kernel_path, "cancel-boot-raw-kernel.bin")) {
    kind = 2;
    if (!same_string(device_identifier(device), "tcp:127.0.0.1:5554") ||
        ramdisk_path != NULL || second_stage_path != NULL ||
        !valid_default_legacy_boot_options(legacy_options) ||
        options->timeout_ms != UINT32_MAX || options->progress_callback == NULL) {
      record_failure(122);
      return KB_E_INVALID_ARGUMENT;
    }
  } else {
    record_failure(123);
    return KB_E_INVALID_ARGUMENT;
  }

  const int32_t status = start_legacy_operation(
      device_identifier(device), kind, options, operation, error, 124);
  if (status == KB_OK) {
    ++boot_async_start_count;
  }
  return status;
}

KB_TEST_API int32_t KB_TEST_CALL kb_boot_raw(
    kb_device_t *device, const char *kernel_path,
    const char *ramdisk_path, const char *second_stage_path,
    const kb_legacy_boot_options_t *legacy_options,
    const kb_flash_options_t *options, void **error) {
  if (device != (kb_device_t *)(uintptr_t)1 || error == NULL ||
      !valid_flash_options(options) || options->timeout_ms != 17 ||
      options->progress_callback != NULL ||
      !same_string(kernel_path, "blocking-boot-raw-kernel.bin") ||
      ramdisk_path != NULL || second_stage_path != NULL ||
      !valid_custom_legacy_boot_layout(legacy_options) ||
      !same_string(legacy_options->command_line, "console=blocking")) {
    record_failure(125);
    return KB_E_INVALID_ARGUMENT;
  }
  *error = NULL;
  ++boot_blocking_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_boot_file_async(
    kb_device_t *device, const char *file_path,
    const kb_flash_options_t *options, kb_operation_t **operation,
    void **error) {
  if (device == NULL || operation == NULL || error == NULL ||
      !valid_flash_options(options)) {
    record_failure(100);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(file_path, expected_unicode_boot_image)) {
    kind = 1;
    if (!same_string(device_identifier(device), "usb:serial:boot-device") ||
        options->timeout_ms != 2 || options->progress_callback == NULL) {
      record_failure(101);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(file_path, "cancel-boot.img")) {
    kind = 2;
    if (!same_string(device_identifier(device), "tcp:127.0.0.1:5554") ||
        options->timeout_ms != UINT32_MAX ||
        options->progress_callback == NULL) {
      record_failure(102);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(file_path, "default-boot.img")) {
    kind = 3;
    if (device_identifier(device)[0] != '\0' ||
        options->timeout_ms != UINT32_MAX ||
        options->progress_callback != NULL ||
        options->progress_user_data != NULL) {
      record_failure(103);
      return KB_E_INVALID_ARGUMENT;
    }
  } else {
    record_failure(104);
    return KB_E_INVALID_ARGUMENT;
  }

  kb_operation_t *created = (kb_operation_t *)calloc(1, sizeof(*created));
  if (created == NULL) {
    record_failure(105);
    return KB_E_INVALID_ARGUMENT;
  }
  created->kind = kind;
  created->progress_callback = options->progress_callback;
  created->progress_user_data = options->progress_user_data;
  const char *identifier = device_identifier(device);
  const size_t identifier_size = strlen(identifier) + 1;
  created->device_identifier = (char *)malloc(identifier_size);
  if (created->device_identifier == NULL) {
    free(created);
    record_failure(106);
    return KB_E_INVALID_ARGUMENT;
  }
  memcpy(created->device_identifier, identifier, identifier_size);
  *operation = created;
  *error = NULL;
  ++boot_async_start_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_boot_file(
    kb_device_t *device, const char *file_path,
    const kb_flash_options_t *options, void **error) {
  if (device != (kb_device_t *)(uintptr_t)1 || error == NULL ||
      !valid_flash_options(options) || options->timeout_ms != 17 ||
      options->progress_callback != NULL ||
      options->progress_user_data != NULL ||
      !same_string(file_path, "blocking-boot.img")) {
    record_failure(107);
    return KB_E_INVALID_ARGUMENT;
  }
  *error = NULL;
  ++boot_blocking_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_operation_wait(kb_operation_t *operation,
                                                   uint32_t timeout_ms) {
  if (operation == NULL || timeout_ms != 0) {
    record_failure(50);
    return KB_E_INVALID_ARGUMENT;
  }

  if (operation->polls == 0) {
    if (operation->kind == 3) {
      ++operation->polls;
      return KB_OK;
    }
    if (operation->kind == 4 || operation->kind == 5) {
      emit_progress(operation, "download", 0, 4);
    } else {
      emit_progress(operation, "preflight", 0, 0);
    }
    ++operation->polls;
    if (operation->cancelled) {
      return KB_E_CANCELLED;
    }
    return KB_E_TIMEOUT;
  }

  if (operation->cancelled) {
    return KB_E_CANCELLED;
  }

  if (operation->kind == 2 || operation->kind == 5) {
    return KB_E_TIMEOUT;
  }

  if (operation->polls == 1) {
    emit_progress(operation, "download", 2, 4);
    ++operation->polls;
    return KB_E_TIMEOUT;
  }

  emit_progress(operation, "complete", 4, 4);
  ++operation->polls;
  return operation->cancelled ? KB_E_CANCELLED : KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_operation_cancel(
    kb_operation_t *operation) {
  if (operation == NULL) {
    record_failure(60);
    return KB_E_INVALID_ARGUMENT;
  }
  if (!operation->cancelled) {
    operation->cancelled = 1;
    ++cancel_count;
  }
  return KB_OK;
}

KB_TEST_API void KB_TEST_CALL kb_operation_release(kb_operation_t *operation) {
  if (operation == NULL) {
    record_failure(70);
    return;
  }
  ++operation_release_count;
  free(operation->device_identifier);
  free(operation);
}

KB_TEST_API int32_t KB_TEST_CALL kb_error_status(const kb_error_t *error) {
  return error == NULL ? KB_E_INVALID_ARGUMENT : error->status;
}

KB_TEST_API const char *KB_TEST_CALL kb_error_message(const kb_error_t *error) {
  return error == NULL ? "" : error->message;
}

KB_TEST_API const char *KB_TEST_CALL kb_error_device_identifier(
    const kb_error_t *error) {
  (void)error;
  return "SERIAL-FAIL";
}

KB_TEST_API int32_t KB_TEST_CALL kb_error_native_code(const kb_error_t *error) {
  (void)error;
  return -55;
}

KB_TEST_API int32_t KB_TEST_CALL kb_error_transfer_state(
    const kb_error_t *error) {
  (void)error;
  return 0;
}

KB_TEST_API const char *KB_TEST_CALL kb_error_device_message(
    const kb_error_t *error, size_t *size) {
  (void)error;
  if (size != NULL) {
    *size = 0;
  }
  return NULL;
}

KB_TEST_API size_t KB_TEST_CALL kb_error_command_message_count(
    const kb_error_t *error) {
  (void)error;
  return 0;
}

KB_TEST_API int32_t KB_TEST_CALL kb_error_command_message_kind(
    const kb_error_t *error, size_t index) {
  (void)error;
  (void)index;
  return 0;
}

KB_TEST_API const char *KB_TEST_CALL kb_error_command_message_payload(
    const kb_error_t *error, size_t index, size_t *size) {
  (void)error;
  (void)index;
  if (size != NULL) {
    *size = 0;
  }
  return NULL;
}

KB_TEST_API uint64_t KB_TEST_CALL kb_error_inbound_expected_bytes(
    const kb_error_t *error) {
  (void)error;
  return UINT64_MAX;
}

KB_TEST_API uint64_t KB_TEST_CALL kb_error_inbound_transferred_bytes(
    const kb_error_t *error) {
  (void)error;
  return 0;
}

KB_TEST_API int32_t KB_TEST_CALL kb_error_inbound_transfer_state(
    const kb_error_t *error) {
  (void)error;
  return 0;
}

KB_TEST_API int32_t KB_TEST_CALL kb_error_session_poisoned(
    const kb_error_t *error) {
  (void)error;
  return 0;
}

KB_TEST_API void KB_TEST_CALL kb_error_release(kb_error_t *error) {
  (void)error;
}
