#include <kairosboot/kairosboot.h>

#include <stdio.h>

static void print_error(kb_status_t status, const kb_error_t *error) {
  const char *message =
      error != NULL ? kb_error_message(error) : kb_status_string(status);
  fprintf(stderr, "KairosBoot: %s\n",
          message != NULL ? message : "unknown error");
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "Usage: getvar <variable> [device-selector]\n");
    return 2;
  }

  const char *selector = argc == 3 ? argv[2] : NULL;
  kb_context_t *context = NULL;
  kb_device_t *device = NULL;
  kb_command_result_t *result = NULL;
  kb_error_t *error = NULL;
  kb_status_t status = kb_context_create(NULL, &context, &error);
  int exit_code = 1;

  if (status != KB_OK) {
    print_error(status, error);
    goto cleanup;
  }

  status = kb_device_open(context, selector, &device, &error);
  if (status != KB_OK) {
    print_error(status, error);
    goto cleanup;
  }

  status = kb_getvar(device, argv[1], NULL, &result, &error);
  if (status != KB_OK) {
    print_error(status, error);
    goto cleanup;
  }

  size_t payload_size = 0;
  const uint8_t *payload =
      kb_command_result_terminal_payload(result, &payload_size);
  if ((payload_size != 0 &&
       (payload == NULL || fwrite(payload, 1, payload_size, stdout) !=
                               payload_size)) ||
      fputc('\n', stdout) == EOF || fflush(stdout) == EOF) {
    fprintf(stderr, "KairosBoot: failed to write the getvar result\n");
    goto cleanup;
  }

  exit_code = 0;

cleanup:
  if (result != NULL) {
    kb_command_result_release(result);
  }
  if (error != NULL) {
    kb_error_release(error);
  }
  kb_device_release(device);
  if (context != NULL) {
    kb_context_release(context);
  }
  return exit_code;
}
