#include <kairosboot/kairosboot.h>

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

  CHECK(kb_operation_wait(NULL, 0U) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_operation_cancel(NULL) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_operation_state(NULL) == KB_OPERATION_FAILED);
  CHECK(kb_operation_error(NULL) == NULL);
  CHECK(kb_operation_command_result(NULL, NULL, NULL) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_command_result_message_count(NULL) == 0U);
  CHECK(kb_command_result_terminal_payload(NULL, NULL) == NULL);
  CHECK(kb_command_result_message_payload(NULL, 0U, NULL) == NULL);
  CHECK(kb_command_result_data(NULL, NULL) == NULL);
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
  kb_operation_release(NULL);
  kb_device_list_release(NULL);
  kb_context_release(context);
  return 0;
}
