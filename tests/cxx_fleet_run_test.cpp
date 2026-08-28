// SPDX-License-Identifier: MIT

#include <kairosboot/kairosboot.hpp>

#include <concepts>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" void kb_test_set_fleet_script(int mode);

namespace {

struct CheckFailure final {
  const char *condition;
  int line;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw CheckFailure{#condition, __LINE__};                                \
    }                                                                          \
  } while (false)

enum : int {
  kSuccess = 0,
  kFailure = 1,
  kWaitForCancel = 2,
};

static_assert(!std::is_copy_constructible_v<kairosboot::Job>);
static_assert(!std::is_copy_assignable_v<kairosboot::Job>);
static_assert(std::is_nothrow_move_constructible_v<kairosboot::Job>);
static_assert(std::is_nothrow_move_assignable_v<kairosboot::Job>);
static_assert(!std::is_copy_constructible_v<kairosboot::JobReport>);
static_assert(std::is_nothrow_move_constructible_v<kairosboot::JobReport>);
static_assert(requires(kairosboot::Job &job, std::stop_token stop) {
  { job.wait(stop) } -> std::same_as<std::expected<void, kairosboot::Error>>;
  { job.cancel() } -> std::same_as<std::expected<void, kairosboot::Error>>;
  { job.error() } -> std::same_as<std::optional<kairosboot::Error>>;
  { job.report() } ->
      std::same_as<std::expected<kairosboot::JobReport, kairosboot::Error>>;
});

constexpr std::string_view kManifest =
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

void write_manifest(const std::filesystem::path &path) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(kManifest.data(), static_cast<std::streamsize>(kManifest.size()));
  CHECK(output.good());
}

bool contains(const kairosboot::JobReport &report,
              const std::string_view value) {
  return report.json().find(value) != std::string_view::npos;
}

} // namespace

int main() {
  try {
    const std::filesystem::path manifest{"kb_cxx_fleet_run.yaml"};
    write_manifest(manifest);
    auto context = kairosboot::Context::create();
    CHECK(context);

    std::vector<std::string> blocking_trace;
    std::vector<std::string> async_trace;
    kb_test_set_fleet_script(kSuccess);
    kairosboot::JobOptions blocking_options;
    blocking_options.progress = [&](const kairosboot::Progress &progress) {
      blocking_trace.emplace_back(progress.stage);
      return kairosboot::ProgressAction::Continue;
    };
    auto blocking = context->run_job_file(manifest, blocking_options);
    CHECK(blocking);
    CHECK(contains(*blocking, "\"state\":\"succeeded\""));

    kairosboot::JobOptions async_options;
    async_options.progress = [&](const kairosboot::Progress &progress) {
      async_trace.emplace_back(progress.stage);
      return kairosboot::ProgressAction::Continue;
    };
    auto async = context->run_job_file_async(manifest, async_options);
    CHECK(async);
    kairosboot::Job moved{std::move(*async)};
    CHECK(moved.wait());
    auto async_report = moved.report();
    CHECK(async_report);
    CHECK(contains(*async_report, "\"state\":\"succeeded\""));
    CHECK(blocking_trace == async_trace);

    kb_test_set_fleet_script(kFailure);
    auto failed = context->run_job_file_async(manifest);
    CHECK(failed);
    auto failed_wait = failed->wait();
    CHECK(!failed_wait);
    CHECK(failed_wait.error().status() == KB_E_DEVICE_FAIL);
    CHECK(failed->error());
    CHECK(failed->error()->status() == KB_E_DEVICE_FAIL);
    auto failed_report = failed->report();
    CHECK(failed_report);
    CHECK(contains(*failed_report, "\"state\":\"failed\""));
    CHECK(contains(*failed_report, "scripted device rejected flash"));

    kb_test_set_fleet_script(kWaitForCancel);
    auto cancelled = context->run_job_file_async(manifest);
    CHECK(cancelled);
    std::stop_source stop;
    CHECK(stop.request_stop());
    auto cancelled_wait = cancelled->wait(stop.get_token());
    CHECK(!cancelled_wait);
    CHECK(cancelled_wait.error().status() == KB_E_CANCELLED);
    auto cancelled_report = cancelled->report();
    CHECK(cancelled_report);
    CHECK(contains(*cancelled_report, "\"state\":\"cancelled\""));

    std::error_code ignored;
    std::filesystem::remove(manifest, ignored);
    std::cout << "PASS: C++23 fleet run/cancel/report parity\n";
    return 0;
  } catch (const CheckFailure &failure) {
    std::cerr << "check failed at line " << failure.line << ": "
              << failure.condition << '\n';
    return failure.line;
  }
}
