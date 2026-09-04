#include <kairosboot/kairosboot.h>

#include "../abi/kairosboot-layout-v1.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
      return __LINE__;                                                          \
    }                                                                           \
  } while (0)

#define CHECK_CURRENT_INIT_DESIGNATORS(initializer, type)                     \
  do {                                                                         \
    type pointer_value;                                                        \
    type parenthesized_value;                                                  \
    void(KB_CALL *initializer_pointer)(type *) = (initializer);                \
    memset(&pointer_value, 0xa5, sizeof(pointer_value));                       \
    memset(&parenthesized_value, 0xa5, sizeof(parenthesized_value));           \
    initializer_pointer(&pointer_value);                                       \
    (initializer)(&parenthesized_value);                                       \
    CHECK(pointer_value.struct_size == sizeof(type));                          \
    CHECK(pointer_value.api_version == KB_API_VERSION);                        \
    CHECK(parenthesized_value.struct_size == sizeof(type));                    \
    CHECK(parenthesized_value.api_version == KB_API_VERSION);                  \
  } while (0)

int kb_test_legacy_initializer_bounds(void);

struct update_progress_probe {
  int calls;
  int saw_preflight;
  int cancel;
};

static kb_progress_action_t KB_CALL
observe_update_progress(const kb_progress_t *progress, void *user_data) {
  struct update_progress_probe *probe =
      (struct update_progress_probe *)user_data;
  if (probe == NULL || progress == NULL ||
      progress->struct_size < sizeof(*progress) ||
      progress->api_version != KB_API_VERSION) {
    return KB_PROGRESS_CANCEL;
  }
  ++probe->calls;
  if (progress->stage != NULL && strcmp(progress->stage, "preflight") == 0) {
    probe->saw_preflight = 1;
  }
  return probe->cancel != 0 ? KB_PROGRESS_CANCEL : KB_PROGRESS_CONTINUE;
}

