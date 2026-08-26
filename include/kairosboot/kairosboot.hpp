#ifndef KAIROSBOOT_KAIROSBOOT_HPP
#define KAIROSBOOT_KAIROSBOOT_HPP

#include <kairosboot/kairosboot.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kairosboot {

enum class ProgressAction : std::uint8_t {
  Continue,
  Cancel,
};

struct FlashProgress {
  std::uint64_t bytes_completed{};
  std::uint64_t bytes_total{};
  // These views are valid only for the duration of the progress callback.
  std::string_view stage;
  std::string_view device_identifier;
};

struct FlashOptions {
  // Per-I/O timeout. milliseconds::max() selects the native infinite default.
  std::chrono::milliseconds timeout{std::chrono::milliseconds::max()};
  std::function<ProgressAction(const FlashProgress &)> progress;
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

private:
  friend Error detail_take_error(kb_status_t, kb_error_t *);
  friend Error detail_copy_error(kb_status_t, const kb_error_t *);
  friend Error detail_make_error(kb_status_t, std::string);

  kb_status_t status_{KB_E_INTERNAL};
  std::string message_;
  std::string device_identifier_;
  std::int32_t native_code_{0};
  kb_transfer_state_t transfer_state_{KB_TRANSFER_NOT_SENT};
};

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
  }
  return result;
}

inline Error detail_take_error(kb_status_t fallback, kb_error_t *handle) {
  Error result = detail_copy_error(fallback, handle);
  kb_error_release(handle);
  return result;
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
};

[[nodiscard]] inline std::expected<PreparedFlashOptions, Error>
prepare_flash_options(const FlashOptions &options) {
  std::uint32_t timeout_ms = KB_WAIT_INFINITE;
  if (options.timeout != std::chrono::milliseconds::max()) {
    const auto count = options.timeout.count();
    if (count < 0 ||
        static_cast<std::uint64_t>(count) >=
            static_cast<std::uint64_t>(KB_WAIT_INFINITE)) {
      return std::unexpected(detail_make_error(
          KB_E_INVALID_ARGUMENT,
          "flash timeout must be non-negative and less than UINT32_MAX "
          "milliseconds, or std::chrono::milliseconds::max()"));
    }
    timeout_ms = static_cast<std::uint32_t>(count);
  }

  PreparedFlashOptions result;
  kb_flash_options_init(&result.native);
  result.native.timeout_ms = timeout_ms;
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
  [[nodiscard]] std::expected<void, Error> cancel() {
    const kb_status_t status = kb_operation_cancel(resources_.handle());
    if (status != KB_OK) {
      return std::unexpected(detail_copy_error(status, nullptr));
    }
    return {};
  }

private:
  friend class Context;

  Operation(
      kb_operation_t *handle,
      std::shared_ptr<detail::ProgressCallbackState> callback_state) noexcept
      : resources_(handle, std::move(callback_state)) {}

  detail::OperationResources resources_;
};

class Context {
public:
  static std::expected<Context, Error> create() {
    kb_context_t *handle = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = kb_context_create(nullptr, &handle, &error);
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

private:
  explicit Context(kb_context_t *handle) noexcept : handle_(handle) {}
  kb_context_t *handle_{};
};

} // namespace kairosboot

#endif
