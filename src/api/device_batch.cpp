// SPDX-License-Identifier: MIT

#include <kairosboot/kairosboot.h>

#include "src/api/error_handle.hpp"
#include "src/api/operation_state.hpp"
#include "src/fleet/controller_scheduler.hpp"
#include "src/kairosboot_internal.hpp"
#include "src/transport/buffer_budget.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using kairosboot::api::OperationErrorPayload;
using kairosboot::api::OperationOutcome;
using kairosboot::api::OperationPhase;
using kairosboot::api::OperationState;

struct DeviceBatchResult final {
  kb_status_t status{KB_E_CANCELLED};
  std::string identifier;
  std::string message{"device operation was not started"};
};

struct DeviceBatchSharedState final {
  mutable std::mutex mutex;
  std::shared_ptr<const std::vector<DeviceBatchResult>> results;
  std::shared_ptr<const std::string> report_json;
};

struct ActiveChildren final {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<kb_operation_t*> operations;
  std::size_t finished_workers{};
  bool abort_requested{};
};

struct BatchTransferScheduling final {
  kairosboot::transport::TransferRingConfig config{};
  std::shared_ptr<kairosboot::fleet::WeightedControllerScheduler> scheduler;
  std::vector<std::shared_ptr<kairosboot::transport::TransferPermitProvider>>
      providers;
};

[[nodiscard]] OperationErrorPayload make_error(
    const kb_status_t status, std::string message,
    std::string identifier = {}) {
  return {
      .status = status,
      .message = std::move(message),
      .native_code = 0,
      .transfer_state = KB_TRANSFER_NOT_SENT,
      .device_identifier = std::move(identifier),
      .device_message = {},
      .command_messages = {},
      .inbound_expected = std::nullopt,
      .inbound_transferred = 0,
      .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
      .session_poisoned = false,
  };
}

[[nodiscard]] OperationErrorPayload copy_error(
    const kb_status_t fallback, const kb_error_t* error,
    const std::string_view fallback_identifier) {
  if (error == nullptr) {
    return make_error(fallback, kb_status_string(fallback),
                      std::string{fallback_identifier});
  }
  OperationErrorPayload result = make_error(
      kb_error_status(error), kb_error_message(error),
      kb_error_device_identifier(error));
  result.native_code = kb_error_native_code(error);
  result.transfer_state = kb_error_transfer_state(error);
  result.inbound_transferred = kb_error_inbound_transferred_bytes(error);
  result.inbound_transfer_state = kb_error_inbound_transfer_state(error);
  result.session_poisoned = kb_error_session_poisoned(error) != 0;
  const auto expected = kb_error_inbound_expected_bytes(error);
  if (expected != KB_FETCH_UNSPECIFIED) {
    result.inbound_expected = expected;
  }
  if (result.device_identifier.empty()) {
    result.device_identifier = fallback_identifier;
  }
  return result;
}

[[nodiscard]] std::string json_escape(const std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    switch (character) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20U) {
          result += "\\u00";
          result += hex[(character >> 4U) & 0x0fU];
          result += hex[character & 0x0fU];
        } else {
          result += static_cast<char>(character);
        }
        break;
    }
  }
  return result;
}

[[nodiscard]] std::string make_report_json(
    const std::vector<DeviceBatchResult>& results) {
  std::ostringstream stream;
  stream << "{\"schemaVersion\":1,\"devices\":[";
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (index != 0U) {
      stream << ',';
    }
    const auto& result = results[index];
    stream << "{\"index\":" << index << ",\"identifier\":\""
           << json_escape(result.identifier) << "\",\"status\":"
           << result.status << ",\"message\":\""
           << json_escape(result.message) << "\"}";
  }
  stream << "]}";
  return stream.str();
}

void publish_results(
    const std::shared_ptr<DeviceBatchSharedState>& shared,
    const std::vector<DeviceBatchResult>& results) {
  auto published_results =
      std::make_shared<const std::vector<DeviceBatchResult>>(results);
  auto json = std::make_shared<const std::string>(make_report_json(results));
  std::scoped_lock lock(shared->mutex);
  shared->results = std::move(published_results);
  shared->report_json = std::move(json);
}

