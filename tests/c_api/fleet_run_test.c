/* SPDX-License-Identifier: MIT */
#include <kairosboot/kairosboot.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
      return __LINE__;                                                         \
    }                                                                          \
  } while (0)

enum {
  KB_TEST_SCRIPT_SUCCESS = 0,
  KB_TEST_SCRIPT_FAILURE = 1,
  KB_TEST_SCRIPT_WAIT_FOR_CANCEL = 2
};

enum {
  KB_TEST_PRODUCTION_SUCCESS = 0,
  KB_TEST_PRODUCTION_NO_DEVICE = 1,
  KB_TEST_PRODUCTION_AMBIGUOUS = 2,
  KB_TEST_PRODUCTION_PRODUCT_MISMATCH = 3,
  KB_TEST_PRODUCTION_CONTINUE_FAILURE = 4
};

void kb_test_set_fleet_script(int mode);
void kb_test_set_fleet_production_script(int mode);
size_t kb_test_fleet_dependency_calls(void);
size_t kb_test_fleet_permit_bind_count(void);
size_t kb_test_fleet_payload_count(void);

typedef struct trace_buffer {
  char text[512];
  size_t size;
} trace_buffer_t;

static FILE *open_binary_write(const char *path) {
#if defined(_WIN32)
  FILE *file = NULL;
  return fopen_s(&file, path, "wb") == 0 ? file : NULL;
#else
  return fopen(path, "wb");
#endif
}

static kb_progress_action_t KB_CALL trace_progress(
    const kb_progress_t *progress, void *user_data) {
  trace_buffer_t *trace = (trace_buffer_t *)user_data;
  const size_t stage_size = strlen(progress->stage);
  if (trace->size != 0U && trace->size + 1U < sizeof(trace->text)) {
    trace->text[trace->size++] = ',';
  }
  if (stage_size < sizeof(trace->text) - trace->size) {
    memcpy(trace->text + trace->size, progress->stage, stage_size);
    trace->size += stage_size;
    trace->text[trace->size] = '\0';
  }
  return KB_PROGRESS_CONTINUE;
}

static kb_progress_action_t KB_CALL cancel_device_execution(
    const kb_progress_t *progress, void *user_data) {
  (void)user_data;
  if (strcmp(progress->stage, "execute") == 0 &&
      progress->device_identifier != NULL &&
      progress->device_identifier[0] != '\0') {
    return KB_PROGRESS_CANCEL;
  }
  return KB_PROGRESS_CONTINUE;
}

static int write_manifest(const char *path) {
  static const char manifest[] =
      "apiVersion: kairosboot.io/v1\n"
      "kind: FlashJob\n"
      "artifacts:\n"
      "  - id: system\n"
      "    path: images/system.img\n"
      "    sha256: \""
      "1111111111111111111111111111111111111111111111111111111111111111\"\n"
      "targets:\n"
      "  - name: product-a\n"
      "    selector:\n"
      "      serials: [SERIAL-01]\n"
      "    expectedProduct: product_a\n"
      "    steps:\n"
      "      - flash: { partition: system, artifact: system }\n"
      "policy:\n"
      "  onDeviceFailure: continue\n"
      "  maxParallelDevices: 32\n"
      "  memoryBudget: auto\n";
  FILE *file = open_binary_write(path);
  if (file == NULL) {
    return 0;
  }
  const size_t size = sizeof(manifest) - 1U;
  const int wrote = fwrite(manifest, 1U, size, file) == size;
  const int closed = fclose(file) == 0;
  return wrote && closed;
}

