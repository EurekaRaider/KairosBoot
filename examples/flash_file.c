#include <kairosboot/kairosboot.h>

#include <stdio.h>

static void print_error(kb_status_t status, const kb_error_t *error) {
  const char *message =
      error != NULL ? kb_error_message(error) : kb_status_string(status);
  fprintf(stderr, "KairosBoot: %s\n",
          message != NULL ? message : "unknown error");
}

int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    fprintf(stderr,
            "Usage: flash_file <partition> <image-file> [usb-serial]\n");
    return 2;
  }

  const char *serial = argc == 4 ? argv[3] : NULL;
  kb_context_t *context = NULL;
  kb_error_t *error = NULL;
  kb_status_t status = kb_context_create(NULL, &context, &error);
  int exit_code = 1;

  if (status != KB_OK) {
    print_error(status, error);
    goto cleanup;
  }

  status = kb_flash_file(context, serial, argv[1], argv[2], NULL, &error);
  if (status != KB_OK) {
    print_error(status, error);
    goto cleanup;
  }

  printf("Flashed %s from %s\n", argv[1], argv[2]);
  exit_code = 0;

cleanup:
  if (error != NULL) {
    kb_error_release(error);
  }
  if (context != NULL) {
    kb_context_release(context);
  }
  return exit_code;
}