[[nodiscard]] std::expected<BatchTransferScheduling, OperationErrorPayload>
prepare_transfer_scheduling(const std::vector<kb_device_t*>& devices) {
  try {
    BatchTransferScheduling result;
    result.providers.resize(devices.size());

    std::vector<kairosboot::api::DeviceBatchSchedulingInfo> infos;
    infos.reserve(devices.size());
    bool has_usb_device = false;
    for (const auto* device : devices) {
      auto info = kairosboot::api::device_batch_scheduling_info(device);
      if (info.device_key.empty()) {
        return std::unexpected(make_error(
            KB_E_INTERNAL, "device batch contains a Device without a stable identity"));
      }
      has_usb_device = has_usb_device || !info.controller_id.empty();
      infos.push_back(std::move(info));
    }
    if (!has_usb_device) {
      return result;
    }

    result.scheduler =
        std::make_shared<kairosboot::fleet::WeightedControllerScheduler>(
            kairosboot::transport::process_usb_buffer_budget());
    for (std::size_t index = 0; index < infos.size(); ++index) {
      const auto& info = infos[index];
      if (info.controller_id.empty()) {
        continue;
      }
      if (!result.scheduler->add_flow({
              .device_id = info.device_key,
              .controller_id = info.controller_id,
              .weight = 1U,
              .bytes_remaining = std::numeric_limits<std::uint64_t>::max(),
              .max_outstanding = result.config.depth,
          })) {
        return std::unexpected(make_error(
            KB_E_INTERNAL,
            "unable to register a USB Device with the batch scheduler",
            info.device_key));
      }
    }
    for (std::size_t index = 0; index < infos.size(); ++index) {
      const auto& info = infos[index];
      if (info.controller_id.empty()) {
        continue;
      }
      result.providers[index] = result.scheduler->make_permit_provider(
          info.device_key, result.config.depth);
      if (result.providers[index] == nullptr) {
        return std::unexpected(make_error(
            KB_E_INTERNAL,
            "unable to bind a USB Device to the batch scheduler",
            info.device_key));
      }
    }
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(make_error(
        KB_E_OUT_OF_MEMORY, "unable to allocate USB batch scheduling state"));
  } catch (...) {
    return std::unexpected(make_error(
        KB_E_INTERNAL, "unable to prepare USB batch scheduling state"));
  }
}

[[nodiscard]] kb_operation_state_t public_state(
    const OperationPhase phase) noexcept {
  switch (phase) {
    case OperationPhase::Created: return KB_OPERATION_CREATED;
    case OperationPhase::Running: return KB_OPERATION_RUNNING;
    case OperationPhase::Succeeded: return KB_OPERATION_SUCCEEDED;
    case OperationPhase::Failed: return KB_OPERATION_FAILED;
    case OperationPhase::Cancelled: return KB_OPERATION_CANCELLED;
  }
  return KB_OPERATION_FAILED;
}

void clear_error(kb_error_t** error) noexcept {
  if (error != nullptr) {
    *error = nullptr;
  }
}

kb_status_t fail(kb_error_t** error, const OperationErrorPayload& payload) noexcept {
  if (error != nullptr) {
    try {
      *error = new kb_error{
          .status = payload.status,
          .message = payload.message,
          .device_identifier = payload.device_identifier,
          .native_code = payload.native_code,
          .transfer_state = payload.transfer_state,
          .device_message = payload.device_message,
          .command_messages = payload.command_messages,
          .inbound_expected = payload.inbound_expected,
          .inbound_transferred = payload.inbound_transferred,
          .inbound_transfer_state = payload.inbound_transfer_state,
          .session_poisoned = payload.session_poisoned,
      };
    } catch (...) {
      *error = nullptr;
    }
  }
  return payload.status;
}

kb_status_t fail(kb_error_t** error, const kb_status_t status,
                 std::string message) noexcept {
  return fail(error, make_error(status, std::move(message)));
}

[[nodiscard]] bool valid_options(
    const kb_device_batch_options_t* options) noexcept {
  return options == nullptr ||
         (options->struct_size >= KB_DEVICE_BATCH_OPTIONS_V1_SIZE &&
          options->api_version == KB_API_VERSION);
}