int main(void) {
  kb_status_t(KB_CALL *device_flash_file)(
      kb_device_t *, const char *, const char *, const kb_flash_options_t *,
      kb_error_t **) = &kb_flash_file;
  kb_status_t(KB_CALL *device_getvar)(
      kb_device_t *, const char *, const kb_command_options_t *,
      kb_command_result_t **, kb_error_t **) = &kb_getvar;
  CHECK(device_flash_file == &kb_flash_file);
  CHECK(device_getvar == &kb_getvar);

  kb_version_t version = {0};
  CHECK(kb_get_version(&version) == KB_E_INVALID_ARGUMENT);
  kb_version_init(&version);
  CHECK(kb_get_version(&version) == KB_OK);
  CHECK(version.api_version == KB_API_VERSION);
  CHECK(version.string != NULL);
  CHECK(strlen(version.string) > 0U);
  CHECK(kb_get_version(NULL) == KB_E_INVALID_ARGUMENT);
  {
    struct extended_version {
      kb_version_t v1;
      uint64_t future_field;
    } extended;
    extended.future_field = UINT64_C(0x1122334455667788);
    kb_version_init_sized(&extended.v1, sizeof(extended));
    CHECK(kb_get_version(&extended.v1) == KB_OK);
    CHECK(extended.future_field == UINT64_C(0x1122334455667788));
  }

  kb_context_options_t context_options;
  kb_context_options_init(&context_options);
  CHECK(context_options.struct_size == sizeof(context_options));
  CHECK(context_options.api_version == KB_API_VERSION);
  CHECK(context_options.usb_vendor_id == 0U);

  kb_flash_options_t flash_options;
  kb_flash_options_init(&flash_options);
  CHECK(flash_options.struct_size == sizeof(flash_options));
  CHECK(flash_options.api_version == KB_API_VERSION);
  CHECK(flash_options.timeout_ms == KB_WAIT_INFINITE);
  CHECK(flash_options.slot == NULL);
  CHECK(flash_options.set_active == 0);
  CHECK(flash_options.active_slot == NULL);
  CHECK(flash_options.sparse_limit_bytes == 0);
  CHECK(flash_options.force == 0);
  CHECK(flash_options.filesystem_options == KB_FILESYSTEM_OPTION_NONE);

  kb_legacy_boot_options_t legacy_boot_options;
  kb_legacy_boot_options_init(&legacy_boot_options);
  CHECK(legacy_boot_options.struct_size == sizeof(legacy_boot_options));
  CHECK(legacy_boot_options.api_version == KB_API_VERSION);
  CHECK(legacy_boot_options.command_line == NULL);
  CHECK(legacy_boot_options.base == UINT32_C(0x10000000));
  CHECK(legacy_boot_options.page_size == 2048U);
  CHECK(legacy_boot_options.kernel_offset == UINT32_C(0x00008000));
  CHECK(legacy_boot_options.ramdisk_offset == UINT32_C(0x01000000));
  CHECK(legacy_boot_options.second_offset == UINT32_C(0x00f00000));
  CHECK(legacy_boot_options.tags_offset == UINT32_C(0x00000100));
  CHECK(legacy_boot_options.header_version == 0U);
  CHECK(legacy_boot_options.os_version == NULL);
  CHECK(legacy_boot_options.os_patch_level == NULL);
  CHECK(legacy_boot_options.dtb_path == NULL);
  CHECK(legacy_boot_options.dtb_offset == UINT64_C(0x01100000));

  kb_update_options_t update_options;
  kb_update_options_init(&update_options);
  CHECK(update_options.struct_size == sizeof(update_options));
  CHECK(update_options.api_version == KB_API_VERSION);
  CHECK(update_options.timeout_ms == KB_WAIT_INFINITE);
  CHECK(update_options.wipe == 0);
  CHECK(update_options.progress_callback == NULL);
  CHECK(update_options.slot == NULL);
  CHECK(update_options.set_active == 0);
  CHECK(update_options.active_slot == NULL);
  CHECK(update_options.sparse_limit_bytes == 0);
  CHECK(update_options.force == 0);
  CHECK(update_options.filesystem_options == KB_FILESYSTEM_OPTION_NONE);
  CHECK(update_options.disable_super_optimization == 0);

  kb_command_options_t command_options;
  kb_command_options_init(&command_options);
  CHECK(command_options.struct_size == sizeof(command_options));
  CHECK(command_options.api_version == KB_API_VERSION);
  CHECK(command_options.timeout_ms == KB_WAIT_INFINITE);
  CHECK(command_options.maximum_receive_bytes == UINT64_C(64) * 1024U * 1024U);
  CHECK(strcmp(kb_status_string(KB_E_PROTOCOL), "protocol") == 0);
  CHECK(strcmp(kb_status_string(KB_E_DEVICE_FAIL), "device_fail") == 0);

  CHECK_CURRENT_INIT_DESIGNATORS(kb_context_options_init,
                                 kb_context_options_t);
  CHECK_CURRENT_INIT_DESIGNATORS(kb_flash_options_init, kb_flash_options_t);
  CHECK_CURRENT_INIT_DESIGNATORS(kb_legacy_boot_options_init,
                                 kb_legacy_boot_options_t);
  CHECK_CURRENT_INIT_DESIGNATORS(kb_update_options_init, kb_update_options_t);
  CHECK_CURRENT_INIT_DESIGNATORS(kb_command_options_init,
                                 kb_command_options_t);
  CHECK_CURRENT_INIT_DESIGNATORS(kb_version_init, kb_version_t);
  CHECK(kb_test_legacy_initializer_bounds() == 0);

  kb_error_t *error = NULL;
  CHECK(kb_context_create(NULL, NULL, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(error != NULL);
  CHECK(kb_error_status(error) == KB_E_INVALID_ARGUMENT);
  kb_error_release(error);

  kb_context_t *context = NULL;
  error = NULL;
  CHECK(kb_context_create(NULL, &context, &error) == KB_OK);
  CHECK(context != NULL);
  CHECK(error == NULL);

  kb_device_t *tcp_device = NULL;
  kb_device_t *udp_device = NULL;
  kb_device_t *tcp_default_device = NULL;
  CHECK(kb_device_open(context, "tcp:127.0.0.1:1", &tcp_device, &error) ==
        KB_OK);
  CHECK(kb_device_open(context, "udp:127.0.0.1:1", &udp_device, &error) ==
        KB_OK);
  CHECK(kb_device_open(context, "tcp:127.0.0.1", &tcp_default_device,
                       &error) == KB_OK);
  CHECK(strcmp(kb_device_identifier(tcp_device), "tcp:127.0.0.1:1") == 0);
  CHECK(strcmp(kb_device_serial(tcp_device), "") == 0);
  CHECK(strcmp(kb_device_usb_path(tcp_device), "") == 0);
  CHECK(kb_device_retain(tcp_device) == KB_OK);
  kb_device_release(tcp_device);
  CHECK(strcmp(kb_device_identifier(tcp_device), "tcp:127.0.0.1:1") == 0);

  {
    kb_context_options_t vendor_options;
    kb_context_options_init_sized(&vendor_options, sizeof(vendor_options));
    vendor_options.usb_vendor_id = UINT32_C(0x18d1);
    kb_context_t *vendor_context = NULL;
    CHECK(kb_context_create(&vendor_options, &vendor_context, &error) == KB_OK);
    CHECK(vendor_context != NULL);
    CHECK(error == NULL);
    kb_context_release(vendor_context);

    vendor_options.usb_vendor_id = UINT32_C(0x10000);
    CHECK(kb_context_create(&vendor_options, &vendor_context, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(vendor_context == NULL);
    CHECK(error != NULL);
    kb_error_release(error);
    error = NULL;
  }

  {
    struct extended_command_options {
      kb_command_options_t v1;
      uint64_t future_field;
    } extended_options;
    extended_options.future_field = UINT64_C(0x1234);
    kb_command_options_init_sized(&extended_options.v1,
                                  sizeof(extended_options));
    kb_device_t *invalid_device = NULL;
    kb_operation_t *typed_operation = NULL;
    CHECK(kb_device_open(context, "unknown:device", &invalid_device, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(invalid_device == NULL);
    CHECK(typed_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "unknown scheme") != NULL);
    kb_error_release(error);
    error = NULL;

    command_options.struct_size = sizeof(uint32_t);
    CHECK(kb_boot_async(tcp_device, &command_options, &typed_operation,
                        &error) == KB_E_INVALID_ARGUMENT);
    CHECK(typed_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "command options") != NULL);
    kb_error_release(error);
    error = NULL;
    kb_command_options_init_sized(&command_options, sizeof(command_options));
  }

  {
    struct legacy_flash_options_v1 {
      uint32_t struct_size;
      uint32_t api_version;
      uint32_t timeout_ms;
      kb_progress_callback_t progress_callback;
      void *progress_user_data;
    } legacy_flash_options = {0};
    legacy_flash_options.struct_size = sizeof(legacy_flash_options);
    legacy_flash_options.api_version = KB_API_VERSION;
    legacy_flash_options.timeout_ms = KB_WAIT_INFINITE;
    kb_operation_t *legacy_flash_operation = NULL;
    CHECK(kb_flash_file_async(
              tcp_device, "system",
              "kairosboot-hermetic-sparse-limit-missing.img",
              (const kb_flash_options_t *)&legacy_flash_options,
              &legacy_flash_operation, &error) == KB_E_IO);
    CHECK(legacy_flash_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "flash options") == NULL);
    kb_error_release(error);
    error = NULL;

    struct legacy_update_options_v1 {
      uint32_t struct_size;
      uint32_t api_version;
      uint32_t timeout_ms;
      int32_t wipe;
      kb_progress_callback_t progress_callback;
      void *progress_user_data;
    } legacy_options = {0};
    legacy_options.struct_size = sizeof(legacy_options);
    legacy_options.api_version = KB_API_VERSION;
    legacy_options.timeout_ms = KB_WAIT_INFINITE;
    kb_operation_t *legacy_operation = NULL;
    CHECK(kb_update_package_async(
              tcp_device, "unused-update-package",
              (const kb_update_options_t *)&legacy_options,
              &legacy_operation, &error) == KB_OK);
    CHECK(kb_operation_wait(legacy_operation, KB_WAIT_INFINITE) == KB_E_IO);
    kb_operation_release(legacy_operation);
    kb_device_t *invalid_device = NULL;
    CHECK(kb_device_open(context, "unknown:device", &invalid_device, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(invalid_device == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "unknown scheme") != NULL);
    kb_error_release(error);
    error = NULL;

    struct extended_update_options {
      kb_update_options_t v1;
      uint64_t future_field;
    } extended_options;
    extended_options.future_field = UINT64_C(0xabcddcba12344321);
    kb_update_options_init_sized(&extended_options.v1,
                                 sizeof(extended_options));
    kb_operation_t *update_operation = NULL;
    CHECK(kb_update_package_async(tcp_device, "unused-update-package",
                                  &extended_options.v1, &update_operation,
                                  &error) == KB_OK);
    CHECK(update_operation != NULL);
    CHECK(error == NULL);
    CHECK(extended_options.future_field ==
          UINT64_C(0xabcddcba12344321));
    CHECK(kb_operation_wait(update_operation, KB_WAIT_INFINITE) == KB_E_IO);
    kb_operation_release(update_operation);
    update_operation = NULL;
    error = NULL;

    update_options.struct_size = sizeof(uint32_t);
    CHECK(kb_update_package_async(
              tcp_device, "unused-update-package", &update_options,
              &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "update options") != NULL);
    kb_error_release(error);
    error = NULL;
    kb_update_options_init_sized(&update_options, sizeof(update_options));

    update_options.wipe = 2;
    CHECK(kb_update_package_async(
              tcp_device, "unused-update-package", &update_options,
              &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    kb_error_release(error);
    error = NULL;
    kb_update_options_init_sized(&update_options, sizeof(update_options));

    update_options.exclude_dynamic_partitions = 2;
    CHECK(kb_update_package_async(
              tcp_device, "unused-update-package", &update_options,
              &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    kb_error_release(error);
    error = NULL;
    kb_update_options_init_sized(&update_options, sizeof(update_options));

    update_options.set_active = 2;
    CHECK(kb_update_package_async(
              tcp_device, "unused-update-package", &update_options,
              &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    kb_error_release(error);
    error = NULL;
    kb_update_options_init_sized(&update_options, sizeof(update_options));

    update_options.disable_super_optimization = 2;
    CHECK(kb_update_package_async(
              tcp_device, "unused-update-package", &update_options,
              &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    kb_error_release(error);
    error = NULL;
    kb_update_options_init_sized(&update_options, sizeof(update_options));

    update_options.active_slot = "b";
    CHECK(kb_update_package_async(
              tcp_device, "unused-update-package", &update_options,
              &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    kb_error_release(error);
    error = NULL;
    kb_update_options_init_sized(&update_options, sizeof(update_options));
  }

  {
    kb_operation_t *wipe_operation = NULL;
    CHECK(kb_wipe_super_async(tcp_device, "", &update_options,
                              &wipe_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(wipe_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "super_empty") != NULL);
    kb_error_release(error);
    error = NULL;

    CHECK(kb_wipe_super(tcp_device,
                        "kairosboot-hermetic-super-empty-does-not-exist.img",
                        &update_options, &error) == KB_E_IO);
    CHECK(error != NULL);
    CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
    kb_error_release(error);
    error = NULL;
  }

  {
    const char *missing_package =
        "kairosboot-hermetic-update-package-does-not-exist";
    struct update_progress_probe probe = {0, 0, 0};
    kb_operation_t *update_operation = NULL;
    update_options.progress_callback = &observe_update_progress;
    update_options.progress_user_data = &probe;
    CHECK(kb_update_package_async(
              tcp_device, missing_package, &update_options,
              &update_operation, &error) == KB_OK);
    CHECK(update_operation != NULL);
    CHECK(error == NULL);
    CHECK(kb_operation_wait(update_operation, KB_WAIT_INFINITE) == KB_E_IO);
    CHECK(probe.calls >= 1);
    CHECK(probe.saw_preflight == 1);
    CHECK(kb_operation_error(update_operation) != NULL);
    CHECK(strstr(kb_error_message(kb_operation_error(update_operation)),
                 "update package") != NULL);
    CHECK(strcmp(kb_error_device_identifier(
                     kb_operation_error(update_operation)),
                 "tcp:127.0.0.1:1") == 0);
    CHECK(kb_error_transfer_state(kb_operation_error(update_operation)) ==
          KB_TRANSFER_NOT_SENT);
    kb_operation_release(update_operation);

    probe.calls = 0;
    probe.saw_preflight = 0;
    probe.cancel = 1;
    update_operation = NULL;
    CHECK(kb_update_package_async(
              udp_device, missing_package, &update_options,
              &update_operation, &error) == KB_OK);
    CHECK(kb_operation_wait(update_operation, KB_WAIT_INFINITE) ==
          KB_E_CANCELLED);
    CHECK(probe.calls == 1);
    CHECK(probe.saw_preflight == 1);
    kb_operation_release(update_operation);

    kb_update_options_init_sized(&update_options, sizeof(update_options));
    update_options.timeout_ms = 0;
    update_operation = NULL;
    CHECK(kb_update_package_async(
              tcp_device, missing_package, &update_options,
              &update_operation, &error) == KB_OK);
    CHECK(kb_operation_wait(update_operation, KB_WAIT_INFINITE) ==
          KB_E_TIMEOUT);
    CHECK(kb_error_transfer_state(kb_operation_error(update_operation)) ==
          KB_TRANSFER_NOT_SENT);
    kb_operation_release(update_operation);

    kb_update_options_init_sized(&update_options, sizeof(update_options));
    CHECK(kb_update_package(tcp_device, missing_package, &update_options,
                            &error) == KB_E_IO);
    CHECK(error != NULL);
    CHECK(strcmp(kb_error_device_identifier(error),
                 "tcp:127.0.0.1:1") == 0);
    CHECK(strstr(kb_error_message(error), "update package") != NULL);
    CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
    kb_error_release(error);
    error = NULL;
  }

  {
    kb_operation_t *typed_operation = NULL;
    CHECK(kb_fetch_async(tcp_device, "bad:partition",
                         KB_FETCH_UNSPECIFIED, KB_FETCH_UNSPECIFIED, NULL,
                         &typed_operation, &error) == KB_E_INVALID_ARGUMENT);
    CHECK(typed_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "fetch partition") != NULL);
    kb_error_release(error);
    error = NULL;
  }

  kb_device_list_t *devices = NULL;
  CHECK(kb_enumerate_devices(context, &devices, &error) == KB_OK);
  CHECK(devices != NULL);
  CHECK(error == NULL);
  for (size_t index = 0; index < kb_device_list_count(devices); ++index) {
    CHECK(kb_device_list_serial(devices, index) != NULL);
    CHECK(kb_device_list_usb_path(devices, index) != NULL);
    CHECK(kb_device_list_product(devices, index) != NULL);
  }
  kb_device_list_release(devices);

  kb_context_t *second_context = NULL;
  {
    struct extended_context_options {
      kb_context_options_t v1;
      uint64_t future_field;
    } extended;
    extended.future_field = UINT64_C(0x8877665544332211);
    kb_context_options_init_sized(&extended.v1, sizeof(extended));
    CHECK(kb_context_create(&extended.v1, &second_context, &error) == KB_OK);
    CHECK(extended.future_field == UINT64_C(0x8877665544332211));
  }
  CHECK(second_context != NULL);
  CHECK(error == NULL);
  kb_context_release(second_context);

  kb_operation_t *operation = NULL;
  error = NULL;
  CHECK(kb_flash_file_async(tcp_device, "", "system.img", NULL,
                            &operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_flash_vendor_boot_ramdisk_async(
            tcp_device, "boot", "default", "vendor_ramdisk.img", NULL,
            NULL, &operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_flash_vendor_boot_ramdisk_async(
            tcp_default_device, "vendor_boot", "default",
            "kairosboot-test-does-not-exist-vendor-ramdisk", NULL, NULL,
            &operation, &error) == KB_E_IO);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "tcp:127.0.0.1") == 0);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_boot_raw_async(tcp_device, "kernel", NULL, "second",
                          &legacy_boot_options, NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_flash_file_async(tcp_default_device, "system",
                            "kairosboot-test-does-not-exist.img", NULL,
                            &operation, &error) == KB_E_IO);
  CHECK(operation == NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "tcp:127.0.0.1") == 0);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_flash_file(tcp_device, "system",
                      "kairosboot-test-does-not-exist.img", NULL, &error) ==
        KB_E_IO);
  CHECK(error != NULL);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_flash_vendor_boot_ramdisk(
            tcp_device, "vendor_boot", NULL,
            "kairosboot-test-does-not-exist-vendor-ramdisk", NULL, NULL,
            &error) == KB_E_IO);
  CHECK(error != NULL);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_flash_raw_async(tcp_device, "", "kernel", NULL, NULL, NULL,
                           &operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_signature_file_async(tcp_device, "", NULL, &operation,
                                &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_boot_file_async(tcp_default_device, "", NULL, &operation,
                           &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_flash_raw_async(tcp_device, "boot", "kernel", NULL, "second",
                           NULL, &operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_flash_raw_async(tcp_default_device, "boot",
                           "kairosboot-test-does-not-exist-kernel", NULL, NULL,
                           NULL, &operation, &error) == KB_E_IO);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "tcp:127.0.0.1") == 0);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_boot_file(tcp_default_device,
                     "kairosboot-test-does-not-exist.img",
                     NULL, &error) == KB_E_IO);
  CHECK(error != NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "tcp:127.0.0.1") == 0);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_signature_file_async(
            tcp_default_device, "kairosboot-signature-missing.bin",
            NULL, &operation, &error) == KB_E_IO);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "tcp:127.0.0.1") == 0);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  CHECK(kb_operation_wait(NULL, 0U) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_operation_cancel(NULL) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_operation_state(NULL) == KB_OPERATION_FAILED);
  CHECK(kb_operation_error(NULL) == NULL);
  CHECK(kb_operation_command_result(NULL, NULL, NULL) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_command_result_message_count(NULL) == 0U);
  CHECK(kb_command_result_terminal_payload(NULL, NULL) == NULL);
  CHECK(kb_command_result_message_payload(NULL, 0U, NULL) == NULL);
  CHECK(kb_command_result_data(NULL, NULL) == NULL);
  CHECK(strcmp(kb_command_result_output_path(NULL), "") == 0);
  CHECK(kb_command_result_received_bytes(NULL) == 0U);
  CHECK(strcmp(kb_command_result_device_identifier(NULL), "") == 0);
  kb_command_result_release(NULL);

  CHECK(kb_error_device_message(NULL, NULL) == NULL);
  CHECK(kb_error_command_message_count(NULL) == 0U);
  CHECK(kb_error_command_message_payload(NULL, 0U, NULL) == NULL);
  CHECK(kb_error_inbound_expected_bytes(NULL) == KB_FETCH_UNSPECIFIED);
  CHECK(kb_error_inbound_transferred_bytes(NULL) == 0U);
  CHECK(kb_error_inbound_transfer_state(NULL) == KB_TRANSFER_NOT_SENT);
  CHECK(kb_error_session_poisoned(NULL) == 0);

  operation = NULL;
  error = NULL;
  {
    kb_device_t *invalid_device = NULL;
    CHECK(kb_device_open(context, "bad:selector", &invalid_device, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(invalid_device == NULL);
  }
  kb_error_release(error);
  error = NULL;
  CHECK(kb_erase_async(tcp_device, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_format_partition_async(tcp_device, "system", "xfs", 0U, NULL,
                                  &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_set_active_async(tcp_device, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_reboot_async(tcp_device, INT32_MAX, NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_flashing_async(tcp_device, INT32_MAX, NULL, &operation,
                          &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_gsi_async(tcp_device, INT32_MAX, NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_snapshot_update_async(tcp_device, INT32_MAX, NULL, &operation,
                                 &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_create_logical_partition_async(tcp_device, "", 0, NULL,
                                           &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_delete_logical_partition_async(tcp_device, "system:other",
                                           NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(strstr(kb_error_message(error), "ASCII letters") != NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_delete_logical_partition_async(tcp_device, "system other",
                                           NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_resize_logical_partition_async(tcp_device, "bad\nname", 1,
                                           NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  {
    char oversized_name[4097];
    memset(oversized_name, 'x', sizeof(oversized_name) - 1U);
    oversized_name[sizeof(oversized_name) - 1U] = '\0';
    CHECK(kb_create_logical_partition_async(
              tcp_device, oversized_name, UINT64_MAX, NULL, &operation,
              &error) == KB_E_INVALID_ARGUMENT);
    CHECK(operation == NULL);
    kb_error_release(error);
    error = NULL;
  }
  CHECK(kb_flashing(tcp_device, KB_FLASHING_LOCK, NULL, NULL, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_oem_async(tcp_device, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_raw_command_async(tcp_device, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_stage_async(tcp_device, NULL, 0U, NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_fetch_async(tcp_device, "system", KB_FETCH_UNSPECIFIED, 4U,
                       NULL, &operation, &error) == KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_upload_file_async(tcp_device, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_get_staged_file_async(tcp_device, NULL, NULL, &operation,
                                 &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_fetch_file_async(tcp_device, "system", KB_FETCH_UNSPECIFIED, 4U,
                            "output.img", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  {
    kb_command_result_t *file_result = NULL;
    CHECK(kb_upload_file(tcp_device,
                         "kairosboot-unreachable-upload.bin", NULL,
                         &file_result, &error) == KB_E_IO);
    CHECK(file_result == NULL);
    CHECK(error != NULL);
    CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
    kb_error_release(error);
  }
  kb_operation_release(NULL);
  kb_device_list_release(NULL);
  kb_device_release(tcp_default_device);
  kb_device_release(udp_device);
  kb_device_release(tcp_device);
  kb_context_release(context);
  return 0;
}
