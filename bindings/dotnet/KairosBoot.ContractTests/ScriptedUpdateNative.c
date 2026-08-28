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

typedef struct kb_update_options {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t timeout_ms;
  int32_t wipe;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
} kb_update_options_t;

typedef struct kb_flash_options {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
} kb_flash_options_t;

typedef struct kb_operation {
  int32_t kind;
  int32_t polls;
  int32_t cancelled;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
  char *device_identifier;
} kb_operation_t;

typedef struct kb_job_options {
  uint32_t struct_size;
  uint32_t api_version;
  uint32_t timeout_ms;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
} kb_job_options_t;

typedef struct kb_job {
  int32_t kind;
  int32_t polls;
  int32_t cancelled;
  int32_t terminal_state;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
} kb_job_t;

typedef struct kb_job_report {
  char *json;
} kb_job_report_t;

typedef struct kb_error {
  int32_t status;
  const char *message;
} kb_error_t;

static int32_t failure_code;
static int32_t options_init_count;
static int32_t async_start_count;
static int32_t blocking_count;
static int32_t flash_options_init_count;
static int32_t flash_raw_async_start_count;
static int32_t flash_raw_blocking_count;
static int32_t boot_async_start_count;
static int32_t boot_blocking_count;
static int32_t cancel_count;
static int32_t operation_release_count;
static int32_t context_release_count;
static int32_t job_options_init_count;
static int32_t job_async_start_count;
static int32_t job_blocking_count;
static int32_t job_cancel_count;
static int32_t job_release_count;
static int32_t job_report_release_count;
static int32_t context_sentinel;
static kb_error_t job_error = {KB_E_IO, "scripted fleet failure"};

static const char expected_unicode_package[] = {
    'i', 'm', 'a', 'g', 'e', 's', '/', (char)0xe5, (char)0x8d,
    (char)0x87, (char)0xe7, (char)0xba, (char)0xa7, '.', 'z', 'i', 'p', 0};

static const char expected_unicode_kernel[] = {
    'i', 'm', 'a', 'g', 'e', 's', '/', (char)0xe5, (char)0x86,
    (char)0x85, (char)0xe6, (char)0xa0, (char)0xb8, '.', 'b', 'i', 'n', 0};

static const char expected_unicode_boot_image[] = {
    'i', 'm', 'a', 'g', 'e', 's', '/', (char)0xe5, (char)0x90,
    (char)0xaf, (char)0xe5, (char)0x8a, (char)0xa8, '.', 'i', 'm', 'g', 0};

static int same_string(const char *actual, const char *expected) {
  return actual != NULL && strcmp(actual, expected) == 0;
}

static int valid_options(const kb_update_options_t *options) {
  return options != NULL &&
         options->struct_size == (uint32_t)sizeof(*options) &&
         options->api_version == 1 &&
         ((options->progress_callback == NULL &&
           options->progress_user_data == NULL) ||
          (options->progress_callback != NULL &&
           options->progress_user_data != NULL));
}

static int valid_flash_options(const kb_flash_options_t *options) {
  return options != NULL &&
         options->struct_size == (uint32_t)sizeof(*options) &&
         options->api_version == 1 &&
         ((options->progress_callback == NULL &&
           options->progress_user_data == NULL) ||
          (options->progress_callback != NULL &&
           options->progress_user_data != NULL));
}