[[nodiscard]] kb_device_batch_options_t options_or_default(
    const kb_device_batch_options_t* options,
    const std::size_t device_count) noexcept {
  kb_device_batch_options_t result{};
  result.struct_size = sizeof(result);
  result.api_version = KB_API_VERSION;
  result.timeout_ms = KB_WAIT_INFINITE;
  result.max_parallel_devices = static_cast<std::uint32_t>(
      std::min<std::size_t>(device_count, 32U));
  result.continue_on_error = 1;
  if (options != nullptr) {
    result = *options;
    if (result.max_parallel_devices == 0U) {
      result.max_parallel_devices = static_cast<std::uint32_t>(
          std::min<std::size_t>(device_count, 32U));
    }
  }
  result.max_parallel_devices = static_cast<std::uint32_t>(
      std::min<std::size_t>(result.max_parallel_devices, device_count));
  return result;
}

[[nodiscard]] bool publish_progress(
    const kb_device_batch_options_t& options, std::mutex& callback_mutex,
    const std::size_t completed, const std::size_t total,
    const std::string_view stage, const std::string_view identifier) noexcept {
  if (options.progress_callback == nullptr) {
    return true;
  }
  const std::string stage_storage{stage};
  const std::string identifier_storage{identifier};
  const kb_progress_t progress{
      sizeof(kb_progress_t), KB_API_VERSION,
      static_cast<std::uint64_t>(completed), static_cast<std::uint64_t>(total),
      stage_storage.c_str(), identifier_storage.c_str(),
  };
  try {
    const std::scoped_lock lock(callback_mutex);
    return options.progress_callback(&progress, options.progress_user_data) !=
           KB_PROGRESS_CANCEL;
  } catch (...) {
    return false;
  }
}

void cancel_active(const std::shared_ptr<ActiveChildren>& active) noexcept {
  std::scoped_lock lock(active->mutex);
  active->abort_requested = true;
  for (auto* operation : active->operations) {
    static_cast<void>(kb_operation_cancel(operation));
  }
  active->changed.notify_all();
}

void remove_active(const std::shared_ptr<ActiveChildren>& active,
                   kb_operation_t* operation) noexcept {
  std::scoped_lock lock(active->mutex);
  const auto position = std::find(active->operations.begin(),
                                  active->operations.end(), operation);
  if (position != active->operations.end()) {
    active->operations.erase(position);
  }
}

