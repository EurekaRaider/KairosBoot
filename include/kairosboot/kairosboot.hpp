#ifndef KAIROSBOOT_KAIROSBOOT_HPP
#define KAIROSBOOT_KAIROSBOOT_HPP

#include <kairosboot/kairosboot.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kairosboot {

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
  explicit Operation(kb_operation_t *handle) noexcept : handle_(handle) {}
  ~Operation() { kb_operation_release(handle_); }

  Operation(const Operation &) = delete;
  Operation &operator=(const Operation &) = delete;

  Operation(Operation &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  Operation &operator=(Operation &&other) noexcept {
    if (this != &other) {
      kb_operation_release(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] kb_operation_state_t state() const noexcept {
    return kb_operation_state(handle_);
  }
  [[nodiscard]] std::expected<void, Error>
  wait(std::uint32_t timeout_ms = KB_WAIT_INFINITE) {
    const kb_status_t status = kb_operation_wait(handle_, timeout_ms);
    if (status != KB_OK) {
      return std::unexpected(
          detail_copy_error(status, kb_operation_error(handle_)));
    }
    return {};
  }
  [[nodiscard]] std::expected<void, Error> cancel() {
    const kb_status_t status = kb_operation_cancel(handle_);
    if (status != KB_OK) {
      return std::unexpected(detail_copy_error(status, nullptr));
    }
    return {};
  }

private:
  kb_operation_t *handle_{};
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
      const std::filesystem::path &file) const {
    const std::string serial_string =
        serial.has_value() ? std::string{*serial} : std::string{};
    const std::string partition_string{partition};
    const auto path_u8 = file.u8string();
    const std::string path_string{path_u8.begin(), path_u8.end()};
    kb_operation_t *operation = nullptr;
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_flash_file_async(
        handle_, serial.has_value() ? serial_string.c_str() : nullptr,
        partition_string.c_str(), path_string.c_str(), nullptr, &operation,
        &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return Operation{operation};
  }

  [[nodiscard]] std::expected<Operation, Error>
  flash_file_async(std::string_view partition,
                   const std::filesystem::path &file) const {
    return flash_file_async(std::nullopt, partition, file);
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
             const std::filesystem::path &file) const {
    const std::string serial_string =
        serial.has_value() ? std::string{*serial} : std::string{};
    const std::string partition_string{partition};
    const auto path_u8 = file.u8string();
    const std::string path_string{path_u8.begin(), path_u8.end()};
    kb_error_t *error = nullptr;
    const kb_status_t status = ::kb_flash_file(
        handle_, serial.has_value() ? serial_string.c_str() : nullptr,
        partition_string.c_str(), path_string.c_str(), nullptr, &error);
    if (status != KB_OK) {
      return std::unexpected(detail_take_error(status, error));
    }
    return {};
  }

  [[nodiscard]] std::expected<void, Error>
  flash_file(std::string_view partition,
             const std::filesystem::path &file) const {
    return flash_file(std::nullopt, partition, file);
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
