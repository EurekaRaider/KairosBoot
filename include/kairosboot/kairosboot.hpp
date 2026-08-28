#ifndef KAIROSBOOT_KAIROSBOOT_HPP
#define KAIROSBOOT_KAIROSBOOT_HPP

#include <kairosboot/kairosboot.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kairosboot {

enum class ProgressAction : std::uint8_t {
  Continue,
  Cancel,
};

struct Progress {
  std::uint64_t bytes_completed{};
  std::uint64_t bytes_total{};
  // These views are valid only for the duration of the progress callback.
  std::string_view stage;
  std::string_view device_identifier;
};

using FlashProgress = Progress;

enum class CommandMessageKind : std::uint8_t {
  Info,
  Text,
};

struct CommandMessage {
  CommandMessageKind kind{CommandMessageKind::Info};
  std::vector<std::byte> payload;
};

struct CommandMessageView {
  CommandMessageKind kind{CommandMessageKind::Info};
  std::span<const std::byte> payload;
};

enum class RebootTarget : kb_reboot_target_t {
  System = KB_REBOOT_SYSTEM,
  Bootloader = KB_REBOOT_BOOTLOADER,
  Recovery = KB_REBOOT_RECOVERY,
  Fastboot = KB_REBOOT_FASTBOOT,
};

enum class FlashingCommand : kb_flashing_command_t {
  Lock = KB_FLASHING_LOCK,
  Unlock = KB_FLASHING_UNLOCK,
  LockCritical = KB_FLASHING_LOCK_CRITICAL,
  UnlockCritical = KB_FLASHING_UNLOCK_CRITICAL,
  GetUnlockAbility = KB_FLASHING_GET_UNLOCK_ABILITY,
};

enum class GsiCommand : kb_gsi_command_t {
  Wipe = KB_GSI_WIPE,
  Disable = KB_GSI_DISABLE,
  Status = KB_GSI_STATUS,
};

enum class SnapshotUpdateCommand : kb_snapshot_update_command_t {
  Cancel = KB_SNAPSHOT_UPDATE_CANCEL,
  Merge = KB_SNAPSHOT_UPDATE_MERGE,
};

struct FetchRange {
  std::optional<std::uint64_t> offset;
  std::optional<std::uint64_t> size;
};

using DeviceSelector = std::optional<std::string_view>;

struct ContextOptions {
  // Zero accepts every Fastboot USB vendor. Network transports are unaffected.
  std::uint16_t usb_vendor_id{};
};

struct FlashOptions {
  // Per-I/O timeout. milliseconds::max() selects the native infinite default.
  std::chrono::milliseconds timeout{std::chrono::milliseconds::max()};
  bool disable_verity{};
  bool disable_verification{};
  std::function<ProgressAction(const FlashProgress &)> progress;
  std::optional<std::string> slot;
  bool set_active{};
  std::optional<std::string> active_slot;
  // AOSP -S sparse-part limit in bytes; zero selects automatic device limits.
  std::uint64_t sparse_limit_bytes{};
};

struct LegacyBootOptions {
  std::string command_line;
  std::uint32_t base{0x10000000U};
  std::uint32_t page_size{2048U};
  std::uint32_t kernel_offset{0x00008000U};
  std::uint32_t ramdisk_offset{0x01000000U};
  std::uint32_t second_offset{0x00f00000U};
  std::uint32_t tags_offset{0x00000100U};
};

struct UpdateOptions {
  // Whole-operation timeout. milliseconds::max() selects no deadline.
  std::chrono::milliseconds timeout{std::chrono::milliseconds::max()};
  bool wipe{};
  bool skip_reboot{};
  bool skip_secondary{};
  bool exclude_dynamic_partitions{};
  bool disable_fastboot_info{};
  bool disable_verity{};
  bool disable_verification{};
  std::function<ProgressAction(const Progress &)> progress;
  std::optional<std::string> slot;
  bool set_active{};
  std::optional<std::string> active_slot;
  // AOSP -S sparse-part limit in bytes; zero selects automatic device limits.
  std::uint64_t sparse_limit_bytes{};
};

struct CommandOptions {
  // Per-I/O timeout. milliseconds::max() selects the native infinite default.
  std::chrono::milliseconds timeout{std::chrono::milliseconds::max()};
  std::function<ProgressAction(const Progress &)> progress;
  std::uint64_t maximum_receive_bytes{64ULL * 1024ULL * 1024ULL};
};

struct JobOptions {
  // Whole-job timeout. milliseconds::max() selects no deadline.
  std::chrono::milliseconds timeout{std::chrono::milliseconds::max()};
  std::function<ProgressAction(const Progress &)> progress;
};

class Error {
public:
  [[nodiscard]] kb_status_t status() const noexcept { return status_; }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }
  [[nodiscard]] const std::string &device_identifier() const noexcept {
    return device_identifier_;
  }
  [[nodiscard]] std::int32_t native_code() const noexcept {
    return native_code_;
  }
  [[nodiscard]] kb_transfer_state_t transfer_state() const noexcept {
    return transfer_state_;
  }
  [[nodiscard]] std::span<const std::byte> device_message() const noexcept {
    return device_message_;
  }
  [[nodiscard]] const std::vector<CommandMessage> &command_messages()
      const noexcept {
    return command_messages_;
  }
  [[nodiscard]] std::optional<std::uint64_t> inbound_expected_bytes()
      const noexcept {
    return inbound_expected_bytes_;
  }
  [[nodiscard]] std::uint64_t inbound_transferred_bytes() const noexcept {
    return inbound_transferred_bytes_;
  }
  [[nodiscard]] kb_transfer_state_t inbound_transfer_state() const noexcept {
    return inbound_transfer_state_;
  }
  [[nodiscard]] bool session_poisoned() const noexcept {
    return session_poisoned_;
  }

private:
  friend Error detail_take_error(kb_status_t, kb_error_t *);
  friend Error detail_copy_error(kb_status_t, const kb_error_t *);
  friend Error detail_make_error(kb_status_t, std::string);

  kb_status_t status_{KB_E_INTERNAL};
  std::string message_;
  std::string device_identifier_;
  std::int32_t native_code_{0};
  kb_transfer_state_t transfer_state_{KB_TRANSFER_NOT_SENT};
  std::vector<std::byte> device_message_;
  std::vector<CommandMessage> command_messages_;
  std::optional<std::uint64_t> inbound_expected_bytes_;
  std::uint64_t inbound_transferred_bytes_{0};
  kb_transfer_state_t inbound_transfer_state_{KB_TRANSFER_NOT_SENT};
  bool session_poisoned_{false};
};

inline std::span<const std::byte> detail_byte_view(const std::uint8_t *data,
                                                  std::size_t size) noexcept {
  return {reinterpret_cast<const std::byte *>(data), size};
}

inline std::vector<std::byte> detail_copy_bytes(const std::uint8_t *data,
                                                const std::size_t size) {
  if (data == nullptr || size == 0) {
    return {};
  }
  const auto *first = reinterpret_cast<const std::byte *>(data);
  return {first, first + size};
}

inline Error detail_copy_error(kb_status_t fallback,
                               const kb_error_t *handle) {
  Error result;
  result.status_ = handle == nullptr ? fallback : kb_error_status(handle);
  result.message_ =
      handle == nullptr ? kb_status_string(fallback) : kb_error_message(handle);
  if (handle != nullptr) {
    result.device_identifier_ = kb_error_device_identifier(handle);
    result.native_code_ = kb_error_native_code(handle);
    result.transfer_state_ = kb_error_transfer_state(handle);
    std::size_t size = 0;
    const auto *device_message = kb_error_device_message(handle, &size);
    result.device_message_ = detail_copy_bytes(device_message, size);
    const std::size_t message_count = kb_error_command_message_count(handle);
    result.command_messages_.reserve(message_count);
    for (std::size_t index = 0; index < message_count; ++index) {
      size = 0;
      const auto *payload =
          kb_error_command_message_payload(handle, index, &size);
      result.command_messages_.push_back(CommandMessage{
          kb_error_command_message_kind(handle, index) ==
                  KB_COMMAND_MESSAGE_TEXT
              ? CommandMessageKind::Text
              : CommandMessageKind::Info,
          detail_copy_bytes(payload, size),
      });
    }
    const auto inbound_expected = kb_error_inbound_expected_bytes(handle);
    if (inbound_expected != KB_FETCH_UNSPECIFIED) {
      result.inbound_expected_bytes_ = inbound_expected;
    }
    result.inbound_transferred_bytes_ =
        kb_error_inbound_transferred_bytes(handle);
    result.inbound_transfer_state_ =
        kb_error_inbound_transfer_state(handle);
    result.session_poisoned_ = kb_error_session_poisoned(handle) != 0;
  }
  return result;
}

inline Error detail_take_error(kb_status_t fallback, kb_error_t *handle) {
  const std::unique_ptr<kb_error_t, void (*)(kb_error_t *)> owner{
      handle, &kb_error_release};
  return detail_copy_error(fallback, owner.get());
}