[[nodiscard]] OperationOutcome run_batch_task(
    const std::vector<kb_device_t*>& devices,
    const kb_device_batch_start_callback_t start_callback,
    void* const start_user_data,
    const kb_device_batch_options_t options,
    const std::shared_ptr<DeviceBatchSharedState>& shared,
    OperationState::TaskContext& task_context) {
  std::vector<DeviceBatchResult> results(devices.size());
  for (std::size_t index = 0; index < devices.size(); ++index) {
    const char* identifier = kb_device_identifier(devices[index]);
    results[index].identifier = identifier == nullptr ? "" : identifier;
  }

  auto scheduling = prepare_transfer_scheduling(devices);
  if (!scheduling) {
    for (auto& result : results) {
      result.status = scheduling.error().status;
      result.message = scheduling.error().message;
    }
    publish_results(shared, results);
    return OperationOutcome::failed(std::move(scheduling.error()));
  }

  auto active = std::make_shared<ActiveChildren>();
  auto cancellation = task_context.register_cancellation_hook(
      [active] { cancel_active(active); });
  std::atomic_size_t next_index{0U};
  std::atomic_size_t completed{0U};
  std::atomic_bool stop_starting{false};
  std::vector<std::uint8_t> claimed(devices.size(), 0U);
  std::mutex callback_mutex;

  const std::size_t worker_count = options.max_parallel_devices;
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (std::size_t worker_index = 0; worker_index < worker_count;
       ++worker_index) {
    workers.emplace_back([&] {
      for (;;) {
        if (task_context.cancel_requested() ||
            stop_starting.load(std::memory_order_acquire)) {
          break;
        }
        const std::size_t index =
            next_index.fetch_add(1U, std::memory_order_acq_rel);
        if (index >= devices.size()) {
          break;
        }
        claimed[index] = 1U;
        auto& result = results[index];
        if (!publish_progress(options, callback_mutex,
                              completed.load(std::memory_order_acquire),
                              devices.size(), "starting", result.identifier)) {
          stop_starting.store(true, std::memory_order_release);
          cancel_active(active);
          result.status = KB_E_CANCELLED;
          result.message = "batch cancelled by progress callback";
          break;
        }

        kb_operation_t* operation = nullptr;
        kb_error_t* start_error = nullptr;
        std::optional<kairosboot::api::ScopedDeviceBatchTransferPermits>
            permit_scope;
        if (scheduling->providers[index] != nullptr) {
          permit_scope.emplace(devices[index], scheduling->providers[index],
                               scheduling->config);
        }
        const auto started = start_callback(devices[index], index,
                                            start_user_data, &operation,
                                            &start_error);
        permit_scope.reset();
        if (started != KB_OK || operation == nullptr) {
          const auto payload = copy_error(
              started == KB_OK ? KB_E_INTERNAL : started, start_error,
              result.identifier);
          kb_error_release(start_error);
          result.status = payload.status;
          result.message = payload.message;
          if (options.continue_on_error == 0) {
            stop_starting.store(true, std::memory_order_release);
            cancel_active(active);
          }
        } else {
          {
            std::scoped_lock lock(active->mutex);
            active->operations.push_back(operation);
            if (active->abort_requested || task_context.cancel_requested()) {
              static_cast<void>(kb_operation_cancel(operation));
            }
          }
          const auto status = kb_operation_wait(operation, KB_WAIT_INFINITE);
          const auto payload = copy_error(status, kb_operation_error(operation),
                                          result.identifier);
          remove_active(active, operation);
          kb_operation_release(operation);
          result.status = status;
          result.message = status == KB_OK ? "completed" : payload.message;
          if (status != KB_OK && options.continue_on_error == 0) {
            stop_starting.store(true, std::memory_order_release);
            cancel_active(active);
          }
        }

        const auto now_completed =
            completed.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        static_cast<void>(publish_progress(
            options, callback_mutex, now_completed, devices.size(),
            result.status == KB_OK ? "completed" : "failed",
            result.identifier));
      }
      {
        std::scoped_lock lock(active->mutex);
        ++active->finished_workers;
      }
      active->changed.notify_all();
    });
  }

  bool timed_out = false;
  {
    std::unique_lock lock(active->mutex);
    const auto all_finished = [&] {
      return active->finished_workers == worker_count;
    };
    if (options.timeout_ms == KB_WAIT_INFINITE) {
      active->changed.wait(lock, all_finished);
    } else if (!active->changed.wait_for(
                   lock, std::chrono::milliseconds{options.timeout_ms},
                   all_finished)) {
      timed_out = true;
    }
  }
  if (timed_out || task_context.cancel_requested()) {
    stop_starting.store(true, std::memory_order_release);
    cancel_active(active);
  }
  for (auto& worker : workers) {
    worker.join();
  }
  cancellation.reset();

  const kb_status_t skipped_status = timed_out ? KB_E_TIMEOUT : KB_E_CANCELLED;
  const char* skipped_message = timed_out
      ? "device operation was not started before the batch deadline"
      : "device operation was not started because the batch stopped";
  for (std::size_t index = 0U; index < results.size(); ++index) {
    if (claimed[index] == 0U) {
      results[index].status = skipped_status;
      results[index].message = skipped_message;
    }
  }

  publish_results(shared, results);

  if (timed_out) {
    return OperationOutcome::failed(
        make_error(KB_E_TIMEOUT, "device batch deadline expired"));
  }
  if (task_context.cancel_requested()) {
    return OperationOutcome::cancelled(
        make_error(KB_E_CANCELLED, "device batch was cancelled"));
  }
  const auto failed = std::find_if(results.begin(), results.end(),
                                   [](const DeviceBatchResult& result) {
    return result.status != KB_OK;
  });
  if (failed != results.end()) {
    return OperationOutcome::failed(
        make_error(failed->status, failed->message, failed->identifier));
  }
  return OperationOutcome::succeeded();
}

struct FlashBatchStart final {
  std::string partition;
  std::string file_path;
  std::optional<kb_flash_options_t> options;
  std::string slot;
  std::string active_slot;
};

struct UpdateBatchStart final {
  std::string package_path;
  std::optional<kb_update_options_t> options;
  std::string slot;
  std::string active_slot;
};