static int write_production_fixture(const char *manifest_path,
                                    const char *artifact_path,
                                    size_t device_count,
                                    int valid_hash) {
  static const char artifact[] = "KairosBootFleet!";
  static const char digest[] =
      "5b4857879890c8ea3dc9c345bfbb18703a7f7e96831b038b2e8f0d6061451c5d";
  FILE *file = open_binary_write(artifact_path);
  if (file == NULL) {
    return 0;
  }
  if (fwrite(artifact, 1U, sizeof(artifact) - 1U, file) !=
          sizeof(artifact) - 1U ||
      fclose(file) != 0) {
    return 0;
  }

  file = open_binary_write(manifest_path);
  if (file == NULL) {
    return 0;
  }
  if (fprintf(file,
              "apiVersion: kairosboot.io/v1\n"
              "kind: FlashJob\n"
              "artifacts:\n"
              "  - id: system\n"
              "    path: %s\n"
              "    sha256: \"%s\"\n"
              "targets:\n"
              "  - name: product-a\n"
              "    selector:\n"
              "      serials: [",
              artifact_path,
              valid_hash ? digest
                         : "0000000000000000000000000000000000000000000000000000000000000000") <
      0) {
    fclose(file);
    return 0;
  }
  for (size_t index = 0U; index < device_count; ++index) {
    if (fprintf(file, "%sSERIAL-%03zu", index == 0U ? "" : ", ", index) <
        0) {
      fclose(file);
      return 0;
    }
  }
  if (fprintf(file,
              "]\n"
              "    expectedProduct: product_a\n"
              "    steps:\n"
              "      - flash: { partition: system, artifact: system }\n"
              "policy:\n"
              "  onDeviceFailure: continue\n"
              "  maxParallelDevices: 32\n"
              "  memoryBudget: auto\n") < 0 ||
      fclose(file) != 0) {
    return 0;
  }
  return 1;
}

static int report_contains(const kb_job_report_t *report, const char *text) {
  size_t size = 0U;
  const char *json = kb_job_report_json(report, &size);
  return json != NULL && json[size] == '\0' && strstr(json, text) != NULL;
}