inline Error detail_make_error(kb_status_t status, std::string message) {
  Error result;
  result.status_ = status;
  result.message_ = std::move(message);
  return result;
}

namespace detail {

struct ProgressCallbackState final {
  std::function<ProgressAction(const FlashProgress &)> callback;
};

inline kb_progress_action_t KB_CALL
progress_trampoline(const kb_progress_t *progress, void *user_data) noexcept {
  if (progress == nullptr || user_data == nullptr ||
      progress->struct_size < sizeof(kb_progress_t) ||
      progress->api_version != KB_API_VERSION) {
    return KB_PROGRESS_CANCEL;
  }

  auto *state = static_cast<ProgressCallbackState *>(user_data);
  if (!state->callback) {
    return KB_PROGRESS_CANCEL;
  }

  try {
    const FlashProgress converted{
        progress->bytes_completed,
        progress->bytes_total,
        progress->stage == nullptr ? std::string_view{}
                                   : std::string_view{progress->stage},
        progress->device_identifier == nullptr
            ? std::string_view{}
            : std::string_view{progress->device_identifier},
    };
    return state->callback(converted) == ProgressAction::Continue
               ? KB_PROGRESS_CONTINUE
               : KB_PROGRESS_CANCEL;
  } catch (...) {
    return KB_PROGRESS_CANCEL;
  }
}

struct PreparedFlashOptions final {
  kb_flash_options_t native{};
  std::shared_ptr<ProgressCallbackState> callback_state;
  std::shared_ptr<std::string> slot;
  std::shared_ptr<std::string> active_slot;
};

struct PreparedLegacyBootOptions final {
  kb_legacy_boot_options_t native{};
  std::string command_line;
};

struct PreparedUpdateOptions final {
  kb_update_options_t native{};
  std::shared_ptr<ProgressCallbackState> callback_state;
  std::shared_ptr<std::string> slot;
  std::shared_ptr<std::string> active_slot;
};

[[nodiscard]] inline std::expected<std::uint32_t, Error>
prepare_timeout(const std::chrono::milliseconds timeout,
                const std::string_view option_name) {
  if (timeout == std::chrono::milliseconds::max()) {
    return KB_WAIT_INFINITE;
  }
  const auto count = timeout.count();
  if (count < 0 ||
      static_cast<std::uint64_t>(count) >=
          static_cast<std::uint64_t>(KB_WAIT_INFINITE)) {
    return std::unexpected(detail_make_error(
        KB_E_INVALID_ARGUMENT,
        std::string{option_name} +
            " must be non-negative and less than UINT32_MAX milliseconds, "
            "or std::chrono::milliseconds::max()"));
  }
  return static_cast<std::uint32_t>(count);
}

[[nodiscard]] inline std::expected<PreparedUpdateOptions, Error>
prepare_update_options(const UpdateOptions &options) {
  auto timeout = prepare_timeout(options.timeout, "update timeout");
  if (!timeout) {
    return std::unexpected(std::move(timeout.error()));
  }

  PreparedUpdateOptions result;
  kb_update_options_init(&result.native);
  result.native.timeout_ms = *timeout;
  result.native.wipe = options.wipe ? 1 : 0;
  result.native.skip_reboot = options.skip_reboot ? 1 : 0;
  result.native.skip_secondary = options.skip_secondary ? 1 : 0;
  result.native.exclude_dynamic_partitions =
      options.exclude_dynamic_partitions ? 1 : 0;
  result.native.disable_fastboot_info = options.disable_fastboot_info ? 1 : 0;
  result.native.disable_verity = options.disable_verity ? 1 : 0;
  result.native.disable_verification = options.disable_verification ? 1 : 0;
  result.native.sparse_limit_bytes = options.sparse_limit_bytes;
  if (options.slot) {
    if (options.slot->empty() || options.slot->find('\0') != std::string::npos) {
      return std::unexpected(detail_make_error(
          KB_E_INVALID_ARGUMENT, "update slot must be non-empty and NUL-free"));
    }
    result.slot = std::make_shared<std::string>(*options.slot);
    result.native.slot = result.slot->c_str();
  }
  result.native.set_active = options.set_active ? 1 : 0;
  if (options.active_slot) {
    if (!options.set_active) {
      return std::unexpected(detail_make_error(
          KB_E_INVALID_ARGUMENT,
          "update active_slot requires set_active"));
    }
    if (options.active_slot->empty() ||
        options.active_slot->find('\0') != std::string::npos) {
      return std::unexpected(detail_make_error(
          KB_E_INVALID_ARGUMENT,
          "update active_slot must be non-empty and NUL-free"));
    }
    result.active_slot = std::make_shared<std::string>(*options.active_slot);
    result.native.active_slot = result.active_slot->c_str();
  }
  if (options.progress) {
    result.callback_state =
        std::make_shared<ProgressCallbackState>(ProgressCallbackState{
            options.progress,
        });
    result.native.progress_callback = &progress_trampoline;
    result.native.progress_user_data = result.callback_state.get();
  }
  return result;
}

[[nodiscard]] inline std::expected<PreparedFlashOptions, Error>
prepare_flash_options(const FlashOptions &options) {
  auto timeout = prepare_timeout(options.timeout, "flash timeout");
  if (!timeout) {
    return std::unexpected(std::move(timeout.error()));
  }

  PreparedFlashOptions result;
  kb_flash_options_init(&result.native);
  result.native.timeout_ms = *timeout;
  result.native.disable_verity = options.disable_verity ? 1 : 0;
  result.native.disable_verification = options.disable_verification ? 1 : 0;
  result.native.sparse_limit_bytes = options.sparse_limit_bytes;
  if (options.slot) {
    if (options.slot->empty() || options.slot->find('\0') != std::string::npos) {
      return std::unexpected(detail_make_error(
          KB_E_INVALID_ARGUMENT, "flash slot must be non-empty and NUL-free"));
    }
    result.slot = std::make_shared<std::string>(*options.slot);
    result.native.slot = result.slot->c_str();
  }
  result.native.set_active = options.set_active ? 1 : 0;
  if (options.active_slot) {
    if (!options.set_active) {
      return std::unexpected(detail_make_error(
          KB_E_INVALID_ARGUMENT, "flash active_slot requires set_active"));
    }
    if (options.active_slot->empty() ||
        options.active_slot->find('\0') != std::string::npos) {
      return std::unexpected(detail_make_error(
          KB_E_INVALID_ARGUMENT,
          "flash active_slot must be non-empty and NUL-free"));
    }
    result.active_slot = std::make_shared<std::string>(*options.active_slot);
    result.native.active_slot = result.active_slot->c_str();
  }
  if (options.progress) {
    // This ordinary C++ allocation/copy happens before calling the C ABI and
    // may propagate std::bad_alloc or a callable-defined copy exception.
    result.callback_state =
        std::make_shared<ProgressCallbackState>(ProgressCallbackState{
            options.progress,
        });
    result.native.progress_callback = &progress_trampoline;
    result.native.progress_user_data = result.callback_state.get();
  }
  return result;
}

[[nodiscard]] inline std::expected<PreparedLegacyBootOptions, Error>
prepare_legacy_boot_options(const LegacyBootOptions &options) {
  if (options.command_line.find('\0') != std::string::npos) {
    return std::unexpected(detail_make_error(
        KB_E_INVALID_ARGUMENT, "legacy boot command line must be NUL-free"));
  }
  PreparedLegacyBootOptions result;
  kb_legacy_boot_options_init(&result.native);
  result.command_line = options.command_line;
  result.native.base = options.base;
  result.native.page_size = options.page_size;
  result.native.kernel_offset = options.kernel_offset;
  result.native.ramdisk_offset = options.ramdisk_offset;
  result.native.second_offset = options.second_offset;
  result.native.tags_offset = options.tags_offset;
  return result;
}

struct PreparedCommandOptions final {
  kb_command_options_t native{};
  std::shared_ptr<ProgressCallbackState> callback_state;
};

struct PreparedJobOptions final {
  kb_job_options_t native{};
  std::shared_ptr<ProgressCallbackState> callback_state;
};

[[nodiscard]] inline std::expected<PreparedJobOptions, Error>
prepare_job_options(const JobOptions &options) {
  auto timeout = prepare_timeout(options.timeout, "job timeout");
  if (!timeout) {
    return std::unexpected(std::move(timeout.error()));
  }

  PreparedJobOptions result;
  kb_job_options_init(&result.native);
  result.native.timeout_ms = *timeout;
  if (options.progress) {
    result.callback_state =
        std::make_shared<ProgressCallbackState>(ProgressCallbackState{
            options.progress,
        });
    result.native.progress_callback = &progress_trampoline;
    result.native.progress_user_data = result.callback_state.get();
  }
  return result;
}

[[nodiscard]] inline std::expected<PreparedCommandOptions, Error>
prepare_command_options(const CommandOptions &options) {
  auto timeout = prepare_timeout(options.timeout, "command timeout");
  if (!timeout) {
    return std::unexpected(std::move(timeout.error()));
  }
  if (options.maximum_receive_bytes == 0) {
    return std::unexpected(detail_make_error(
        KB_E_INVALID_ARGUMENT,
        "command maximum_receive_bytes must be greater than zero"));
  }

  PreparedCommandOptions result;
  kb_command_options_init(&result.native);
  result.native.timeout_ms = *timeout;
  result.native.maximum_receive_bytes = options.maximum_receive_bytes;
  if (options.progress) {
    result.callback_state =
        std::make_shared<ProgressCallbackState>(ProgressCallbackState{
            options.progress,
        });
    result.native.progress_callback = &progress_trampoline;
    result.native.progress_user_data = result.callback_state.get();
  }
  return result;
}

[[nodiscard]] inline kb_reboot_target_t
native_reboot_target(const RebootTarget target) noexcept {
  return static_cast<kb_reboot_target_t>(target);
}

[[nodiscard]] inline kb_flashing_command_t
native_flashing_command(const FlashingCommand command) noexcept {
  return static_cast<kb_flashing_command_t>(command);
}

[[nodiscard]] inline kb_gsi_command_t
native_gsi_command(const GsiCommand command) noexcept {
  return static_cast<kb_gsi_command_t>(command);
}

[[nodiscard]] inline kb_snapshot_update_command_t
native_snapshot_update_command(const SnapshotUpdateCommand command) noexcept {
  return static_cast<kb_snapshot_update_command_t>(command);
}

[[nodiscard]] inline std::uint64_t
native_fetch_value(const std::optional<std::uint64_t> value) noexcept {
  return value.value_or(KB_FETCH_UNSPECIFIED);
}

// Owns the callback state until native release has drained every callback.
// The shared reset path also gives move assignment the same ordering.
class OperationResources final {
public:
  explicit OperationResources(
      kb_operation_t *handle,
      std::shared_ptr<ProgressCallbackState> callback_state = {}) noexcept
      : handle_(handle), callback_state_(std::move(callback_state)) {}

