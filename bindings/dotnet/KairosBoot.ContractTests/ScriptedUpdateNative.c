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
  KB_E_TIMEOUT = 7,
  KB_E_CANCELLED = 8,
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

typedef struct kb_operation {
  int32_t kind;
  int32_t polls;
  int32_t cancelled;
  kb_progress_callback_t progress_callback;
  void *progress_user_data;
  char *device_identifier;
} kb_operation_t;

static int32_t failure_code;
static int32_t options_init_count;
static int32_t async_start_count;
static int32_t blocking_count;
static int32_t cancel_count;
static int32_t operation_release_count;
static int32_t context_release_count;
static int32_t context_sentinel;

static const char expected_unicode_package[] = {
    'i', 'm', 'a', 'g', 'e', 's', '/', (char)0xe5, (char)0x8d,
    (char)0x87, (char)0xe7, (char)0xba, (char)0xa7, '.', 'z', 'i', 'p', 0};

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
  cancel_count = 0;
  operation_release_count = 0;
  context_release_count = 0;
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

KB_TEST_API int32_t KB_TEST_CALL kb_test_cancel_count(void) {
  return cancel_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_operation_release_count(void) {
  return operation_release_count;
}

KB_TEST_API int32_t KB_TEST_CALL kb_test_context_release_count(void) {
  return context_release_count;
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
    emit_progress(operation, "preflight", 0, 0);
    ++operation->polls;
    if (operation->cancelled) {
      return KB_E_CANCELLED;
    }
    return KB_E_TIMEOUT;
  }

  if (operation->cancelled) {
    return KB_E_CANCELLED;
  }

  if (operation->kind == 2) {
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
