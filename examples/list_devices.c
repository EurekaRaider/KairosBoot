#include <kairosboot/kairosboot.h>

#include <stdio.h>

int main(void) {
  kb_context_t *context = NULL;
  kb_error_t *error = NULL;
  if (kb_context_create(NULL, &context, &error) != KB_OK) {
    fprintf(stderr, "%s\n", kb_error_message(error));
    kb_error_release(error);
    return 1;
  }

  kb_device_list_t *devices = NULL;
  if (kb_enumerate_devices(context, &devices, &error) != KB_OK) {
    fprintf(stderr, "%s\n", kb_error_message(error));
    kb_error_release(error);
    kb_context_release(context);
    return 1;
  }

  printf("%zu device(s)\n", kb_device_list_count(devices));
  kb_device_list_release(devices);
  kb_context_release(context);
  return 0;
}