  ~OperationResources() { reset(); }

  OperationResources(const OperationResources &) = delete;
  OperationResources &operator=(const OperationResources &) = delete;

  OperationResources(OperationResources &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)),
        callback_state_(std::move(other.callback_state_)) {}

  OperationResources &operator=(OperationResources &&other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
      callback_state_ = std::move(other.callback_state_);
    }
    return *this;
  }

  [[nodiscard]] kb_operation_t *handle() const noexcept { return handle_; }

private:
  void reset() noexcept {
    kb_operation_t *handle = std::exchange(handle_, nullptr);
    kb_operation_release(handle);
    callback_state_.reset();
  }

  kb_operation_t *handle_{};
  std::shared_ptr<ProgressCallbackState> callback_state_;
};

} // namespace detail

struct Version {
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t patch{};
  std::uint32_t api_version{};
  std::string string;
};

[[nodiscard]] inline Version version() {
  kb_version_t native{};
  kb_version_init(&native);
  (void)kb_get_version(&native);
  return Version{native.major, native.minor, native.patch, native.api_version,
                 native.string};
}

class DeviceList {
public:
  explicit DeviceList(kb_device_list_t *handle) noexcept : handle_(handle) {}
  ~DeviceList() { kb_device_list_release(handle_); }

  DeviceList(const DeviceList &) = delete;
  DeviceList &operator=(const DeviceList &) = delete;