template <typename Options>
[[nodiscard]] std::optional<Options> copy_options(const Options* source) {
  if (source == nullptr) {
    return std::nullopt;
  }
  Options result{};
  const auto bytes = std::min<std::size_t>(source->struct_size, sizeof(Options));
  std::memcpy(&result, source, bytes);
  return result;
}

kb_status_t KB_CALL start_flash(kb_device_t* device, size_t, void* user_data,
                                kb_operation_t** operation,
                                kb_error_t** error) {
  auto& start = *static_cast<FlashBatchStart*>(user_data);
  return kb_flash_file_async(
      device, start.partition.c_str(), start.file_path.c_str(),
      start.options ? &*start.options : nullptr, operation, error);
}

kb_status_t KB_CALL start_update(kb_device_t* device, size_t, void* user_data,
                                 kb_operation_t** operation,
                                 kb_error_t** error) {
  auto& start = *static_cast<UpdateBatchStart*>(user_data);
  return kb_update_package_async(
      device, start.package_path.c_str(),
      start.options ? &*start.options : nullptr, operation, error);
}

}  // namespace

struct kb_device_batch_report {
  std::shared_ptr<const std::vector<DeviceBatchResult>> results;
  std::shared_ptr<const std::string> json;
};

struct kb_device_batch {
  kb_device_batch(std::unique_ptr<OperationState> operation,
                  std::shared_ptr<DeviceBatchSharedState> result_state,
                  std::vector<kb_device_t*> retained_devices)
      : state(std::move(operation)), shared(std::move(result_state)),
        devices(std::move(retained_devices)) {}

  ~kb_device_batch() {
    state.reset();
    for (auto* device : devices) {
      kb_device_release(device);
    }
  }

  std::unique_ptr<OperationState> state;
  std::shared_ptr<DeviceBatchSharedState> shared;
  std::vector<kb_device_t*> devices;
  std::shared_ptr<void> owned_start_state;
  mutable std::mutex error_mutex;
  mutable std::unique_ptr<kb_error> public_error;
};

namespace {

[[nodiscard]] const kb_error_t* materialize_error(
    const kb_device_batch_t* batch) noexcept {
  if (batch == nullptr || batch->state == nullptr) {
    return nullptr;
  }
  const auto payload = batch->state->error();
  if (!payload.has_value()) {
    return nullptr;
  }
  std::scoped_lock lock(batch->error_mutex);
  try {
    batch->public_error = std::make_unique<kb_error>(kb_error{
        .status = payload->status,
        .message = payload->message,
        .device_identifier = payload->device_identifier,
        .native_code = payload->native_code,
        .transfer_state = payload->transfer_state,
        .device_message = payload->device_message,
        .command_messages = payload->command_messages,
        .inbound_expected = payload->inbound_expected,
        .inbound_transferred = payload->inbound_transferred,
        .inbound_transfer_state = payload->inbound_transfer_state,
        .session_poisoned = payload->session_poisoned,
    });
  } catch (...) {
    return nullptr;
  }
  return batch->public_error.get();
}

kb_status_t run_blocking(kb_device_batch_t* batch,
                         kb_device_batch_report_t** report,
                         kb_error_t** error) {
  const auto waited = kb_device_batch_wait(batch, KB_WAIT_INFINITE);
  kb_error_t* report_error = nullptr;
  const auto extracted =
      kb_device_batch_get_report(batch, report, &report_error);
  kb_error_release(report_error);
  if (waited != KB_OK) {
    const auto payload = batch->state->error();
    if (payload.has_value()) {
      static_cast<void>(fail(error, *payload));
    } else {
      static_cast<void>(fail(error, waited, "device batch failed"));
    }
  } else if (extracted != KB_OK) {
    static_cast<void>(fail(error, extracted,
                           "device batch report is unavailable"));
  }
  kb_device_batch_release(batch);
  return waited == KB_OK ? extracted : waited;
}

}  // namespace

