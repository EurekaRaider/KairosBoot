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
    kb_version_init(&extended.v1);
    extended.v1.struct_size = sizeof(extended);
    CHECK(kb_get_version(&extended.v1) == KB_OK);
    CHECK(extended.future_field == UINT64_C(0x1122334455667788));
  }

  kb_context_options_t context_options;
  kb_context_options_init(&context_options);
  CHECK(context_options.struct_size == sizeof(context_options));
  CHECK(context_options.api_version == KB_API_VERSION);

  kb_flash_options_t flash_options;
  kb_flash_options_init(&flash_options);
  CHECK(flash_options.struct_size == sizeof(flash_options));
  CHECK(flash_options.api_version == KB_API_VERSION);
  CHECK(flash_options.timeout_ms == KB_WAIT_INFINITE);

  kb_update_options_t update_options;
  kb_update_options_init(&update_options);
  CHECK(update_options.struct_size == sizeof(update_options));
  CHECK(update_options.api_version == KB_API_VERSION);
  CHECK(update_options.timeout_ms == KB_WAIT_INFINITE);
  CHECK(update_options.wipe == 0);
  CHECK(update_options.progress_callback == NULL);

  kb_command_options_t command_options;
  kb_command_options_init(&command_options);
  CHECK(command_options.struct_size == sizeof(command_options));
  CHECK(command_options.api_version == KB_API_VERSION);
  CHECK(command_options.timeout_ms == KB_WAIT_INFINITE);
  CHECK(command_options.maximum_receive_bytes == UINT64_C(64) * 1024U * 1024U);
  CHECK(strcmp(kb_status_string(KB_E_PROTOCOL), "protocol") == 0);
  CHECK(strcmp(kb_status_string(KB_E_DEVICE_FAIL), "device_fail") == 0);

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

  {
    struct extended_command_options {
      kb_command_options_t v1;
      uint64_t future_field;
    } extended_options;
    kb_command_options_init(&extended_options.v1);
    extended_options.v1.struct_size = sizeof(extended_options);
    extended_options.future_field = UINT64_C(0x1234);
    kb_operation_t *typed_operation = NULL;
    CHECK(kb_continue_boot_async(context, "unknown:device", &extended_options.v1,
                                 &typed_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(typed_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "unknown scheme") != NULL);
    kb_error_release(error);
    error = NULL;

    command_options.struct_size = sizeof(uint32_t);
    CHECK(kb_boot_async(context, "tcp:localhost", &command_options,
                        &typed_operation, &error) == KB_E_INVALID_ARGUMENT);
    CHECK(typed_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "command options") != NULL);
    kb_error_release(error);
    error = NULL;
    kb_command_options_init(&command_options);
  }

  {
    struct extended_update_options {
      kb_update_options_t v1;
      uint64_t future_field;
    } extended_options;
    kb_update_options_init(&extended_options.v1);
    extended_options.v1.struct_size = sizeof(extended_options);
    extended_options.future_field = UINT64_C(0xabcddcba12344321);
    kb_operation_t *update_operation = NULL;
    CHECK(kb_update_package_async(
              context, "unknown:device", "unused-update-package",
              &extended_options.v1, &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "unknown scheme") != NULL);
    CHECK(extended_options.future_field ==
          UINT64_C(0xabcddcba12344321));
    kb_error_release(error);
    error = NULL;

    update_options.struct_size = sizeof(uint32_t);
    CHECK(kb_update_package_async(
              context, "tcp:127.0.0.1:1", "unused-update-package",
              &update_options, &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "update options") != NULL);
    kb_error_release(error);
    error = NULL;
    kb_update_options_init(&update_options);

    update_options.wipe = 2;
    CHECK(kb_update_package_async(
              context, "tcp:127.0.0.1:1", "unused-update-package",
              &update_options, &update_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(update_operation == NULL);
    kb_error_release(error);
    error = NULL;
    kb_update_options_init(&update_options);
  }

  {
    kb_operation_t *wipe_operation = NULL;
    CHECK(kb_wipe_super_async(context, "tcp:127.0.0.1:1", "",
                              &update_options, &wipe_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(wipe_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "super_empty") != NULL);
    kb_error_release(error);
    error = NULL;

    CHECK(kb_wipe_super_async(
              context, "unknown:device", "unused-super-empty.img",
              &update_options, &wipe_operation, &error) ==
          KB_E_INVALID_ARGUMENT);
    CHECK(wipe_operation == NULL);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "unknown scheme") != NULL);
    kb_error_release(error);
    error = NULL;

    CHECK(kb_wipe_super(context, "tcp:127.0.0.1:1",
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
              context, "tcp:127.0.0.1:1", missing_package, &update_options,
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
              context, "udp:127.0.0.1:1", missing_package, &update_options,
              &update_operation, &error) == KB_OK);
    CHECK(kb_operation_wait(update_operation, KB_WAIT_INFINITE) ==
          KB_E_CANCELLED);
    CHECK(probe.calls == 1);
    CHECK(probe.saw_preflight == 1);
    kb_operation_release(update_operation);

    kb_update_options_init(&update_options);
    update_options.timeout_ms = 0;
    update_operation = NULL;
    CHECK(kb_update_package_async(
              context, "usb:1-1", missing_package, &update_options,
              &update_operation, &error) == KB_OK);
    CHECK(kb_operation_wait(update_operation, KB_WAIT_INFINITE) ==
          KB_E_TIMEOUT);
    CHECK(kb_error_transfer_state(kb_operation_error(update_operation)) ==
          KB_TRANSFER_NOT_SENT);
    kb_operation_release(update_operation);

    kb_update_options_init(&update_options);
    CHECK(kb_update_package(
              context, "tcp:127.0.0.1:1", missing_package, &update_options,
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
    CHECK(kb_fetch_async(context, "tcp:127.0.0.1:1", "bad:partition",
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
    kb_context_options_init(&extended.v1);
    extended.v1.struct_size = sizeof(extended);
    extended.future_field = UINT64_C(0x8877665544332211);
    CHECK(kb_context_create(&extended.v1, &second_context, &error) == KB_OK);
    CHECK(extended.future_field == UINT64_C(0x8877665544332211));
  }
  CHECK(second_context != NULL);
  CHECK(error == NULL);
  kb_context_release(second_context);

  kb_operation_t *operation = NULL;
  error = NULL;
  CHECK(kb_flash_file_async(context, NULL, "", "system.img", NULL,
                            &operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_flash_file_async(context, "tcp:127.0.0.1", "system",
                            "kairosboot-test-does-not-exist.img", NULL,
                            &operation, &error) == KB_E_IO);
  CHECK(operation == NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "tcp:127.0.0.1") == 0);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_flash_file_async(context, "ABC", "system",
                            "kairosboot-test-does-not-exist.img", NULL,
                            &operation, &error) == KB_E_IO);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "ABC") == 0);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_flash_file(context, NULL, "system",
                      "kairosboot-test-does-not-exist.img", NULL, &error) ==
        KB_E_IO);
  CHECK(error != NULL);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_flash_raw_async(context, NULL, "", "kernel", NULL, NULL, NULL,
                           &operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_boot_file_async(context, "tcp:127.0.0.1", "", NULL, &operation,
                           &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_flash_raw_async(context, NULL, "boot", "kernel", NULL, "second",
                           NULL, &operation, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  kb_error_release(error);

  operation = NULL;
  error = NULL;
  CHECK(kb_flash_raw_async(context, "tcp:127.0.0.1", "boot",
                           "kairosboot-test-does-not-exist-kernel", NULL, NULL,
                           NULL, &operation, &error) == KB_E_IO);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "tcp:127.0.0.1") == 0);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_boot_file(context, "ABC", "kairosboot-test-does-not-exist.img",
                     NULL, &error) == KB_E_IO);
  CHECK(error != NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "ABC") == 0);
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
  CHECK(kb_getvar_async(context, "bad:selector", "product", NULL, &operation,
                        &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_erase_async(context, NULL, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_set_active_async(context, NULL, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_reboot_async(context, NULL, INT32_MAX, NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_flashing_async(context, NULL, INT32_MAX, NULL, &operation,
                          &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_gsi_async(context, NULL, INT32_MAX, NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_snapshot_update_async(context, NULL, INT32_MAX, NULL, &operation,
                                 &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_create_logical_partition_async(context, NULL, "", 0, NULL,
                                           &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_delete_logical_partition_async(context, NULL, "system:other",
                                           NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  CHECK(strstr(kb_error_message(error), "ASCII letters") != NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_delete_logical_partition_async(context, NULL, "system other",
                                           NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_resize_logical_partition_async(context, NULL, "bad\nname", 1,
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
              context, NULL, oversized_name, UINT64_MAX, NULL, &operation,
              &error) == KB_E_INVALID_ARGUMENT);
    CHECK(operation == NULL);
    kb_error_release(error);
    error = NULL;
  }
  CHECK(kb_flashing(context, NULL, KB_FLASHING_LOCK, NULL, NULL, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_oem_async(context, NULL, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_raw_command_async(context, NULL, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_stage_async(context, NULL, NULL, 0U, NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_fetch_async(context, NULL, "system", KB_FETCH_UNSPECIFIED, 4U,
                       NULL, &operation, &error) == KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_upload_file_async(context, NULL, "", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_get_staged_file_async(context, NULL, NULL, NULL, &operation,
                                 &error) == KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_fetch_file_async(context, NULL, "system", KB_FETCH_UNSPECIFIED, 4U,
                            "output.img", NULL, &operation, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(operation == NULL);
  kb_error_release(error);
  error = NULL;
  {
    kb_command_result_t *file_result = NULL;
    CHECK(kb_upload_file(context, "tcp:127.0.0.1:1",
                         "kairosboot-unreachable-upload.bin", NULL,
                         &file_result, &error) == KB_E_IO);
    CHECK(file_result == NULL);
    CHECK(error != NULL);
    CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
    kb_error_release(error);
  }
  kb_operation_release(NULL);
  kb_device_list_release(NULL);
  kb_context_release(context);
  return 0;
}