int main(void) {
  const char *manifest = "kb_fleet_run_test.yaml";
  const char *production_manifest = "kb_fleet_production_test.yaml";
  const char *production_artifact = "kb_fleet_production_system.img";
  kb_context_t *context = NULL;
  kb_error_t *error = NULL;
  kb_job_options_t options;

  CHECK(write_manifest(manifest));
  kb_job_options_init_sized(&options, sizeof(options));
  CHECK(options.struct_size == sizeof(options));
  CHECK(options.api_version == KB_API_VERSION);
  CHECK(options.timeout_ms == KB_WAIT_INFINITE);
  kb_job_options_init(NULL);

  CHECK(kb_context_create(NULL, &context, &error) == KB_OK);
  CHECK(context != NULL);
  CHECK(error == NULL);

  CHECK(kb_run_job_file_async(NULL, manifest, NULL, NULL, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(error != NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_job_wait(NULL, 0U) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_job_cancel(NULL) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_job_state(NULL) == KB_OPERATION_FAILED);
  CHECK(kb_job_error(NULL) == NULL);
  kb_job_release(NULL);
  kb_job_report_release(NULL);
  {
    size_t size = 99U;
    CHECK(kb_job_report_json(NULL, &size) == NULL);
    CHECK(size == 0U);
  }

  /* Blocking and async paths use the same state machine and progress trace. */
  {
    trace_buffer_t blocking_trace = {{0}, 0U};
    trace_buffer_t async_trace = {{0}, 0U};
    kb_job_report_t *blocking_report = NULL;
    kb_job_report_t *async_report = NULL;
    kb_job_t *job = NULL;
    kb_test_set_fleet_script(KB_TEST_SCRIPT_SUCCESS);
    options.progress_callback = trace_progress;
    options.progress_user_data = &blocking_trace;
    {
      const kb_status_t status = kb_run_job_file(
          context, manifest, &options, &blocking_report, &error);
      if (status != KB_OK && error != NULL) {
        fprintf(stderr, "blocking fleet status %d: %s\n", (int)status,
                kb_error_message(error));
      }
      CHECK(status == KB_OK);
    }
    CHECK(error == NULL);
    CHECK(blocking_report != NULL);
    CHECK(report_contains(blocking_report, "\"state\":\"succeeded\""));
    CHECK(report_contains(blocking_report, "\"succeeded\":1"));

    options.progress_user_data = &async_trace;
    CHECK(kb_run_job_file_async(context, manifest, &options, &job, &error) ==
          KB_OK);
    CHECK(job != NULL);
    CHECK(kb_job_wait(job, KB_WAIT_INFINITE) == KB_OK);
    CHECK(kb_job_state(job) == KB_OPERATION_SUCCEEDED);
    CHECK(kb_job_error(job) == NULL);
    CHECK(kb_job_get_report(job, &async_report, &error) == KB_OK);
    CHECK(error == NULL);
    CHECK(async_report != NULL);
    CHECK(strcmp(blocking_trace.text, async_trace.text) == 0);
    CHECK(report_contains(async_report, "\"state\":\"succeeded\""));

    /* The report owns its JSON independently of the job. */
    kb_job_release(job);
    CHECK(report_contains(async_report, "\"planSha256\":"));
    kb_job_report_release(async_report);
    kb_job_report_release(blocking_report);
  }

  /* Device failure keeps a terminal report while the call returns failure. */
  {
    kb_job_report_t *report = NULL;
    kb_test_set_fleet_script(KB_TEST_SCRIPT_FAILURE);
    options.progress_callback = NULL;
    options.progress_user_data = NULL;
    CHECK(kb_run_job_file(context, manifest, &options, &report, &error) ==
          KB_E_DEVICE_FAIL);
    CHECK(report != NULL);
    CHECK(error != NULL);
    CHECK(kb_error_status(error) == KB_E_DEVICE_FAIL);
    CHECK(report_contains(report, "\"state\":\"failed\""));
    CHECK(report_contains(report, "scripted device rejected flash"));
    kb_error_release(error);
    kb_job_report_release(report);
    error = NULL;
  }

  /* Cancellation is thread-safe, isolates the active context and publishes a
   * cancelled report. */
  {
    kb_job_t *job = NULL;
    kb_job_t *busy_job = NULL;
    kb_job_report_t *report = NULL;
    kb_test_set_fleet_script(KB_TEST_SCRIPT_WAIT_FOR_CANCEL);
    CHECK(kb_run_job_file_async(context, manifest, NULL, &job, &error) ==
          KB_OK);
    CHECK(kb_run_job_file_async(context, manifest, NULL, &busy_job, &error) ==
          KB_E_BUSY);
    CHECK(busy_job == NULL);
    CHECK(error != NULL);
    kb_error_release(error);
    error = NULL;
    CHECK(kb_job_cancel(job) == KB_OK);
    CHECK(kb_job_cancel(job) == KB_OK);
    CHECK(kb_job_wait(job, KB_WAIT_INFINITE) == KB_E_CANCELLED);
    CHECK(kb_job_state(job) == KB_OPERATION_CANCELLED);
    CHECK(kb_job_error(job) != NULL);
    CHECK(kb_error_status(kb_job_error(job)) == KB_E_CANCELLED);
    {
      const kb_status_t report_status = kb_job_get_report(job, &report, &error);
      if (report_status != KB_OK && error != NULL) {
        fprintf(stderr, "cancel report status %d: %s\n", (int)report_status,
                kb_error_message(error));
      }
      CHECK(report_status == KB_OK);
    }
    CHECK(report_contains(report, "\"state\":\"cancelled\""));
    kb_job_report_release(report);
    kb_job_release(job);
  }

  /* Releasing an in-flight job requests cancellation and drains its worker. */
  {
    kb_job_t *job = NULL;
    kb_test_set_fleet_script(KB_TEST_SCRIPT_WAIT_FOR_CANCEL);
    CHECK(kb_run_job_file_async(context, manifest, NULL, &job, &error) ==
          KB_OK);
    kb_job_release(job);
    CHECK(kb_run_job_file_async(context, manifest, NULL, &job, &error) ==
          KB_OK);
    CHECK(kb_job_cancel(job) == KB_OK);
    CHECK(kb_job_wait(job, KB_WAIT_INFINITE) == KB_E_CANCELLED);
    kb_job_release(job);
  }

  /* The default production preparation path performs artifact, device,
   * actor, scheduler and coordinator work. USB enumeration/open/probe are
   * scripted below; no hardware success is implied. */
  {
    kb_job_report_t *report = NULL;
    CHECK(write_production_fixture(production_manifest, production_artifact,
                                   32U, 1));
    kb_test_set_fleet_production_script(KB_TEST_PRODUCTION_SUCCESS);
    options.progress_callback = NULL;
    options.progress_user_data = NULL;
    CHECK(kb_run_job_file(context, production_manifest, &options, &report,
                          &error) == KB_OK);
    CHECK(error == NULL);
    CHECK(report != NULL);
    CHECK(report_contains(report, "\"state\":\"succeeded\""));
    CHECK(report_contains(report, "\"succeeded\":32"));
    CHECK(kb_test_fleet_dependency_calls() == 1U);
    CHECK(kb_test_fleet_permit_bind_count() == 32U);
    CHECK(kb_test_fleet_payload_count() == 32U);
    kb_job_report_release(report);
  }

  /* Selection failures remain pre-destructive and map to stable C statuses. */
  {
    kb_job_report_t *report = NULL;
    CHECK(write_production_fixture(production_manifest, production_artifact,
                                   1U, 1));
    kb_test_set_fleet_production_script(KB_TEST_PRODUCTION_NO_DEVICE);
    CHECK(kb_run_job_file(context, production_manifest, NULL, &report,
                          &error) == KB_E_NO_DEVICE);
    CHECK(error != NULL);
    CHECK(kb_error_status(error) == KB_E_NO_DEVICE);
    CHECK(report != NULL);
    CHECK(report_contains(report, "\"state\":\"failed\""));
    CHECK(kb_test_fleet_permit_bind_count() == 0U);
    kb_error_release(error);
    kb_job_report_release(report);
    error = NULL;

    report = NULL;
    kb_test_set_fleet_production_script(KB_TEST_PRODUCTION_AMBIGUOUS);
    CHECK(kb_run_job_file(context, production_manifest, NULL, &report,
                          &error) == KB_E_AMBIGUOUS_DEVICE);
    CHECK(error != NULL);
    CHECK(report != NULL);
    kb_error_release(error);
    kb_job_report_release(report);
    error = NULL;

    report = NULL;
    kb_test_set_fleet_production_script(KB_TEST_PRODUCTION_PRODUCT_MISMATCH);
    CHECK(kb_run_job_file(context, production_manifest, NULL, &report,
                          &error) == KB_E_INVALID_ARGUMENT);
    CHECK(error != NULL);
    CHECK(strstr(kb_error_message(error), "products do not match") != NULL);
    CHECK(report != NULL);
    kb_error_release(error);
    kb_job_report_release(report);
    error = NULL;
  }

  /* Artifact hash failure occurs before USB/device dependency acquisition. */
  {
    kb_job_report_t *report = NULL;
    CHECK(write_production_fixture(production_manifest, production_artifact,
                                   1U, 0));
    kb_test_set_fleet_production_script(KB_TEST_PRODUCTION_SUCCESS);
    CHECK(kb_run_job_file(context, production_manifest, NULL, &report,
                          &error) == KB_E_INVALID_ARGUMENT);
    CHECK(error != NULL);
    CHECK(report != NULL);
    CHECK(kb_test_fleet_dependency_calls() == 0U);
    CHECK(kb_test_fleet_permit_bind_count() == 0U);
    kb_error_release(error);
    kb_job_report_release(report);
    error = NULL;
  }

  /* A device-specific progress cancellation reaches the coordinator token and
   * publishes a drained cancelled report. */
  {
    kb_job_report_t *report = NULL;
    CHECK(write_production_fixture(production_manifest, production_artifact,
                                   2U, 1));
    kb_test_set_fleet_production_script(KB_TEST_PRODUCTION_SUCCESS);
    options.progress_callback = cancel_device_execution;
    options.progress_user_data = NULL;
    CHECK(kb_run_job_file(context, production_manifest, &options, &report,
                          &error) == KB_E_CANCELLED);
    CHECK(error != NULL);
    CHECK(report != NULL);
    CHECK(report_contains(report, "\"state\":\"cancelled\""));
    kb_error_release(error);
    kb_job_report_release(report);
    error = NULL;
    options.progress_callback = NULL;
  }

  /* continue isolates one device rejection and lets its sibling complete. */
  {
    kb_job_report_t *report = NULL;
    kb_test_set_fleet_production_script(KB_TEST_PRODUCTION_CONTINUE_FAILURE);
    CHECK(kb_run_job_file(context, production_manifest, NULL, &report,
                          &error) == KB_E_DEVICE_FAIL);
    CHECK(error != NULL);
    CHECK(report != NULL);
    CHECK(report_contains(report, "\"state\":\"partially_failed\""));
    CHECK(report_contains(report, "\"succeeded\":1"));
    CHECK(report_contains(report, "\"failed\":1"));
    CHECK(kb_test_fleet_permit_bind_count() == 2U);
    CHECK(kb_test_fleet_payload_count() == 2U);
    kb_error_release(error);
    kb_job_report_release(report);
    error = NULL;
  }

  kb_context_release(context);
  (void)remove(manifest);
  (void)remove(production_manifest);
  (void)remove(production_artifact);
  puts("PASS: C11 fleet run/cancel/report lifecycle");
  return 0;
}
