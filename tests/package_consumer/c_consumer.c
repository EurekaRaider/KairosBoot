#include <kairosboot/kairosboot.h>

int main(void) {
  kb_version_t version = {0};
  kb_version_init(&version);
  if (kb_get_version(&version) != KB_OK) {
    return 1;
  }

  kb_context_t *context = NULL;
  kb_error_t *error = NULL;
  if (kb_context_create(NULL, &context, &error) != KB_OK) {
    kb_error_release(error);
    return 2;
  }

  kb_device_list_t *devices = NULL;
  if (kb_enumerate_devices(context, &devices, &error) != KB_OK) {
    kb_error_release(error);
    kb_context_release(context);
    return 3;
  }
  kb_device_list_release(devices);
  kb_context_release(context);
  return 0;
}
