#include <kairosboot/kairosboot.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <span>
#include <stop_token>
#include <type_traits>
#include <vector>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "check failed at line " << __LINE__ << ": " #condition    \
                << '\n';                                                       \
      return __LINE__;                                                         \
    }                                                                          \
  } while (false)

static_assert(__cplusplus >= 202100L);
static_assert(!std::is_copy_constructible_v<kairosboot::Context>);
static_assert(std::is_move_constructible_v<kairosboot::Context>);
static_assert(!std::is_copy_constructible_v<kairosboot::Operation>);
static_assert(std::is_nothrow_move_constructible_v<kairosboot::Operation>);
static_assert(std::is_nothrow_move_assignable_v<kairosboot::Operation>);
static_assert(!std::is_copy_constructible_v<
              kairosboot::detail::OperationResources>);
static_assert(std::is_nothrow_move_constructible_v<
              kairosboot::detail::OperationResources>);
static_assert(std::is_nothrow_move_assignable_v<
              kairosboot::detail::OperationResources>);
static_assert(!std::is_copy_constructible_v<kairosboot::CommandResult>);
static_assert(std::is_nothrow_move_constructible_v<kairosboot::CommandResult>);
static_assert(std::is_nothrow_move_assignable_v<kairosboot::CommandResult>);
static_assert(std::is_copy_constructible_v<kairosboot::Error>);
static_assert(std::is_same_v<decltype(kairosboot::FlashOptions{}.timeout),
                             std::chrono::milliseconds>);
static_assert(std::is_same_v<decltype(kairosboot::CommandOptions{}.timeout),
                             std::chrono::milliseconds>);
static_assert(!std::is_convertible_v<kairosboot::ProgressAction, int>);
static_assert(noexcept(kairosboot::detail::progress_trampoline(nullptr,
                                                               nullptr)));