static int valid_job_options(const kb_job_options_t *options) {
  return options != NULL &&
         options->struct_size == (uint32_t)sizeof(*options) &&
         options->api_version == 1 &&
         ((options->progress_callback == NULL &&
           options->progress_user_data == NULL) ||
          (options->progress_callback != NULL &&
           options->progress_user_data != NULL));
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
  flash_raw_async_start_count = 0;
  flash_raw_blocking_count = 0;
  boot_async_start_count = 0;
  boot_blocking_count = 0;
  cancel_count = 0;
  operation_release_count = 0;
  context_release_count = 0;
  job_options_init_count = 0;
  job_async_start_count = 0;
  job_blocking_count = 0;
  job_cancel_count = 0;
  job_release_count = 0;
  job_report_release_count = 0;
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

KB_TEST_API int32_t KB_TEST_CALL kb_test_job_options_init_count(void) {
  return job_options_init_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_job_async_start_count(void) {
  return job_async_start_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_job_blocking_count(void) {
  return job_blocking_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_job_cancel_count(void) {
  return job_cancel_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_job_release_count(void) {
  return job_release_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_job_report_release_count(void) {
  return job_report_release_count;
}

KB_TEST_API const char *KB_TEST_CALL kb_status_string(int32_t status) {
  (void)status;
  return "scripted update native validation failed";
}

KB_TEST_API int32_t KB_TEST_CALL kb_context_create(
    const void *options, void **context, void **error) {
  if (options != NULL || context == NULL || error == NULL) {
    record_failure(10);
    return KB_E_INVALID_ARGUMENT;
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

KB_TEST_API void KB_TEST_CALL kb_update_options_init(
    kb_update_options_t *options) {
  if (options == NULL) {
    record_failure(20);
    return;
  }
  memset(options, 0, sizeof(*options));
  options->struct_size = (uint32_t)sizeof(*options);
  options->api_version = 1;
  options->timeout_ms = UINT32_MAX;
  ++options_init_count;
}

KB_TEST_API void KB_TEST_CALL kb_flash_options_init(
    kb_flash_options_t *options) {
  if (options == NULL) {
    record_failure(21);
    return;
  }
  memset(options, 0, sizeof(*options));
  options->struct_size = (uint32_t)sizeof(*options);
  options->api_version = 1;
  options->timeout_ms = UINT32_MAX;
  ++flash_options_init_count;
}

KB_TEST_API void KB_TEST_CALL kb_job_options_init(kb_job_options_t *options) {
  if (options == NULL) {
    record_failure(80);
    return;
  }
  memset(options, 0, sizeof(*options));
  options->struct_size = (uint32_t)sizeof(*options);
  options->api_version = 1;
  options->timeout_ms = UINT32_MAX;
  ++job_options_init_count;
}

static kb_job_report_t *make_job_report(const char *json) {
  kb_job_report_t *report = (kb_job_report_t *)calloc(1, sizeof(*report));
  if (report == NULL) {
    return NULL;
  }
  const size_t size = strlen(json) + 1;
  report->json = (char *)malloc(size);
  if (report->json == NULL) {
    free(report);
    return NULL;
  }
  memcpy(report->json, json, size);
  return report;
}

static const char *job_report_json(const kb_job_t *job) {
  if (job->kind == 1) {
    return "{\"state\":\"succeeded\",\"devices\":[{\"id\":\"SERIAL-01\"}]}";
  }
  if (job->kind == 2) {
    return "{\"state\":\"failed\",\"devices\":[{\"id\":\"SERIAL-FAIL\"}]}";
  }
  return "{\"state\":\"cancelled\",\"devices\":[]}";
}

KB_TEST_API int32_t KB_TEST_CALL kb_run_job_file_async(
    void *context, const char *file_path, const kb_job_options_t *options,
    kb_job_t **job, void **error) {
  if (context != &context_sentinel || file_path == NULL || job == NULL ||
      error == NULL || !valid_job_options(options)) {
    record_failure(81);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(file_path, "success.yaml")) {
    kind = 1;
    if (options->timeout_ms != 2 || options->progress_callback == NULL) {
      record_failure(82);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(file_path, "failure.yaml")) {
    kind = 2;
  } else if (same_string(file_path, "cancel.yaml")) {
    kind = 3;
    if (options->progress_callback == NULL) {
      record_failure(83);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(file_path, "dispose.yaml")) {
    kind = 4;
  } else {
    record_failure(84);
    return KB_E_INVALID_ARGUMENT;
  }

  kb_job_t *created = (kb_job_t *)calloc(1, sizeof(*created));
  if (created == NULL) {
    record_failure(85);
    return KB_E_INVALID_ARGUMENT;
  }
  created->kind = kind;
  created->progress_callback = options->progress_callback;
  created->progress_user_data = options->progress_user_data;
  *job = created;
  *error = NULL;
  ++job_async_start_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_run_job_file(
    void *context, const char *file_path, const kb_job_options_t *options,
    kb_job_report_t **report, void **error) {
  if (context != &context_sentinel || !same_string(file_path, "blocking.yaml") ||
      report == NULL || error == NULL || !valid_job_options(options) ||
      options->timeout_ms != 17 || options->progress_callback != NULL) {
    record_failure(86);
    return KB_E_INVALID_ARGUMENT;
  }
  *report = make_job_report("{\"state\":\"succeeded\",\"blocking\":true}");
  if (*report == NULL) {
    record_failure(87);
    return KB_E_INVALID_ARGUMENT;
  }
  *error = NULL;
  ++job_blocking_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_job_wait(kb_job_t *job,
                                             uint32_t timeout_ms) {
  if (job == NULL || (timeout_ms != 0 && timeout_ms != UINT32_MAX)) {
    record_failure(88);
    return KB_E_INVALID_ARGUMENT;
  }
  if (job->terminal_state == 2) {
    return KB_OK;
  }
  if (job->terminal_state == 3) {
    return KB_E_IO;
  }
  if (job->terminal_state == 4 || job->cancelled) {
    job->terminal_state = 4;
    return KB_E_CANCELLED;
  }
  if (job->polls == 0) {
    if (job->progress_callback != NULL) {
      kb_progress_t progress;
      memset(&progress, 0, sizeof(progress));
      progress.struct_size = (uint32_t)sizeof(progress);
      progress.api_version = 1;
      progress.bytes_completed = 3;
      progress.bytes_total = 9;
      progress.stage = "execute";
      progress.device_identifier = "SERIAL-01";
      if (job->progress_callback(&progress, job->progress_user_data) != 0) {
        job->cancelled = 1;
      }
    }
    ++job->polls;
    if (job->cancelled) {
      job->terminal_state = 4;
      return KB_E_CANCELLED;
    }
    if (timeout_ms == 0) {
      return KB_E_TIMEOUT;
    }
  }
  if (job->kind == 1) {
    job->terminal_state = 2;
    return KB_OK;
  }
  if (job->kind == 2) {
    job->terminal_state = 3;
    return KB_E_IO;
  }
  if (timeout_ms == UINT32_MAX) {
    job->terminal_state = 4;
    return KB_E_CANCELLED;
  }
  return KB_E_TIMEOUT;
}

KB_TEST_API int32_t KB_TEST_CALL kb_job_cancel(kb_job_t *job) {
  if (job == NULL) {
    record_failure(89);
    return KB_E_INVALID_ARGUMENT;
  }
  if (!job->cancelled && job->terminal_state == 0) {
    job->cancelled = 1;
    ++job_cancel_count;
  }
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_job_state(const kb_job_t *job) {
  if (job == NULL) {
    return 3;
  }
  return job->terminal_state == 0 ? 1 : job->terminal_state;
}

KB_TEST_API const kb_error_t *KB_TEST_CALL kb_job_error(const kb_job_t *job) {
  if (job == NULL || (job->terminal_state != 3 && job->terminal_state != 4)) {
    return NULL;
  }
  job_error.status = job->terminal_state == 4 ? KB_E_CANCELLED : KB_E_IO;
  job_error.message = job->terminal_state == 4
                          ? "scripted fleet cancellation"
                          : "scripted fleet failure";
  return &job_error;
}

KB_TEST_API int32_t KB_TEST_CALL kb_job_get_report(
    const kb_job_t *job, kb_job_report_t **report, void **error) {
  if (job == NULL || report == NULL || error == NULL) {
    record_failure(90);
    return KB_E_INVALID_ARGUMENT;
  }
  *report = NULL;
  *error = NULL;
  if (job->terminal_state == 0) {
    return KB_E_BUSY;
  }
  *report = make_job_report(job_report_json(job));
  return *report == NULL ? KB_E_INVALID_ARGUMENT : KB_OK;
}

KB_TEST_API void KB_TEST_CALL kb_job_release(kb_job_t *job) {
  if (job == NULL) {
    record_failure(91);
    return;
  }
  if (job->terminal_state == 0 && !job->cancelled) {
    job->cancelled = 1;
    ++job_cancel_count;
  }
  ++job_release_count;
  free(job);
}

KB_TEST_API const char *KB_TEST_CALL kb_job_report_json(
    const kb_job_report_t *report, size_t *size) {
  if (size != NULL) {
    *size = 0;
  }
  if (report == NULL || report->json == NULL) {
    return NULL;
  }
  if (size != NULL) {
    *size = strlen(report->json);
  }
  return report->json;
}

KB_TEST_API void KB_TEST_CALL kb_job_report_release(kb_job_report_t *report) {
  if (report == NULL) {
    record_failure(92);
    return;
  }
  ++job_report_release_count;
  free(report->json);
  free(report);
}

KB_TEST_API int32_t KB_TEST_CALL kb_update_package_async(
    void *context, const char *device_selector, const char *package_path,
    const kb_update_options_t *options, kb_operation_t **operation,
    void **error) {
  if (context != &context_sentinel || operation == NULL || error == NULL ||
      !valid_options(options)) {
    record_failure(30);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(package_path, expected_unicode_package)) {
    kind = 1;
    if (!same_string(device_selector, "usb:serial:device") ||
        options->timeout_ms != 2 || options->wipe != 1) {
      record_failure(31);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(package_path, "cancel.zip")) {
    kind = 2;
    if (!same_string(device_selector, "tcp:127.0.0.1:5554") ||
        options->timeout_ms != UINT32_MAX || options->wipe != 0) {
      record_failure(32);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(package_path, "default.zip")) {
    kind = 3;
    if (device_selector != NULL || options->timeout_ms != UINT32_MAX ||
        options->wipe != 0 || options->progress_callback != NULL ||
        options->progress_user_data != NULL) {
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
  const char *identifier = device_selector == NULL ? "" : device_selector;
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
    void *context, const char *device_selector, const char *package_path,
    const kb_update_options_t *options, void **error) {
  if (context != (void *)(uintptr_t)1 || error == NULL || options == NULL ||
      options->struct_size != (uint32_t)sizeof(*options) ||
      options->api_version != 1 ||
      options->timeout_ms != 17 || options->wipe != 1 ||
      options->progress_callback != NULL || options->progress_user_data != NULL ||
      !same_string(device_selector, "usb:serial:blocking") ||
      !same_string(package_path, "blocking.zip")) {
    record_failure(40);
    return KB_E_INVALID_ARGUMENT;
  }
  *error = NULL;
  ++blocking_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_flash_raw_async(
    void *context, const char *device_selector, const char *partition,
    const char *kernel_path, const char *ramdisk_path,
    const char *second_stage_path, const kb_flash_options_t *options,
    kb_operation_t **operation, void **error) {
  if (context != &context_sentinel || operation == NULL || error == NULL ||
      !valid_flash_options(options) || !same_string(partition, "boot")) {
    record_failure(41);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(kernel_path, expected_unicode_kernel)) {
    kind = 4;
    if (!same_string(device_selector, "usb:serial:raw") ||
        !same_string(ramdisk_path, "ramdisk.img") ||
        second_stage_path != NULL || options->timeout_ms != 2 ||
        options->progress_callback == NULL) {
      record_failure(42);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(kernel_path, "cancel-kernel.bin")) {
    kind = 5;
    if (!same_string(device_selector, "tcp:127.0.0.1:5554") ||
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
  const size_t identifier_size = strlen(device_selector) + 1;
  created->device_identifier = (char *)malloc(identifier_size);
  if (created->device_identifier == NULL) {
    free(created);
    record_failure(46);
    return KB_E_INVALID_ARGUMENT;
  }
  memcpy(created->device_identifier, device_selector, identifier_size);
  *operation = created;
  *error = NULL;
  ++flash_raw_async_start_count;
  return KB_OK;
}

KB_TEST_API int32_t KB_TEST_CALL kb_flash_raw(
    void *context, const char *device_selector, const char *partition,
    const char *kernel_path, const char *ramdisk_path,
    const char *second_stage_path, const kb_flash_options_t *options,
    void **error) {
  if (context != (void *)(uintptr_t)1 || error == NULL ||
      !valid_flash_options(options) || options->timeout_ms != 17 ||
      options->progress_callback != NULL ||
      !same_string(device_selector, "usb:serial:blocking-raw") ||
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

KB_TEST_API int32_t KB_TEST_CALL kb_boot_file_async(
    void *context, const char *device_selector, const char *file_path,
    const kb_flash_options_t *options, kb_operation_t **operation,
    void **error) {
  if (context != &context_sentinel || operation == NULL || error == NULL ||
      !valid_flash_options(options)) {
    record_failure(100);
    return KB_E_INVALID_ARGUMENT;
  }

  int32_t kind = 0;
  if (same_string(file_path, expected_unicode_boot_image)) {
    kind = 1;
    if (!same_string(device_selector, "usb:serial:boot-device") ||
        options->timeout_ms != 2 || options->progress_callback == NULL) {
      record_failure(101);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(file_path, "cancel-boot.img")) {
    kind = 2;
    if (!same_string(device_selector, "tcp:127.0.0.1:5554") ||
        options->timeout_ms != UINT32_MAX ||
        options->progress_callback == NULL) {
      record_failure(102);
      return KB_E_INVALID_ARGUMENT;
    }
  } else if (same_string(file_path, "default-boot.img")) {
    kind = 3;
    if (device_selector != NULL || options->timeout_ms != UINT32_MAX ||
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
  const char *identifier = device_selector == NULL ? "" : device_selector;
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
    void *context, const char *device_selector, const char *file_path,
    const kb_flash_options_t *options, void **error) {
  if (context != (void *)(uintptr_t)1 || error == NULL ||
      !valid_flash_options(options) || options->timeout_ms != 17 ||
      options->progress_callback != NULL ||
      options->progress_user_data != NULL ||
      !same_string(device_selector, "usb:serial:blocking-boot") ||
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