extern "C" {

void KB_CALL kb_device_batch_options_init(
    kb_device_batch_options_t* options) {
  kb_device_batch_options_init_sized(options,
                                     KB_DEVICE_BATCH_OPTIONS_V1_SIZE);
}

void KB_CALL kb_device_batch_options_init_sized(
    kb_device_batch_options_t* options, const uint32_t struct_size) {
  kairosboot::api::detail::initialize_struct_header(options, struct_size);
  kairosboot::api::detail::initialize_field(
      options, struct_size, offsetof(kb_device_batch_options_t, timeout_ms),
      uint32_t{KB_WAIT_INFINITE});
  kairosboot::api::detail::initialize_field(
      options, struct_size,
      offsetof(kb_device_batch_options_t, continue_on_error), int32_t{1});
}

kb_status_t KB_CALL kb_device_batch_run_async(
    kb_device_t* const* devices, const size_t device_count,
    const kb_device_batch_start_callback_t start_callback,
    void* const start_user_data,
    const kb_device_batch_options_t* options_or_null,
    kb_device_batch_t** batch, kb_error_t** error) {
  clear_error(error);
  if (batch == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch output pointer must not be null");
  }
  *batch = nullptr;
  if (devices == nullptr || device_count == 0U) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch requires at least one Device");
  }
  if (device_count > 1024U) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch supports at most 1024 Devices");
  }
  if (start_callback == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch start callback must not be null");
  }
  if (!valid_options(options_or_null)) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch options have an incompatible size or API version");
  }

  std::vector<kb_device_t*> retained;
  std::vector<std::string> device_keys;
  try {
    retained.reserve(device_count);
    device_keys.reserve(device_count);
    for (std::size_t index = 0; index < device_count; ++index) {
      if (devices[index] == nullptr) {
        for (auto* device : retained) {
          kb_device_release(device);
        }
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "device batch contains a null Device");
      }
      if (std::find(retained.begin(), retained.end(), devices[index]) !=
          retained.end()) {
        for (auto* device : retained) {
          kb_device_release(device);
        }
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "device batch contains the same Device more than once");
      }
      auto scheduling_info =
          kairosboot::api::device_batch_scheduling_info(devices[index]);
      if (scheduling_info.device_key.empty()) {
        for (auto* device : retained) {
          kb_device_release(device);
        }
        return fail(error, KB_E_INTERNAL,
                    "device batch contains a Device without a stable identity");
      }
      if (std::find(device_keys.begin(), device_keys.end(),
                    scheduling_info.device_key) != device_keys.end()) {
        for (auto* device : retained) {
          kb_device_release(device);
        }
        return fail(error, KB_E_INVALID_ARGUMENT,
                    "device batch contains the same physical Device more than once");
      }
      static_cast<void>(kb_device_retain(devices[index]));
      retained.push_back(devices[index]);
      device_keys.push_back(std::move(scheduling_info.device_key));
    }
    const auto options = options_or_default(options_or_null, device_count);
    auto shared = std::make_shared<DeviceBatchSharedState>();
    const auto task_devices = retained;
    auto task = [task_devices, start_callback, start_user_data, options, shared](
                    OperationState::TaskContext& context) {
      return run_batch_task(task_devices, start_callback, start_user_data,
                            options, shared, context);
    };
    auto operation = std::make_unique<OperationState>(std::move(task));
    auto result = std::make_unique<kb_device_batch>(
        std::move(operation), std::move(shared), std::move(retained));
    if (!result->state->start()) {
      return fail(error, KB_E_INTERNAL, "unable to start the device batch");
    }
    *batch = result.release();
    return KB_OK;
  } catch (const std::bad_alloc&) {
    for (auto* device : retained) {
      kb_device_release(device);
    }
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the device batch");
  } catch (...) {
    for (auto* device : retained) {
      kb_device_release(device);
    }
    return fail(error, KB_E_INTERNAL, "unable to create the device batch");
  }
}

kb_status_t KB_CALL kb_device_batch_run(
    kb_device_t* const* devices, const size_t device_count,
    const kb_device_batch_start_callback_t start_callback,
    void* const start_user_data,
    const kb_device_batch_options_t* options_or_null,
    kb_device_batch_report_t** report, kb_error_t** error) {
  clear_error(error);
  if (report == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch report output pointer must not be null");
  }
  *report = nullptr;
  kb_device_batch_t* batch = nullptr;
  const auto started = kb_device_batch_run_async(
      devices, device_count, start_callback, start_user_data, options_or_null,
      &batch, error);
  return started == KB_OK ? run_blocking(batch, report, error) : started;
}