static_assert(requires(kairosboot::Context &context,
                       kairosboot::DeviceSelector selector,
                       kairosboot::CommandOptions options,
                       std::span<const std::byte> bytes,
                       kairosboot::FetchRange range) {
  { context.getvar_async(selector, "product", options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { context.getvar(selector, "product", options) } ->
      std::same_as<std::expected<kairosboot::CommandResult, kairosboot::Error>>;
  context.erase_async(selector, "userdata", options);
  context.erase(selector, "userdata", options);
  context.set_active_async(selector, "a", options);
  context.set_active(selector, "a", options);
  context.reboot_async(selector, kairosboot::RebootTarget::Fastboot, options);
  context.reboot(selector, kairosboot::RebootTarget::Recovery, options);
  context.continue_boot_async(selector, options);
  context.continue_boot(selector, options);
  context.oem_async(selector, "device-info", options);
  context.oem(selector, "device-info", options);
  context.raw_command_async(selector, "getvar:all", options);
  context.raw_command(selector, "getvar:all", options);
  context.boot_async(selector, options);
  context.boot(selector, options);
  context.stage_async(selector, bytes, options);
  context.stage(selector, bytes, options);
  context.upload_async(selector, options);
  context.upload(selector, options);
  context.fetch_async(selector, "vendor", range, options);
  context.fetch(selector, "vendor", range, options);
});
static_assert(requires(kairosboot::Operation &operation,
                       std::stop_token stop_token) {
  { operation.command_result() } ->
      std::same_as<
          std::expected<kairosboot::CommandResult, kairosboot::Error>>;
  { operation.wait_result(stop_token) } ->
      std::same_as<
          std::expected<kairosboot::CommandResult, kairosboot::Error>>;
});

namespace {

using namespace std::chrono_literals;

struct LifetimeProbe final {
  explicit LifetimeProbe(bool &destroyed_value) : destroyed(destroyed_value) {}
  ~LifetimeProbe() { destroyed = true; }
  bool &destroyed;
};

struct AllocationFailureOnCopy final {
  explicit AllocationFailureOnCopy(bool &copy_attempted_value)
      : copy_attempted(copy_attempted_value) {}

  AllocationFailureOnCopy(const AllocationFailureOnCopy &other)
      : copy_attempted(other.copy_attempted) {
    copy_attempted = true;
    throw std::bad_alloc{};
  }
  AllocationFailureOnCopy(AllocationFailureOnCopy &&) noexcept = default;

  kairosboot::ProgressAction
  operator()(const kairosboot::FlashProgress &) const {
    return kairosboot::ProgressAction::Continue;
  }

  bool &copy_attempted;
};

} // namespace

int main() {
  {
    const auto defaults =
        kairosboot::detail::prepare_flash_options(kairosboot::FlashOptions{});
    CHECK(defaults.has_value());
    CHECK(defaults->native.timeout_ms == KB_WAIT_INFINITE);
    CHECK(defaults->native.progress_callback == nullptr);

    kairosboot::FlashOptions finite;
    finite.timeout = 250ms;
    const auto prepared = kairosboot::detail::prepare_flash_options(finite);
    CHECK(prepared.has_value());
    CHECK(prepared->native.timeout_ms == 250U);

    finite.timeout = -1ms;
    const auto negative = kairosboot::detail::prepare_flash_options(finite);
    CHECK(!negative.has_value());
    CHECK(negative.error().status() == KB_E_INVALID_ARGUMENT);
    CHECK(!negative.error().message().empty());

    finite.timeout =
        std::chrono::milliseconds{static_cast<std::int64_t>(UINT32_MAX)};
    const auto sentinel = kairosboot::detail::prepare_flash_options(finite);
    CHECK(!sentinel.has_value());
    CHECK(sentinel.error().status() == KB_E_INVALID_ARGUMENT);
  }

  {
    const auto defaults = kairosboot::detail::prepare_command_options(
        kairosboot::CommandOptions{});
    CHECK(defaults.has_value());
    CHECK(defaults->native.timeout_ms == KB_WAIT_INFINITE);
    CHECK(defaults->native.maximum_receive_bytes == 64U * 1024U * 1024U);
    CHECK(defaults->native.progress_callback == nullptr);

    kairosboot::CommandOptions finite;
    finite.timeout = 375ms;
    finite.maximum_receive_bytes = 4096;
    const auto prepared = kairosboot::detail::prepare_command_options(finite);
    CHECK(prepared.has_value());
    CHECK(prepared->native.timeout_ms == 375U);
    CHECK(prepared->native.maximum_receive_bytes == 4096U);

    finite.maximum_receive_bytes = 0;
    const auto zero_bound =
        kairosboot::detail::prepare_command_options(finite);
    CHECK(!zero_bound.has_value());
    CHECK(zero_bound.error().status() == KB_E_INVALID_ARGUMENT);
  }

  {
    bool callback_called = false;
    kairosboot::FlashProgress observed;
    kairosboot::FlashOptions options;
    options.progress = [&](const kairosboot::FlashProgress &progress) {
      callback_called = true;
      observed = progress;
      return kairosboot::ProgressAction::Continue;
    };
    auto prepared = kairosboot::detail::prepare_flash_options(options);
    CHECK(prepared.has_value());
    CHECK(prepared->native.progress_callback != nullptr);
    kb_progress_t native_progress{
        sizeof(kb_progress_t), KB_API_VERSION, 7, 11, "download", "SERIAL-CXX"};
    CHECK(prepared->native.progress_callback(
              &native_progress, prepared->native.progress_user_data) ==
          KB_PROGRESS_CONTINUE);
    CHECK(callback_called);
    CHECK(observed.bytes_completed == 7);
    CHECK(observed.bytes_total == 11);
    CHECK(observed.stage == "download");
    CHECK(observed.device_identifier == "SERIAL-CXX");

    prepared->callback_state->callback =
        [](const kairosboot::FlashProgress &) -> kairosboot::ProgressAction {
      throw std::runtime_error{"callback failure"};
    };
    CHECK(prepared->native.progress_callback(
              &native_progress, prepared->native.progress_user_data) ==
          KB_PROGRESS_CANCEL);
    CHECK(kairosboot::detail::progress_trampoline(nullptr, nullptr) ==
          KB_PROGRESS_CANCEL);
  }

  {
    bool destroyed = false;
    {
      auto probe = std::make_shared<LifetimeProbe>(destroyed);
      kairosboot::FlashOptions options;
      options.progress = [probe](const kairosboot::FlashProgress &) {
        return kairosboot::ProgressAction::Continue;
      };
      auto prepared = kairosboot::detail::prepare_flash_options(options);
      CHECK(prepared.has_value());
      options.progress = {};
      probe.reset();
      CHECK(!destroyed);
    }
    CHECK(destroyed);
  }

  {
    bool copy_attempted = false;
    kairosboot::FlashOptions options;
    options.progress = AllocationFailureOnCopy{copy_attempted};
    copy_attempted = false;
    try {
      static_cast<void>(kairosboot::detail::prepare_flash_options(options));
      CHECK(false);
    } catch (const std::bad_alloc &) {
      CHECK(copy_attempted);
    }
  }

  const auto version = kairosboot::version();
  CHECK(version.api_version == KB_API_VERSION);
  CHECK(!version.string.empty());

  auto context = kairosboot::Context::create();
  CHECK(context.has_value());

  const auto invalid_selector = context->getvar_async(
      kairosboot::DeviceSelector{"unknown:device"}, "product");
  CHECK(!invalid_selector.has_value());
  CHECK(invalid_selector.error().status() == KB_E_INVALID_ARGUMENT);
  CHECK(invalid_selector.error().device_identifier() == "unknown:device");

  auto devices = context->devices();
  CHECK(devices.has_value());
  for (std::size_t index = 0; index < devices->size(); ++index) {
    static_cast<void>(devices->serial(index));
    static_cast<void>(devices->usb_path(index));
    static_cast<void>(devices->product(index));
  }

  auto operation = context->flash_file_async(
      "system", std::filesystem::path{"kairosboot-test-does-not-exist.img"});
  CHECK(!operation.has_value());
  CHECK(operation.error().status() == KB_E_IO);
  CHECK(operation.error().transfer_state() == KB_TRANSFER_NOT_SENT);

  auto flash = context->flash_file(
      std::optional<std::string_view>{"ABC"}, "system",
      std::filesystem::path{"kairosboot-test-does-not-exist.img"});
  CHECK(!flash.has_value());
  CHECK(flash.error().status() == KB_E_IO);
  CHECK(flash.error().device_identifier() == "ABC");
  return 0;
}
