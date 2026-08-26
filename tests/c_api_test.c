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

  kb_context_options_t context_options;
  kb_context_options_init(&context_options);
  CHECK(context_options.struct_size == sizeof(context_options));
  CHECK(context_options.api_version == KB_API_VERSION);

  kb_flash_options_t flash_options;
  kb_flash_options_init(&flash_options);
  CHECK(flash_options.struct_size == sizeof(flash_options));
  CHECK(flash_options.api_version == KB_API_VERSION);
  CHECK(flash_options.timeout_ms > 0U);

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
  CHECK(kb_context_create(NULL, &second_context, &error) == KB_OK);
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
  CHECK(kb_flash_file_async(context, "ABC", "system", "system.img", NULL,
                            &operation, &error) == KB_E_NOT_SUPPORTED);
  CHECK(operation == NULL);
  CHECK(error != NULL);
  CHECK(strcmp(kb_error_device_identifier(error), "ABC") == 0);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  error = NULL;
  CHECK(kb_flash_file(context, NULL, "system", "system.img", NULL, &error) ==
        KB_E_NOT_SUPPORTED);
  CHECK(error != NULL);
  CHECK(kb_error_transfer_state(error) == KB_TRANSFER_NOT_SENT);
  kb_error_release(error);

  CHECK(kb_operation_wait(NULL, 0U) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_operation_cancel(NULL) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_operation_state(NULL) == KB_OPERATION_FAILED);
  CHECK(kb_operation_error(NULL) == NULL);
  kb_operation_release(NULL);
  kb_device_list_release(NULL);
  kb_context_release(context);
  return 0;
}