kb_status_t KB_CALL kb_flash_file_batch_async(
    kb_device_t* const* devices, const size_t device_count,
    const char* partition, const char* file_path,
    const kb_flash_options_t* flash_options_or_null,
    const kb_device_batch_options_t* batch_options_or_null,
    kb_device_batch_t** batch, kb_error_t** error) {
  clear_error(error);
  if (partition == nullptr || partition[0] == '\0' || file_path == nullptr ||
      file_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "batch flash requires a partition and file path");
  }
  try {
    auto start = std::make_shared<FlashBatchStart>();
    start->partition = partition;
    start->file_path = file_path;
    start->options = copy_options(flash_options_or_null);
    if (start->options) {
      if (start->options->slot != nullptr) {
        start->slot = start->options->slot;
        start->options->slot = start->slot.c_str();
      }
      if (start->options->active_slot != nullptr) {
        start->active_slot = start->options->active_slot;
        start->options->active_slot = start->active_slot.c_str();
      }
    }
    const auto status = kb_device_batch_run_async(
        devices, device_count, &start_flash, start.get(),
        batch_options_or_null, batch, error);
    if (status == KB_OK) {
      (*batch)->owned_start_state = std::move(start);
    }
    return status;
  } catch (const std::bad_alloc&) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate batch flash inputs");
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to prepare batch flash inputs");
  }
}

kb_status_t KB_CALL kb_flash_file_batch(
    kb_device_t* const* devices, const size_t device_count,
    const char* partition, const char* file_path,
    const kb_flash_options_t* flash_options_or_null,
    const kb_device_batch_options_t* batch_options_or_null,
    kb_device_batch_report_t** report, kb_error_t** error) {
  if (report == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch report output pointer must not be null");
  }
  *report = nullptr;
  kb_device_batch_t* batch = nullptr;
  const auto started = kb_flash_file_batch_async(
      devices, device_count, partition, file_path, flash_options_or_null,
      batch_options_or_null, &batch, error);
  return started == KB_OK ? run_blocking(batch, report, error) : started;
}

kb_status_t KB_CALL kb_update_package_batch_async(
    kb_device_t* const* devices, const size_t device_count,
    const char* package_path,
    const kb_update_options_t* update_options_or_null,
    const kb_device_batch_options_t* batch_options_or_null,
    kb_device_batch_t** batch, kb_error_t** error) {
  clear_error(error);
  if (package_path == nullptr || package_path[0] == '\0') {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "batch update requires a package path");
  }
  try {
    auto start = std::make_shared<UpdateBatchStart>();
    start->package_path = package_path;
    start->options = copy_options(update_options_or_null);
    if (start->options) {
      if (start->options->slot != nullptr) {
        start->slot = start->options->slot;
        start->options->slot = start->slot.c_str();
      }
      if (start->options->active_slot != nullptr) {
        start->active_slot = start->options->active_slot;
        start->options->active_slot = start->active_slot.c_str();
      }
    }
    const auto status = kb_device_batch_run_async(
        devices, device_count, &start_update, start.get(),
        batch_options_or_null, batch, error);
    if (status == KB_OK) {
      (*batch)->owned_start_state = std::move(start);
    }
    return status;
  } catch (const std::bad_alloc&) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate batch update inputs");
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to prepare batch update inputs");
  }
}

kb_status_t KB_CALL kb_update_package_batch(
    kb_device_t* const* devices, const size_t device_count,
    const char* package_path,
    const kb_update_options_t* update_options_or_null,
    const kb_device_batch_options_t* batch_options_or_null,
    kb_device_batch_report_t** report, kb_error_t** error) {
  if (report == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch report output pointer must not be null");
  }
  *report = nullptr;
  kb_device_batch_t* batch = nullptr;
  const auto started = kb_update_package_batch_async(
      devices, device_count, package_path, update_options_or_null,
      batch_options_or_null, &batch, error);
  return started == KB_OK ? run_blocking(batch, report, error) : started;
}