  DeviceList(DeviceList &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  DeviceList &operator=(DeviceList &&other) noexcept {
    if (this != &other) {
      kb_device_list_release(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return kb_device_list_count(handle_);
  }
  [[nodiscard]] std::string_view serial(std::size_t index) const noexcept {
    const char *value = kb_device_list_serial(handle_, index);
    return value == nullptr ? std::string_view{} : std::string_view{value};
  }
  [[nodiscard]] std::string_view usb_path(std::size_t index) const noexcept {
    const char *value = kb_device_list_usb_path(handle_, index);
    return value == nullptr ? std::string_view{} : std::string_view{value};
  }
  [[nodiscard]] std::string_view product(std::size_t index) const noexcept {
    const char *value = kb_device_list_product(handle_, index);
    return value == nullptr ? std::string_view{} : std::string_view{value};
  }

private:
  kb_device_list_t *handle_{};
};

class CommandResult {
public:
  explicit CommandResult(kb_command_result_t *handle) noexcept
      : handle_(handle) {}
  ~CommandResult() { kb_command_result_release(handle_); }

  CommandResult(const CommandResult &) = delete;
  CommandResult &operator=(const CommandResult &) = delete;

  CommandResult(CommandResult &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  CommandResult &operator=(CommandResult &&other) noexcept {
    if (this != &other) {
      kb_command_result_release(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::span<const std::byte> terminal_payload() const noexcept {
    std::size_t size = 0;
    const auto *data = kb_command_result_terminal_payload(handle_, &size);
    return detail_byte_view(data, size);
  }

  [[nodiscard]] std::size_t message_count() const noexcept {
    return kb_command_result_message_count(handle_);
  }

  [[nodiscard]] std::optional<CommandMessageView>
  message(const std::size_t index) const noexcept {
    if (index >= message_count()) {
      return std::nullopt;
    }
    std::size_t size = 0;
    const auto *data =
        kb_command_result_message_payload(handle_, index, &size);
    return CommandMessageView{
        kb_command_result_message_kind(handle_, index) ==
                KB_COMMAND_MESSAGE_TEXT
            ? CommandMessageKind::Text
            : CommandMessageKind::Info,
        detail_byte_view(data, size),
    };
  }

  [[nodiscard]] std::span<const std::byte> data() const noexcept {
    std::size_t size = 0;
    const auto *data = kb_command_result_data(handle_, &size);
    return detail_byte_view(data, size);
  }

  [[nodiscard]] std::string_view output_path() const noexcept {
    const char *path = kb_command_result_output_path(handle_);
    return path == nullptr ? std::string_view{} : std::string_view{path};
  }

  [[nodiscard]] std::uint64_t received_bytes() const noexcept {
    return kb_command_result_received_bytes(handle_);
  }

  [[nodiscard]] std::string_view device_identifier() const noexcept {
    const char *identifier = kb_command_result_device_identifier(handle_);
    return identifier == nullptr ? std::string_view{}
                                 : std::string_view{identifier};
  }

private:
  kb_command_result_t *handle_{};
};

/* Immutable fleet plan snapshot mirroring the context-free C ABI: validation
 * and planning never touch a device; only the manifest file is read. */
class JobPlan {
public:
  explicit JobPlan(kb_job_plan_t *handle) noexcept : handle_(handle) {}
  ~JobPlan() { kb_job_plan_release(handle_); }

  JobPlan(const JobPlan &) = delete;
  JobPlan &operator=(const JobPlan &) = delete;

  JobPlan(JobPlan &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  JobPlan &operator=(JobPlan &&other) noexcept {
    if (this != &other) {
      kb_job_plan_release(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  // Borrowed views over plan-owned storage: no bytes are copied and the views
  // stay valid until this JobPlan is destroyed or moved from. The canonical
  // JSON is NUL-terminated UTF-8 without a trailing LF; the digest is 64
  // lowercase hex characters.
  [[nodiscard]] std::string_view canonical_json() const noexcept {
    std::size_t size = 0;
    const char *json = kb_job_plan_canonical_json(handle_, &size);
    return json == nullptr ? std::string_view{} : std::string_view{json, size};
  }
  [[nodiscard]] std::string_view sha256_hex() const noexcept {
    const char *hex = kb_job_plan_sha256_hex(handle_);
    return hex == nullptr ? std::string_view{} : std::string_view{hex};
  }

private:
  kb_job_plan_t *handle_{};
};

class JobReport {
public:
  explicit JobReport(kb_job_report_t *handle) noexcept : handle_(handle) {}
  ~JobReport() { kb_job_report_release(handle_); }

  JobReport(const JobReport &) = delete;
  JobReport &operator=(const JobReport &) = delete;

  JobReport(JobReport &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  JobReport &operator=(JobReport &&other) noexcept {
    if (this != &other) {
      kb_job_report_release(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::string_view json() const noexcept {
    std::size_t size = 0;
    const char *value = kb_job_report_json(handle_, &size);
    return value == nullptr ? std::string_view{}
                            : std::string_view{value, size};
  }

private:
  kb_job_report_t *handle_{};
};

/* Context-free manifest entry points: failures surface the manifest source
 * path and, when known, its line and column inside the error message. */
[[nodiscard]] inline std::expected<void, Error>
validate_job_file(const std::filesystem::path &file_path) {
  const auto path_u8 = file_path.u8string();
  const std::string path_string{path_u8.begin(), path_u8.end()};
  kb_error_t *error = nullptr;
  const kb_status_t status =
      ::kb_validate_job_file(path_string.c_str(), &error);
  if (status != KB_OK) {
    return std::unexpected(detail_take_error(status, error));
  }
  return {};
}

[[nodiscard]] inline std::expected<JobPlan, Error>
plan_job_file(const std::filesystem::path &file_path) {
  const auto path_u8 = file_path.u8string();
  const std::string path_string{path_u8.begin(), path_u8.end()};
  kb_job_plan_t *plan = nullptr;
  kb_error_t *error = nullptr;
  const kb_status_t status =
      ::kb_plan_job_file(path_string.c_str(), &plan, &error);
  if (status != KB_OK) {
    return std::unexpected(detail_take_error(status, error));
  }
  return JobPlan{plan};
}

class Operation {
public:
  explicit Operation(kb_operation_t *handle) noexcept : resources_(handle) {}
  ~Operation() = default;

  Operation(const Operation &) = delete;
  Operation &operator=(const Operation &) = delete;

  Operation(Operation &&other) noexcept = default;
  Operation &operator=(Operation &&other) noexcept = default;

  [[nodiscard]] kb_operation_state_t state() const noexcept {
    return kb_operation_state(resources_.handle());
  }
  [[nodiscard]] std::expected<void, Error>
  wait(std::uint32_t timeout_ms = KB_WAIT_INFINITE) {
    const kb_status_t status =
        kb_operation_wait(resources_.handle(), timeout_ms);
    if (status != KB_OK) {
      return std::unexpected(
          detail_copy_error(status,
                            kb_operation_error(resources_.handle())));
    }
    return {};
  }
  [[nodiscard]] std::expected<void, Error>
  wait(const std::chrono::milliseconds timeout) {
    auto native_timeout = detail::prepare_timeout(timeout, "operation timeout");
    if (!native_timeout) {
      return std::unexpected(std::move(native_timeout.error()));
    }
    return wait(*native_timeout);
  }
  [[nodiscard]] std::expected<void, Error>
  wait(const std::stop_token stop_token,
       const std::chrono::milliseconds timeout =
           std::chrono::milliseconds::max()) {
    auto native_timeout = detail::prepare_timeout(timeout, "operation timeout");
    if (!native_timeout) {
      return std::unexpected(std::move(native_timeout.error()));
    }
    std::atomic<bool> stop_observed{false};
    auto result = [&] {
      std::stop_callback cancel_on_stop{
          stop_token, [handle = resources_.handle(), &stop_observed] {
            stop_observed.store(true, std::memory_order_release);
            (void)kb_operation_cancel(handle);
          }};
      return wait(*native_timeout);
    }();
    if (!result && result.error().status() == KB_E_TIMEOUT &&
        stop_observed.load(std::memory_order_acquire)) {
      return wait(KB_WAIT_INFINITE);
    }
    return result;
  }
  [[nodiscard]] std::expected<void, Error> cancel() {
    const kb_status_t status = kb_operation_cancel(resources_.handle());
    if (status != KB_OK) {
      return std::unexpected(detail_copy_error(status, nullptr));
    }
    return {};
  }

  [[nodiscard]] std::expected<CommandResult, Error> command_result() const {
    kb_command_result_t *result = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = kb_operation_command_result(
        resources_.handle(), &result, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return CommandResult{result};
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  wait_result(std::uint32_t timeout_ms = KB_WAIT_INFINITE) {
    auto waited = wait(timeout_ms);
    if (!waited) {
      return std::unexpected(std::move(waited.error()));
    }
    return command_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  wait_result(const std::chrono::milliseconds timeout) {
    auto native_timeout = detail::prepare_timeout(timeout, "operation timeout");
    if (!native_timeout) {
      return std::unexpected(std::move(native_timeout.error()));
    }
    return wait_result(*native_timeout);
  }

  // A stop request calls the C cancellation API, then this method remains in
  // kb_operation_wait until the native transport has reached a terminal state
  // and drained its callbacks.
  [[nodiscard]] std::expected<CommandResult, Error>
  wait_result(const std::stop_token stop_token,
              const std::chrono::milliseconds timeout =
                  std::chrono::milliseconds::max()) {
    auto native_timeout = detail::prepare_timeout(timeout, "operation timeout");
    if (!native_timeout) {
      return std::unexpected(std::move(native_timeout.error()));
    }
    std::atomic<bool> stop_observed{false};
    auto result = [&] {
      std::stop_callback cancel_on_stop{
          stop_token, [handle = resources_.handle(), &stop_observed] {
            stop_observed.store(true, std::memory_order_release);
            (void)kb_operation_cancel(handle);
          }};
      return wait_result(*native_timeout);
    }();
    if (!result && result.error().status() == KB_E_TIMEOUT &&
        stop_observed.load(std::memory_order_acquire)) {
      return wait_result(KB_WAIT_INFINITE);
    }
    return result;
  }

private:
  friend class Context;

  Operation(
      kb_operation_t *handle,
      std::shared_ptr<detail::ProgressCallbackState> callback_state) noexcept
      : resources_(handle, std::move(callback_state)) {}

  detail::OperationResources resources_;
};

class Job {
public:
  explicit Job(kb_job_t *handle) noexcept : handle_(handle) {}
  ~Job() { reset(); }

  Job(const Job &) = delete;
  Job &operator=(const Job &) = delete;

  Job(Job &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)),
        callback_state_(std::move(other.callback_state_)) {}
  Job &operator=(Job &&other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
      callback_state_ = std::move(other.callback_state_);
    }
    return *this;
  }

  [[nodiscard]] kb_operation_state_t state() const noexcept {
    return kb_job_state(handle_);
  }

  [[nodiscard]] std::optional<Error> error() const {
    const kb_error_t *native = kb_job_error(handle_);
    if (native == nullptr) {
      return std::nullopt;
    }
    return detail_copy_error(kb_error_status(native), native);
  }

  [[nodiscard]] std::expected<void, Error>
  wait(std::uint32_t timeout_ms = KB_WAIT_INFINITE) {
    const kb_status_t status = kb_job_wait(handle_, timeout_ms);
    if (status != KB_OK) {
      return std::unexpected(
          detail_copy_error(status, kb_job_error(handle_)));
    }
    return {};
  }

  [[nodiscard]] std::expected<void, Error>
  wait(const std::chrono::milliseconds timeout) {
    auto native_timeout = detail::prepare_timeout(timeout, "job wait timeout");
    if (!native_timeout) {
      return std::unexpected(std::move(native_timeout.error()));
    }
    return wait(*native_timeout);
  }

  [[nodiscard]] std::expected<void, Error>
  wait(const std::stop_token stop_token,
       const std::chrono::milliseconds timeout =
           std::chrono::milliseconds::max()) {
    auto native_timeout = detail::prepare_timeout(timeout, "job wait timeout");
    if (!native_timeout) {
      return std::unexpected(std::move(native_timeout.error()));
    }
    std::atomic<bool> stop_observed{false};
    auto result = [&] {
      std::stop_callback cancel_on_stop{
          stop_token, [handle = handle_, &stop_observed] {
            stop_observed.store(true, std::memory_order_release);
            (void)kb_job_cancel(handle);
          }};
      return wait(*native_timeout);
    }();
    if (!result && result.error().status() == KB_E_TIMEOUT &&
        stop_observed.load(std::memory_order_acquire)) {
      return wait(KB_WAIT_INFINITE);
    }
    return result;
  }

  [[nodiscard]] std::expected<void, Error> cancel() {
    const kb_status_t status = kb_job_cancel(handle_);
    if (status != KB_OK) {
      return std::unexpected(detail_copy_error(status, nullptr));
    }
    return {};
  }

  [[nodiscard]] std::expected<JobReport, Error> report() const {
    kb_job_report_t *report = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = kb_job_get_report(handle_, &report, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return JobReport{report};
  }

private:
  friend class Context;

  Job(kb_job_t *handle,
      std::shared_ptr<detail::ProgressCallbackState> callback_state) noexcept
      : handle_(handle), callback_state_(std::move(callback_state)) {}

  void reset() noexcept {
    kb_job_t *handle = std::exchange(handle_, nullptr);
    kb_job_release(handle);
    callback_state_.reset();
  }

  kb_job_t *handle_{};
  std::shared_ptr<detail::ProgressCallbackState> callback_state_;
};

class Context {
public:
  static std::expected<Context, Error>
  create(const ContextOptions &options = {}) {
    kb_context_options_t native{};
    kb_context_options_init(&native);
    native.usb_vendor_id = options.usb_vendor_id;
    kb_context_t *handle = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = kb_context_create(&native, &handle, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Context{handle};
  }

  ~Context() { kb_context_release(handle_); }
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  Context(Context &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  Context &operator=(Context &&other) noexcept {
    if (this != &other) {
      kb_context_release(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::expected<DeviceList, Error> devices() const {
    kb_device_list_t *devices = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = kb_enumerate_devices(handle_, &devices, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return DeviceList{devices};
  }

  [[nodiscard]] std::expected<Operation, Error> getvar_async(
      const DeviceSelector selector, const std::string_view variable,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string variable_storage{variable};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_getvar_async(handle_, selector_value,
                                   variable_storage.c_str(), native, operation,
                                   error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  getvar_async(const std::string_view variable,
               const CommandOptions &options = {}) const {
    return getvar_async(std::nullopt, variable, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> getvar(
      const DeviceSelector selector, const std::string_view variable,
      const CommandOptions &options = {}) const {
    auto operation = getvar_async(selector, variable, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  getvar(const std::string_view variable,
         const CommandOptions &options = {}) const {
    return getvar(std::nullopt, variable, options);
  }

  [[nodiscard]] std::expected<Operation, Error> erase_async(
      const DeviceSelector selector, const std::string_view partition,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string partition_storage{partition};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_erase_async(handle_, selector_value,
                                  partition_storage.c_str(), native, operation,
                                  error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  erase_async(const std::string_view partition,
              const CommandOptions &options = {}) const {
    return erase_async(std::nullopt, partition, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> erase(
      const DeviceSelector selector, const std::string_view partition,
      const CommandOptions &options = {}) const {
    auto operation = erase_async(selector, partition, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  erase(const std::string_view partition,
        const CommandOptions &options = {}) const {
    return erase(std::nullopt, partition, options);
  }

  [[nodiscard]] std::expected<Operation, Error> format_partition_async(
      const DeviceSelector selector, const std::string_view partition,
      const std::optional<std::string_view> filesystem_type = std::nullopt,
      const std::uint64_t partition_size = 0,
      const FlashOptions &options = {}) const {
    auto prepared = detail::prepare_flash_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string partition_storage{partition};
    const std::optional<std::string> type_storage =
        filesystem_type.has_value()
            ? std::optional<std::string>{std::string{*filesystem_type}}
            : std::nullopt;
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_format_partition_async(
        handle_, selector_value, partition_storage.c_str(),
        type_storage.has_value() ? type_storage->c_str() : nullptr,
        partition_size, &prepared->native, &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<Operation, Error> format_partition_async(
      const std::string_view partition,
      const std::optional<std::string_view> filesystem_type = std::nullopt,
      const std::uint64_t partition_size = 0,
      const FlashOptions &options = {}) const {
    return format_partition_async(std::nullopt, partition, filesystem_type,
                                  partition_size, options);
  }

  [[nodiscard]] std::expected<void, Error> format_partition(
      const DeviceSelector selector, const std::string_view partition,
      const std::optional<std::string_view> filesystem_type = std::nullopt,
      const std::uint64_t partition_size = 0,
      const FlashOptions &options = {}) const {
    auto operation = format_partition_async(
        selector, partition, filesystem_type, partition_size, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<void, Error> format_partition(
      const std::string_view partition,
      const std::optional<std::string_view> filesystem_type = std::nullopt,
      const std::uint64_t partition_size = 0,
      const FlashOptions &options = {}) const {
    return format_partition(std::nullopt, partition, filesystem_type,
                            partition_size, options);
  }

  [[nodiscard]] std::expected<Operation, Error> set_active_async(
      const DeviceSelector selector, const std::string_view slot,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string slot_storage{slot};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_set_active_async(handle_, selector_value,
                                       slot_storage.c_str(), native, operation,
                                       error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  set_active_async(const std::string_view slot,
                   const CommandOptions &options = {}) const {
    return set_active_async(std::nullopt, slot, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> set_active(
      const DeviceSelector selector, const std::string_view slot,
      const CommandOptions &options = {}) const {
    auto operation = set_active_async(selector, slot, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  set_active(const std::string_view slot,
             const CommandOptions &options = {}) const {
    return set_active(std::nullopt, slot, options);
  }

  [[nodiscard]] std::expected<Operation, Error>
  flashing_async(const DeviceSelector selector, const FlashingCommand command,
                 const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_flashing_async(handle_, selector_value,
                                     detail::native_flashing_command(command),
                                     native, operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  flashing_async(const FlashingCommand command,
                 const CommandOptions &options = {}) const {
    return flashing_async(std::nullopt, command, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  flashing(const DeviceSelector selector, const FlashingCommand command,
           const CommandOptions &options = {}) const {
    auto operation = flashing_async(selector, command, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  flashing(const FlashingCommand command,
           const CommandOptions &options = {}) const {
    return flashing(std::nullopt, command, options);
  }

  [[nodiscard]] std::expected<Operation, Error>
  gsi_async(const DeviceSelector selector, const GsiCommand command,
            const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_gsi_async(handle_, selector_value,
                                detail::native_gsi_command(command), native,
                                operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  gsi_async(const GsiCommand command,
            const CommandOptions &options = {}) const {
    return gsi_async(std::nullopt, command, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  gsi(const DeviceSelector selector, const GsiCommand command,
      const CommandOptions &options = {}) const {
    auto operation = gsi_async(selector, command, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  gsi(const GsiCommand command, const CommandOptions &options = {}) const {
    return gsi(std::nullopt, command, options);
  }

  [[nodiscard]] std::expected<Operation, Error>
  snapshot_update_async(const DeviceSelector selector,
                        const SnapshotUpdateCommand command,
                        const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_snapshot_update_async(
              handle_, selector_value,
              detail::native_snapshot_update_command(command), native,
              operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  snapshot_update_async(const SnapshotUpdateCommand command,
                        const CommandOptions &options = {}) const {
    return snapshot_update_async(std::nullopt, command, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  snapshot_update(const DeviceSelector selector,
                  const SnapshotUpdateCommand command,
                  const CommandOptions &options = {}) const {
    auto operation = snapshot_update_async(selector, command, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  snapshot_update(const SnapshotUpdateCommand command,
                  const CommandOptions &options = {}) const {
    return snapshot_update(std::nullopt, command, options);
  }

  [[nodiscard]] std::expected<Operation, Error> create_logical_partition_async(
      const DeviceSelector selector, const std::string_view partition_name,
      const std::uint64_t size, const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string partition_name_storage{partition_name};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_create_logical_partition_async(
              handle_, selector_value, partition_name_storage.c_str(), size,
              native, operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  create_logical_partition_async(const std::string_view partition_name,
                                 const std::uint64_t size,
                                 const CommandOptions &options = {}) const {
    return create_logical_partition_async(std::nullopt, partition_name, size,
                                          options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> create_logical_partition(
      const DeviceSelector selector, const std::string_view partition_name,
      const std::uint64_t size, const CommandOptions &options = {}) const {
    auto operation =
        create_logical_partition_async(selector, partition_name, size, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  create_logical_partition(const std::string_view partition_name,
                           const std::uint64_t size,
                           const CommandOptions &options = {}) const {
    return create_logical_partition(std::nullopt, partition_name, size,
                                    options);
  }

  [[nodiscard]] std::expected<Operation, Error>
  delete_logical_partition_async(const DeviceSelector selector,
                                 const std::string_view partition_name,
                                 const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string partition_name_storage{partition_name};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_delete_logical_partition_async(
              handle_, selector_value, partition_name_storage.c_str(), native,
              operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  delete_logical_partition_async(const std::string_view partition_name,
                                 const CommandOptions &options = {}) const {
    return delete_logical_partition_async(std::nullopt, partition_name,
                                          options);
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  delete_logical_partition(const DeviceSelector selector,
                           const std::string_view partition_name,
                           const CommandOptions &options = {}) const {
    auto operation =
        delete_logical_partition_async(selector, partition_name, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  delete_logical_partition(const std::string_view partition_name,
                           const CommandOptions &options = {}) const {
    return delete_logical_partition(std::nullopt, partition_name, options);
  }

  [[nodiscard]] std::expected<Operation, Error> resize_logical_partition_async(
      const DeviceSelector selector, const std::string_view partition_name,
      const std::uint64_t size, const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string partition_name_storage{partition_name};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_resize_logical_partition_async(
              handle_, selector_value, partition_name_storage.c_str(), size,
              native, operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  resize_logical_partition_async(const std::string_view partition_name,
                                 const std::uint64_t size,
                                 const CommandOptions &options = {}) const {
    return resize_logical_partition_async(std::nullopt, partition_name, size,
                                          options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> resize_logical_partition(
      const DeviceSelector selector, const std::string_view partition_name,
      const std::uint64_t size, const CommandOptions &options = {}) const {
    auto operation =
        resize_logical_partition_async(selector, partition_name, size, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  resize_logical_partition(const std::string_view partition_name,
                           const std::uint64_t size,
                           const CommandOptions &options = {}) const {
    return resize_logical_partition(std::nullopt, partition_name, size,
                                    options);
  }

  [[nodiscard]] std::expected<Operation, Error> reboot_async(
      const DeviceSelector selector, const RebootTarget target,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_reboot_async(handle_, selector_value,
                                   detail::native_reboot_target(target), native,
                                   operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error> reboot_async(
      const RebootTarget target = RebootTarget::System,
      const CommandOptions &options = {}) const {
    return reboot_async(std::nullopt, target, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> reboot(
      const DeviceSelector selector, const RebootTarget target,
      const CommandOptions &options = {}) const {
    auto operation = reboot_async(selector, target, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error> reboot(
      const RebootTarget target = RebootTarget::System,
      const CommandOptions &options = {}) const {
    return reboot(std::nullopt, target, options);
  }

  [[nodiscard]] std::expected<Operation, Error> continue_boot_async(
      const DeviceSelector selector,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_continue_boot_async(handle_, selector_value, native,
                                           operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  continue_boot_async(const CommandOptions &options = {}) const {
    return continue_boot_async(std::nullopt, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> continue_boot(
      const DeviceSelector selector,
      const CommandOptions &options = {}) const {
    auto operation = continue_boot_async(selector, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  continue_boot(const CommandOptions &options = {}) const {
    return continue_boot(std::nullopt, options);
  }

  [[nodiscard]] std::expected<Operation, Error> oem_async(
      const DeviceSelector selector, const std::string_view command_suffix,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string suffix_storage{command_suffix};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_oem_async(handle_, selector_value, suffix_storage.c_str(),
                                native, operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  oem_async(const std::string_view command_suffix,
            const CommandOptions &options = {}) const {
    return oem_async(std::nullopt, command_suffix, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> oem(
      const DeviceSelector selector, const std::string_view command_suffix,
      const CommandOptions &options = {}) const {
    auto operation = oem_async(selector, command_suffix, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  oem(const std::string_view command_suffix,
      const CommandOptions &options = {}) const {
    return oem(std::nullopt, command_suffix, options);
  }

  [[nodiscard]] std::expected<Operation, Error> raw_command_async(
      const DeviceSelector selector, const std::string_view command,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string command_storage{command};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_raw_command_async(handle_, selector_value,
                                        command_storage.c_str(), native,
                                        operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  raw_command_async(const std::string_view command,
                    const CommandOptions &options = {}) const {
    return raw_command_async(std::nullopt, command, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> raw_command(
      const DeviceSelector selector, const std::string_view command,
      const CommandOptions &options = {}) const {
    auto operation = raw_command_async(selector, command, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  raw_command(const std::string_view command,
              const CommandOptions &options = {}) const {
    return raw_command(std::nullopt, command, options);
  }

  [[nodiscard]] std::expected<Operation, Error> boot_async(
      const DeviceSelector selector,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_boot_async(handle_, selector_value, native, operation,
                                 error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  boot_async(const CommandOptions &options = {}) const {
    return boot_async(std::nullopt, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> boot(
      const DeviceSelector selector,
      const CommandOptions &options = {}) const {
    auto operation = boot_async(selector, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  boot(const CommandOptions &options = {}) const {
    return boot(std::nullopt, options);
  }

  [[nodiscard]] std::expected<Operation, Error> stage_async(
      const DeviceSelector selector, const std::span<const std::byte> data,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_stage_async(handle_, selector_value, data.data(),
                                  data.size(), native, operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error> stage_async(
      const std::span<const std::byte> data,
      const CommandOptions &options = {}) const {
    return stage_async(std::nullopt, data, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> stage(
      const DeviceSelector selector, const std::span<const std::byte> data,
      const CommandOptions &options = {}) const {
    auto operation = stage_async(selector, data, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  stage(const std::span<const std::byte> data,
        const CommandOptions &options = {}) const {
    return stage(std::nullopt, data, options);
  }

  [[nodiscard]] std::expected<Operation, Error> upload_async(
      const DeviceSelector selector,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_upload_async(handle_, selector_value, native, operation,
                                   error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error>
  upload_async(const CommandOptions &options = {}) const {
    return upload_async(std::nullopt, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> upload(
      const DeviceSelector selector,
      const CommandOptions &options = {}) const {
    auto operation = upload_async(selector, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error>
  upload(const CommandOptions &options = {}) const {
    return upload(std::nullopt, options);
  }

  [[nodiscard]] std::expected<Operation, Error> fetch_async(
      const DeviceSelector selector, const std::string_view partition,
      const FetchRange range = {},
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string partition_storage{partition};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_fetch_async(
              handle_, selector_value, partition_storage.c_str(),
              detail::native_fetch_value(range.offset),
              detail::native_fetch_value(range.size), native, operation,
              error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error> fetch_async(
      const std::string_view partition, const FetchRange range = {},
      const CommandOptions &options = {}) const {
    return fetch_async(std::nullopt, partition, range, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> fetch(
      const DeviceSelector selector, const std::string_view partition,
      const FetchRange range = {},
      const CommandOptions &options = {}) const {
    auto operation = fetch_async(selector, partition, range, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error> fetch(
      const std::string_view partition, const FetchRange range = {},
      const CommandOptions &options = {}) const {
    return fetch(std::nullopt, partition, range, options);
  }

  [[nodiscard]] std::expected<Operation, Error> upload_file_async(
      const DeviceSelector selector, const std::filesystem::path &output,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const auto output_u8 = output.u8string();
    const std::string output_storage{output_u8.begin(), output_u8.end()};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_upload_file_async(handle_, selector_value,
                                        output_storage.c_str(), native,
                                        operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error> upload_file_async(
      const std::filesystem::path &output,
      const CommandOptions &options = {}) const {
    return upload_file_async(std::nullopt, output, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> upload_file(
      const DeviceSelector selector, const std::filesystem::path &output,
      const CommandOptions &options = {}) const {
    auto operation = upload_file_async(selector, output, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error> upload_file(
      const std::filesystem::path &output,
      const CommandOptions &options = {}) const {
    return upload_file(std::nullopt, output, options);
  }

  [[nodiscard]] std::expected<Operation, Error> get_staged_file_async(
      const DeviceSelector selector, const std::filesystem::path &output,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const auto output_u8 = output.u8string();
    const std::string output_storage{output_u8.begin(), output_u8.end()};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_get_staged_file_async(
              handle_, selector_value, output_storage.c_str(), native,
              operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error> get_staged_file_async(
      const std::filesystem::path &output,
      const CommandOptions &options = {}) const {
    return get_staged_file_async(std::nullopt, output, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> get_staged_file(
      const DeviceSelector selector, const std::filesystem::path &output,
      const CommandOptions &options = {}) const {
    auto operation = get_staged_file_async(selector, output, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error> get_staged_file(
      const std::filesystem::path &output,
      const CommandOptions &options = {}) const {
    return get_staged_file(std::nullopt, output, options);
  }

  [[nodiscard]] std::expected<Operation, Error> fetch_file_async(
      const DeviceSelector selector, const std::string_view partition,
      const std::filesystem::path &output, const FetchRange range = {},
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string partition_storage{partition};
    const auto output_u8 = output.u8string();
    const std::string output_storage{output_u8.begin(), output_u8.end()};
    return start_typed_operation(
        options, [&](const kb_command_options_t *native,
                     kb_operation_t **operation, kb_error_t **error) {
          return ::kb_fetch_file_async(
              handle_, selector_value, partition_storage.c_str(),
              detail::native_fetch_value(range.offset),
              detail::native_fetch_value(range.size), output_storage.c_str(),
              native, operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error> fetch_file_async(
      const std::string_view partition, const std::filesystem::path &output,
      const FetchRange range = {},
      const CommandOptions &options = {}) const {
    return fetch_file_async(std::nullopt, partition, output, range, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> fetch_file(
      const DeviceSelector selector, const std::string_view partition,
      const std::filesystem::path &output, const FetchRange range = {},
      const CommandOptions &options = {}) const {
    auto operation = fetch_file_async(selector, partition, output, range,
                                      options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error> fetch_file(
      const std::string_view partition, const std::filesystem::path &output,
      const FetchRange range = {},
      const CommandOptions &options = {}) const {
    return fetch_file(std::nullopt, partition, output, range, options);
  }

  [[nodiscard]] std::expected<Job, Error> run_job_file_async(
      const std::filesystem::path &manifest,
      const JobOptions &options = {}) const {
    auto prepared = detail::prepare_job_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    const auto path_u8 = manifest.u8string();
    const std::string path_storage{path_u8.begin(), path_u8.end()};
    kb_job_t *job = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_run_job_file_async(
        handle_, path_storage.c_str(), &prepared->native, &job, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Job{job, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<JobReport, Error> run_job_file(
      const std::filesystem::path &manifest,
      const JobOptions &options = {}) const {
    auto job = run_job_file_async(manifest, options);
    if (!job) {
      return std::unexpected(std::move(job.error()));
    }
    auto waited = job->wait();
    if (!waited) {
      return std::unexpected(std::move(waited.error()));
    }
    return job->report();
  }

  [[nodiscard]] std::expected<Operation, Error> update_package_async(
      const DeviceSelector selector, const std::filesystem::path &package,
      const UpdateOptions &options = {}) const {
    auto prepared = detail::prepare_update_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }

    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const auto path_u8 = package.u8string();
    const std::string path_storage{path_u8.begin(), path_u8.end()};
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_update_package_async(
        handle_, selector_value, path_storage.c_str(), &prepared->native,
        &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<Operation, Error> update_package_async(
      const std::filesystem::path &package,
      const UpdateOptions &options = {}) const {
    return update_package_async(std::nullopt, package, options);
  }

  [[nodiscard]] std::expected<Operation, Error> update_package_async(
      const std::string_view selector, const std::filesystem::path &package,
      const UpdateOptions &options = {}) const {
    return update_package_async(DeviceSelector{selector}, package, options);
  }

  [[nodiscard]] std::expected<void, Error> update_package(
      const DeviceSelector selector, const std::filesystem::path &package,
      const UpdateOptions &options = {}) const {
    auto operation = update_package_async(selector, package, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<void, Error> update_package(
      const std::filesystem::path &package,
      const UpdateOptions &options = {}) const {
    return update_package(std::nullopt, package, options);
  }

  [[nodiscard]] std::expected<void, Error> update_package(
      const std::string_view selector, const std::filesystem::path &package,
      const UpdateOptions &options = {}) const {
    return update_package(DeviceSelector{selector}, package, options);
  }

  [[nodiscard]] std::expected<Operation, Error> wipe_super_async(
      const DeviceSelector selector,
      const std::optional<std::filesystem::path> &super_empty_image =
          std::nullopt,
      const UpdateOptions &options = {}) const {
    auto prepared = detail::prepare_update_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }

    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    std::string path_storage;
    const char *path_value = nullptr;
    if (super_empty_image.has_value()) {
      const auto path_u8 = super_empty_image->u8string();
      path_storage.assign(path_u8.begin(), path_u8.end());
      path_value = path_storage.c_str();
    }
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_wipe_super_async(
        handle_, selector_value, path_value, &prepared->native, &operation,
        &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<Operation, Error> wipe_super_async(
      const std::optional<std::filesystem::path> &super_empty_image =
          std::nullopt,
      const UpdateOptions &options = {}) const {
    return wipe_super_async(std::nullopt, super_empty_image, options);
  }

  [[nodiscard]] std::expected<void, Error> wipe_super(
      const DeviceSelector selector,
      const std::optional<std::filesystem::path> &super_empty_image =
          std::nullopt,
      const UpdateOptions &options = {}) const {
    auto operation = wipe_super_async(selector, super_empty_image, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<void, Error> wipe_super(
      const std::optional<std::filesystem::path> &super_empty_image =
          std::nullopt,
      const UpdateOptions &options = {}) const {
    return wipe_super(std::nullopt, super_empty_image, options);
  }

  [[nodiscard]] std::expected<Operation, Error> flash_file_async(
      std::optional<std::string_view> serial, std::string_view partition,
      const std::filesystem::path &file, const FlashOptions &options) const {
    auto prepared = detail::prepare_flash_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }

    const std::string serial_string =
        serial.has_value() ? std::string{*serial} : std::string{};
    const std::string partition_string{partition};
    const auto path_u8 = file.u8string();
    const std::string path_string{path_u8.begin(), path_u8.end()};
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_flash_file_async(
        handle_, serial.has_value() ? serial_string.c_str() : nullptr,
        partition_string.c_str(), path_string.c_str(), &prepared->native,
        &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<Operation, Error> flash_file_async(
      std::optional<std::string_view> serial, std::string_view partition,
      const std::filesystem::path &file) const {
    return flash_file_async(serial, partition, file, FlashOptions{});
  }

  [[nodiscard]] std::expected<Operation, Error>
  flash_file_async(std::string_view partition,
                   const std::filesystem::path &file,
                   const FlashOptions &options) const {
    return flash_file_async(std::nullopt, partition, file, options);
  }

  [[nodiscard]] std::expected<Operation, Error>
  flash_file_async(std::string_view partition,
                   const std::filesystem::path &file) const {
    return flash_file_async(std::nullopt, partition, file);
  }

  [[nodiscard]] std::expected<Operation, Error>
  flash_file_async(std::string_view serial, std::string_view partition,
                   const std::filesystem::path &file,
                   const FlashOptions &options) const {
    return flash_file_async(std::optional<std::string_view>{serial}, partition,
                            file, options);
  }

  [[nodiscard]] std::expected<Operation, Error>
  flash_file_async(std::string_view serial, std::string_view partition,
                   const std::filesystem::path &file) const {
    return flash_file_async(std::optional<std::string_view>{serial}, partition,
                            file);
  }

  [[nodiscard]] std::expected<void, Error>
  flash_file(std::optional<std::string_view> serial,
             std::string_view partition,
             const std::filesystem::path &file,
             const FlashOptions &options) const {
    auto operation = flash_file_async(serial, partition, file, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<void, Error>
  flash_file(std::optional<std::string_view> serial,
             std::string_view partition,
             const std::filesystem::path &file) const {
    return flash_file(serial, partition, file, FlashOptions{});
  }

  [[nodiscard]] std::expected<void, Error>
  flash_file(std::string_view partition, const std::filesystem::path &file,
             const FlashOptions &options) const {
    return flash_file(std::nullopt, partition, file, options);
  }

  [[nodiscard]] std::expected<void, Error>
  flash_file(std::string_view partition,
             const std::filesystem::path &file) const {
    return flash_file(std::nullopt, partition, file);
  }

  [[nodiscard]] std::expected<void, Error>
  flash_file(std::string_view serial, std::string_view partition,
             const std::filesystem::path &file,
             const FlashOptions &options) const {
    return flash_file(std::optional<std::string_view>{serial}, partition, file,
                      options);
  }

  [[nodiscard]] std::expected<void, Error>
  flash_file(std::string_view serial, std::string_view partition,
             const std::filesystem::path &file) const {
    return flash_file(std::optional<std::string_view>{serial}, partition, file);
  }

  [[nodiscard]] std::expected<Operation, Error>
  flash_vendor_boot_ramdisk_async(
      DeviceSelector selector, std::string_view partition,
      const std::filesystem::path &ramdisk,
      std::string_view ramdisk_name = "default",
      const std::optional<std::filesystem::path> &dtb = std::nullopt,
      const FlashOptions &options = {}) const {
    auto prepared = detail::prepare_flash_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    const auto to_utf8 = [](const std::filesystem::path &path) {
      const auto value = path.u8string();
      return std::string{value.begin(), value.end()};
    };
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string partition_storage{partition};
    const std::string name_storage{ramdisk_name};
    const std::string ramdisk_storage = to_utf8(ramdisk);
    const std::optional<std::string> dtb_storage =
        dtb.has_value() ? std::optional<std::string>{to_utf8(*dtb)}
                        : std::nullopt;
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_flash_vendor_boot_ramdisk_async(
        handle_, selector_value, partition_storage.c_str(),
        name_storage.c_str(), ramdisk_storage.c_str(),
        dtb_storage.has_value() ? dtb_storage->c_str() : nullptr,
        &prepared->native, &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<Operation, Error>
  flash_vendor_boot_ramdisk_async(
      std::string_view partition, const std::filesystem::path &ramdisk,
      std::string_view ramdisk_name = "default",
      const std::optional<std::filesystem::path> &dtb = std::nullopt,
      const FlashOptions &options = {}) const {
    return flash_vendor_boot_ramdisk_async(
        std::nullopt, partition, ramdisk, ramdisk_name, dtb, options);
  }

  [[nodiscard]] std::expected<void, Error> flash_vendor_boot_ramdisk(
      DeviceSelector selector, std::string_view partition,
      const std::filesystem::path &ramdisk,
      std::string_view ramdisk_name = "default",
      const std::optional<std::filesystem::path> &dtb = std::nullopt,
      const FlashOptions &options = {}) const {
    auto operation = flash_vendor_boot_ramdisk_async(
        selector, partition, ramdisk, ramdisk_name, dtb, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<void, Error> flash_vendor_boot_ramdisk(
      std::string_view partition, const std::filesystem::path &ramdisk,
      std::string_view ramdisk_name = "default",
      const std::optional<std::filesystem::path> &dtb = std::nullopt,
      const FlashOptions &options = {}) const {
    return flash_vendor_boot_ramdisk(
        std::nullopt, partition, ramdisk, ramdisk_name, dtb, options);
  }

  [[nodiscard]] std::expected<Operation, Error> flash_raw_async(
      DeviceSelector selector, std::string_view partition,
      const std::filesystem::path &kernel,
      const std::optional<std::filesystem::path> &ramdisk = std::nullopt,
      const std::optional<std::filesystem::path> &second_stage = std::nullopt,
      const FlashOptions &options = {}) const {
    auto prepared = detail::prepare_flash_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    const auto to_utf8 = [](const std::filesystem::path &path) {
      const auto value = path.u8string();
      return std::string{value.begin(), value.end()};
    };
    const std::string selector_string =
        selector.has_value() ? std::string{*selector} : std::string{};
    const std::string partition_string{partition};
    const std::string kernel_string = to_utf8(kernel);
    const std::optional<std::string> ramdisk_string =
        ramdisk.has_value()
            ? std::optional<std::string>{to_utf8(*ramdisk)}
            : std::nullopt;
    const std::optional<std::string> second_string =
        second_stage.has_value()
            ? std::optional<std::string>{to_utf8(*second_stage)}
            : std::nullopt;
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_flash_raw_async(
        handle_, selector.has_value() ? selector_string.c_str() : nullptr,
        partition_string.c_str(), kernel_string.c_str(),
        ramdisk_string.has_value() ? ramdisk_string->c_str() : nullptr,
        second_string.has_value() ? second_string->c_str() : nullptr,
        &prepared->native, &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<void, Error> flash_raw(
      DeviceSelector selector, std::string_view partition,
      const std::filesystem::path &kernel,
      const std::optional<std::filesystem::path> &ramdisk = std::nullopt,
      const std::optional<std::filesystem::path> &second_stage = std::nullopt,
      const FlashOptions &options = {}) const {
    auto operation = flash_raw_async(selector, partition, kernel, ramdisk,
                                     second_stage, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<Operation, Error> flash_raw_async(
      DeviceSelector selector, std::string_view partition,
      const std::filesystem::path &kernel,
      const std::optional<std::filesystem::path> &ramdisk,
      const std::optional<std::filesystem::path> &second_stage,
      const LegacyBootOptions &legacy_options,
      const FlashOptions &options = {}) const {
    auto prepared = detail::prepare_flash_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    auto legacy = detail::prepare_legacy_boot_options(legacy_options);
    if (!legacy) {
      return std::unexpected(std::move(legacy.error()));
    }
    legacy->native.command_line = legacy->command_line.c_str();
    const auto to_utf8 = [](const std::filesystem::path &path) {
      const auto value = path.u8string();
      return std::string{value.begin(), value.end()};
    };
    const std::string selector_string =
        selector.has_value() ? std::string{*selector} : std::string{};
    const std::string partition_string{partition};
    const std::string kernel_string = to_utf8(kernel);
    const std::optional<std::string> ramdisk_string =
        ramdisk.has_value()
            ? std::optional<std::string>{to_utf8(*ramdisk)}
            : std::nullopt;
    const std::optional<std::string> second_string =
        second_stage.has_value()
            ? std::optional<std::string>{to_utf8(*second_stage)}
            : std::nullopt;
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_flash_raw_with_boot_options_async(
        handle_, selector.has_value() ? selector_string.c_str() : nullptr,
        partition_string.c_str(), kernel_string.c_str(),
        ramdisk_string.has_value() ? ramdisk_string->c_str() : nullptr,
        second_string.has_value() ? second_string->c_str() : nullptr,
        &legacy->native, &prepared->native, &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<void, Error> flash_raw(
      DeviceSelector selector, std::string_view partition,
      const std::filesystem::path &kernel,
      const std::optional<std::filesystem::path> &ramdisk,
      const std::optional<std::filesystem::path> &second_stage,
      const LegacyBootOptions &legacy_options,
      const FlashOptions &options = {}) const {
    auto operation = flash_raw_async(selector, partition, kernel, ramdisk,
                                     second_stage, legacy_options, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<Operation, Error> boot_raw_async(
      DeviceSelector selector, const std::filesystem::path &kernel,
      const std::optional<std::filesystem::path> &ramdisk = std::nullopt,
      const std::optional<std::filesystem::path> &second_stage = std::nullopt,
      const LegacyBootOptions &legacy_options = {},
      const FlashOptions &options = {}) const {
    auto prepared = detail::prepare_flash_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    auto legacy = detail::prepare_legacy_boot_options(legacy_options);
    if (!legacy) {
      return std::unexpected(std::move(legacy.error()));
    }
    legacy->native.command_line = legacy->command_line.c_str();
    const auto to_utf8 = [](const std::filesystem::path &path) {
      const auto value = path.u8string();
      return std::string{value.begin(), value.end()};
    };
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const std::string kernel_string = to_utf8(kernel);
    const std::optional<std::string> ramdisk_string =
        ramdisk.has_value()
            ? std::optional<std::string>{to_utf8(*ramdisk)}
            : std::nullopt;
    const std::optional<std::string> second_string =
        second_stage.has_value()
            ? std::optional<std::string>{to_utf8(*second_stage)}
            : std::nullopt;
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_boot_raw_async(
        handle_, selector_value, kernel_string.c_str(),
        ramdisk_string.has_value() ? ramdisk_string->c_str() : nullptr,
        second_string.has_value() ? second_string->c_str() : nullptr,
        &legacy->native, &prepared->native, &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<void, Error> boot_raw(
      DeviceSelector selector, const std::filesystem::path &kernel,
      const std::optional<std::filesystem::path> &ramdisk = std::nullopt,
      const std::optional<std::filesystem::path> &second_stage = std::nullopt,
      const LegacyBootOptions &legacy_options = {},
      const FlashOptions &options = {}) const {
    auto operation = boot_raw_async(selector, kernel, ramdisk, second_stage,
                                    legacy_options, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<Operation, Error> boot_file_async(
      const DeviceSelector selector, const std::filesystem::path &file,
      const FlashOptions &options = {}) const {
    auto prepared = detail::prepare_flash_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const auto path_u8 = file.u8string();
    const std::string path_string{path_u8.begin(), path_u8.end()};
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_boot_file_async(
        handle_, selector_value, path_string.c_str(), &prepared->native,
        &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] std::expected<Operation, Error> boot_file_async(
      const std::filesystem::path &file,
      const FlashOptions &options = {}) const {
    return boot_file_async(std::nullopt, file, options);
  }

  [[nodiscard]] std::expected<Operation, Error> boot_file_async(
      const std::string_view selector, const std::filesystem::path &file,
      const FlashOptions &options = {}) const {
    return boot_file_async(DeviceSelector{selector}, file, options);
  }

  [[nodiscard]] std::expected<void, Error> boot_file(
      const DeviceSelector selector, const std::filesystem::path &file,
      const FlashOptions &options = {}) const {
    auto operation = boot_file_async(selector, file, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait();
  }

  [[nodiscard]] std::expected<void, Error> boot_file(
      const std::filesystem::path &file,
      const FlashOptions &options = {}) const {
    return boot_file(std::nullopt, file, options);
  }

  [[nodiscard]] std::expected<void, Error> boot_file(
      const std::string_view selector, const std::filesystem::path &file,
      const FlashOptions &options = {}) const {
    return boot_file(DeviceSelector{selector}, file, options);
  }

  [[nodiscard]] std::expected<Operation, Error> signature_file_async(
      const DeviceSelector selector, const std::filesystem::path &file,
      const CommandOptions &options = {}) const {
    std::string selector_storage;
    const char *selector_value = selector_pointer(selector, selector_storage);
    const auto path_u8 = file.u8string();
    const std::string path_storage{path_u8.begin(), path_u8.end()};
    return start_typed_operation(
        options,
        [&](const kb_command_options_t *native, kb_operation_t **operation,
            kb_error_t **error) {
          return ::kb_signature_file_async(
              handle_, selector_value, path_storage.c_str(), native,
              operation, error);
        });
  }

  [[nodiscard]] std::expected<Operation, Error> signature_file_async(
      const std::filesystem::path &file,
      const CommandOptions &options = {}) const {
    return signature_file_async(std::nullopt, file, options);
  }

  [[nodiscard]] std::expected<Operation, Error> signature_file_async(
      const std::string_view selector, const std::filesystem::path &file,
      const CommandOptions &options = {}) const {
    return signature_file_async(DeviceSelector{selector}, file, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> signature_file(
      const DeviceSelector selector, const std::filesystem::path &file,
      const CommandOptions &options = {}) const {
    auto operation = signature_file_async(selector, file, options);
    if (!operation) {
      return std::unexpected(std::move(operation.error()));
    }
    return operation->wait_result();
  }

  [[nodiscard]] std::expected<CommandResult, Error> signature_file(
      const std::filesystem::path &file,
      const CommandOptions &options = {}) const {
    return signature_file(std::nullopt, file, options);
  }

  [[nodiscard]] std::expected<CommandResult, Error> signature_file(
      const std::string_view selector, const std::filesystem::path &file,
      const CommandOptions &options = {}) const {
    return signature_file(DeviceSelector{selector}, file, options);
  }

private:
  template <typename Start>
  [[nodiscard]] std::expected<Operation, Error>
  start_typed_operation(const CommandOptions &options, Start &&start) const {
    auto prepared = detail::prepare_command_options(options);
    if (!prepared) {
      return std::unexpected(std::move(prepared.error()));
    }
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = std::invoke(
        std::forward<Start>(start), &prepared->native, &operation, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation, std::move(prepared->callback_state)};
  }

  [[nodiscard]] static const char *
  selector_pointer(const DeviceSelector selector, std::string &storage) {
    if (!selector.has_value()) {
      return nullptr;
    }
    storage.assign(selector->data(), selector->size());
    return storage.c_str();
  }

  explicit Context(kb_context_t *handle) noexcept : handle_(handle) {}
  kb_context_t *handle_{};
};

} // namespace kairosboot

#endif