kb_status_t KB_CALL kb_device_batch_wait(kb_device_batch_t* batch,
                                         const uint32_t timeout_ms) {
  if (batch == nullptr || batch->state == nullptr) {
    return KB_E_INVALID_ARGUMENT;
  }
  if (timeout_ms == KB_WAIT_INFINITE) {
    batch->state->wait();
  } else if (batch->state->wait_for(std::chrono::milliseconds{timeout_ms}) ==
             kairosboot::api::OperationWaitResult::Timeout) {
    return KB_E_TIMEOUT;
  }
  return batch->state->status();
}

kb_status_t KB_CALL kb_device_batch_cancel(kb_device_batch_t* batch) {
  if (batch == nullptr || batch->state == nullptr) {
    return KB_E_INVALID_ARGUMENT;
  }
  batch->state->cancel();
  return KB_OK;
}

kb_operation_state_t KB_CALL kb_device_batch_state(
    const kb_device_batch_t* batch) {
  return batch == nullptr || batch->state == nullptr
      ? KB_OPERATION_FAILED
      : public_state(batch->state->phase());
}

const kb_error_t* KB_CALL kb_device_batch_error(
    const kb_device_batch_t* batch) {
  return materialize_error(batch);
}

kb_status_t KB_CALL kb_device_batch_get_report(
    const kb_device_batch_t* batch, kb_device_batch_report_t** report,
    kb_error_t** error) {
  clear_error(error);
  if (report == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch report output pointer must not be null");
  }
  *report = nullptr;
  if (batch == nullptr || batch->state == nullptr || batch->shared == nullptr) {
    return fail(error, KB_E_INVALID_ARGUMENT,
                "device batch must not be null");
  }
  const auto phase = batch->state->phase();
  if (phase == OperationPhase::Created || phase == OperationPhase::Running) {
    return fail(error, KB_E_BUSY, "device batch has not completed");
  }
  try {
    std::shared_ptr<const std::vector<DeviceBatchResult>> results;
    std::shared_ptr<const std::string> json;
    {
      std::scoped_lock lock(batch->shared->mutex);
      results = batch->shared->results;
      json = batch->shared->report_json;
    }
    if (results == nullptr || json == nullptr) {
      return fail(error, KB_E_INTERNAL,
                  "device batch completed without a report");
    }
    auto value = std::make_unique<kb_device_batch_report>();
    value->results = std::move(results);
    value->json = std::move(json);
    *report = value.release();
    return KB_OK;
  } catch (const std::bad_alloc&) {
    return fail(error, KB_E_OUT_OF_MEMORY,
                "unable to allocate the device batch report");
  } catch (...) {
    return fail(error, KB_E_INTERNAL,
                "unable to materialize the device batch report");
  }
}

void KB_CALL kb_device_batch_release(kb_device_batch_t* batch) {
  delete batch;
}

size_t KB_CALL kb_device_batch_report_count(
    const kb_device_batch_report_t* report) {
  return report == nullptr || report->results == nullptr
      ? 0U : report->results->size();
}

kb_status_t KB_CALL kb_device_batch_report_status(
    const kb_device_batch_report_t* report, const size_t device_index) {
  return report == nullptr || report->results == nullptr ||
                 device_index >= report->results->size()
      ? KB_E_INVALID_ARGUMENT : (*report->results)[device_index].status;
}

const char* KB_CALL kb_device_batch_report_identifier(
    const kb_device_batch_report_t* report, const size_t device_index) {
  return report == nullptr || report->results == nullptr ||
                 device_index >= report->results->size()
      ? nullptr : (*report->results)[device_index].identifier.c_str();
}

const char* KB_CALL kb_device_batch_report_message(
    const kb_device_batch_report_t* report, const size_t device_index) {
  return report == nullptr || report->results == nullptr ||
                 device_index >= report->results->size()
      ? nullptr : (*report->results)[device_index].message.c_str();
}

const char* KB_CALL kb_device_batch_report_json(
    const kb_device_batch_report_t* report, size_t* size) {
  if (size != nullptr) {
    *size = 0U;
  }
  if (report == nullptr || report->json == nullptr) {
    return nullptr;
  }
  if (size != nullptr) {
    *size = report->json->size();
  }
  return report->json->c_str();
}

void KB_CALL kb_device_batch_report_release(
    kb_device_batch_report_t* report) {
  delete report;
}

}  // extern "C"
