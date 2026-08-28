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
static_assert(std::is_same_v<decltype(kairosboot::UpdateOptions{}.timeout),
                             std::chrono::milliseconds>);
static_assert(std::is_same_v<decltype(kairosboot::CommandOptions{}.timeout),
                             std::chrono::milliseconds>);
static_assert(!std::is_convertible_v<kairosboot::ProgressAction, int>);
static_assert(!std::is_convertible_v<kairosboot::FlashingCommand, int>);
static_assert(!std::is_convertible_v<kairosboot::GsiCommand, int>);
static_assert(!std::is_convertible_v<kairosboot::SnapshotUpdateCommand, int>);
static_assert(
    std::is_same_v<std::underlying_type_t<kairosboot::FlashingCommand>,
                   kb_flashing_command_t>);
static_assert(std::is_same_v<std::underlying_type_t<kairosboot::GsiCommand>,
                             kb_gsi_command_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<kairosboot::SnapshotUpdateCommand>,
                   kb_snapshot_update_command_t>);
static_assert(noexcept(kairosboot::detail::progress_trampoline(nullptr,
                                                               nullptr)));

static_assert(requires(kairosboot::Context &context,
                       kairosboot::DeviceSelector selector,
                       kairosboot::CommandOptions options,
                       kairosboot::FlashOptions flash_options,
                       kairosboot::LegacyBootOptions legacy_boot_options,
                       kairosboot::UpdateOptions update_options,
                       std::filesystem::path package,
                       std::filesystem::path kernel,
                       std::span<const std::byte> bytes,
                       kairosboot::FetchRange range) {
  { context.getvar_async(selector, "product", options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { context.getvar(selector, "product", options) } ->
      std::same_as<std::expected<kairosboot::CommandResult, kairosboot::Error>>;
  { context.update_package_async(selector, package, update_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { context.update_package(selector, package, update_options) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  { context.flash_raw_async(selector, "boot", kernel) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { context.flash_raw(selector, "boot", kernel) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  { context.flash_raw_async(selector, "boot", kernel, std::nullopt,
                            std::nullopt, legacy_boot_options, flash_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { context.boot_raw_async(selector, kernel, std::nullopt, std::nullopt,
                           legacy_boot_options, flash_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { context.boot_raw(selector, kernel, std::nullopt, std::nullopt,
                     legacy_boot_options, flash_options) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  { context.boot_file_async(selector, package, flash_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { context.boot_file(selector, package, flash_options) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  { context.wipe_super_async(selector, package, update_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { context.wipe_super(selector, package, update_options) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  context.erase_async(selector, "userdata", options);
  context.erase(selector, "userdata", options);
  context.set_active_async(selector, "a", options);
  context.set_active(selector, "a", options);
  context.flashing_async(selector, kairosboot::FlashingCommand::Unlock,
                         options);
  context.flashing(selector, kairosboot::FlashingCommand::Lock, options);
  context.flashing_async(kairosboot::FlashingCommand::Lock, options);
  context.flashing(kairosboot::FlashingCommand::Unlock, options);
  context.gsi_async(selector, kairosboot::GsiCommand::Status, options);
  context.gsi(selector, kairosboot::GsiCommand::Wipe, options);
  context.gsi_async(kairosboot::GsiCommand::Disable, options);
  context.gsi(kairosboot::GsiCommand::Status, options);
  context.snapshot_update_async(
      selector, kairosboot::SnapshotUpdateCommand::Cancel, options);
  context.snapshot_update(selector, kairosboot::SnapshotUpdateCommand::Merge,
                          options);
  context.snapshot_update_async(kairosboot::SnapshotUpdateCommand::Merge,
                                options);
  context.snapshot_update(kairosboot::SnapshotUpdateCommand::Cancel, options);
  context.create_logical_partition_async(selector, "system_ext", 0, options);
  context.create_logical_partition(selector, "system_ext", 0, options);
  context.create_logical_partition_async("system_ext", 0, options);
  context.create_logical_partition("system_ext", 0, options);
  context.delete_logical_partition_async(selector, "system_ext", options);
  context.delete_logical_partition(selector, "system_ext", options);
  context.delete_logical_partition_async("system_ext", options);
  context.delete_logical_partition("system_ext", options);
  context.resize_logical_partition_async(selector, "system_ext", UINT64_MAX,
                                         options);
  context.resize_logical_partition(selector, "system_ext", UINT64_MAX, options);
  context.resize_logical_partition_async("system_ext", UINT64_MAX, options);
  context.resize_logical_partition("system_ext", UINT64_MAX, options);
  context.reboot_async(selector, kairosboot::RebootTarget::Fastboot, options);
  context.reboot(selector, kairosboot::RebootTarget::Recovery, options);
  context.continue_boot_async(selector, options);
  context.continue_boot(selector, options);
  context.oem_async(selector, "device-info", options);
  context.oem(selector, "device-info", options);
  context.raw_command_async(selector, "getvar:all", options);
  context.raw_command(selector, "getvar:all", options);
  context.signature_file_async(selector, package, options);
  context.signature_file(selector, package, options);
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
  { operation.wait(stop_token) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
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
    kairosboot::LegacyBootOptions options;
    options.command_line = "console=ttyS0";
    options.page_size = 4096U;
    auto prepared = kairosboot::detail::prepare_legacy_boot_options(options);
    CHECK(prepared.has_value());
    CHECK(prepared->command_line == "console=ttyS0");
    CHECK(prepared->native.page_size == 4096U);

    options.command_line = std::string{"bad\0cmdline", 11U};
    prepared = kairosboot::detail::prepare_legacy_boot_options(options);
    CHECK(!prepared.has_value());
    CHECK(prepared.error().status() == KB_E_INVALID_ARGUMENT);
  }

  {
    const auto defaults =
        kairosboot::detail::prepare_update_options(kairosboot::UpdateOptions{});
    CHECK(defaults.has_value());
    CHECK(defaults->native.timeout_ms == KB_WAIT_INFINITE);
    CHECK(defaults->native.wipe == 0);
    CHECK(defaults->native.progress_callback == nullptr);

    kairosboot::UpdateOptions finite;
    finite.timeout = 625ms;
    finite.wipe = true;
    const auto prepared = kairosboot::detail::prepare_update_options(finite);
    CHECK(prepared.has_value());
    CHECK(prepared->native.timeout_ms == 625U);
    CHECK(prepared->native.wipe == 1);

    finite.timeout = -1ms;
    const auto negative = kairosboot::detail::prepare_update_options(finite);
    CHECK(!negative.has_value());
    CHECK(negative.error().status() == KB_E_INVALID_ARGUMENT);
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
    bool callback_called = false;
    kairosboot::Progress observed;
    kairosboot::UpdateOptions options;
    options.progress = [&](const kairosboot::Progress &progress) {
      callback_called = true;
      observed = progress;
      return kairosboot::ProgressAction::Continue;
    };
    auto prepared = kairosboot::detail::prepare_update_options(options);
    CHECK(prepared.has_value());
    CHECK(prepared->native.progress_callback != nullptr);
    kb_progress_t native_progress{
        sizeof(kb_progress_t), KB_API_VERSION, 13, 21, "preflight",
        "tcp:127.0.0.1:5554"};
    CHECK(prepared->native.progress_callback(
              &native_progress, prepared->native.progress_user_data) ==
          KB_PROGRESS_CONTINUE);
    CHECK(callback_called);
    CHECK(observed.bytes_completed == 13);
    CHECK(observed.bytes_total == 21);
    CHECK(observed.stage == "preflight");
    CHECK(observed.device_identifier == "tcp:127.0.0.1:5554");
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

  const auto invalid_update_selector = context->update_package_async(
      kairosboot::DeviceSelector{"unknown:device"},
      std::filesystem::path{"unused-update-package"});
  CHECK(!invalid_update_selector.has_value());
  CHECK(invalid_update_selector.error().status() == KB_E_INVALID_ARGUMENT);
  CHECK(invalid_update_selector.error().device_identifier() ==
        "unknown:device");

  kairosboot::UpdateOptions invalid_update_timeout;
  invalid_update_timeout.timeout = -1ms;
  const auto rejected_update_timeout = context->update_package_async(
      kairosboot::DeviceSelector{"tcp:127.0.0.1:1"},
      std::filesystem::path{"unused-update-package"},
      invalid_update_timeout);
  CHECK(!rejected_update_timeout.has_value());
  CHECK(rejected_update_timeout.error().status() == KB_E_INVALID_ARGUMENT);

  const auto invalid_wipe_selector = context->wipe_super_async(
      kairosboot::DeviceSelector{"unknown:device"},
      std::filesystem::path{"unused-super-empty.img"});
  CHECK(!invalid_wipe_selector.has_value());
  CHECK(invalid_wipe_selector.error().status() == KB_E_INVALID_ARGUMENT);

  auto missing_wipe = context->wipe_super(
      kairosboot::DeviceSelector{"tcp:127.0.0.1:1"},
      std::filesystem::path{
          "kairosboot-hermetic-super-empty-does-not-exist.img"});
  CHECK(!missing_wipe.has_value());
  CHECK(missing_wipe.error().status() == KB_E_IO);
  CHECK(missing_wipe.error().transfer_state() == KB_TRANSFER_NOT_SENT);

  const kairosboot::DeviceSelector invalid_target{"unknown:device"};
  const auto invalid_flashing = context->flashing_async(
      invalid_target, static_cast<kairosboot::FlashingCommand>(INT32_MAX));
  CHECK(!invalid_flashing.has_value());
  CHECK(invalid_flashing.error().status() == KB_E_INVALID_ARGUMENT);
  const auto invalid_gsi = context->gsi_async(
      invalid_target, static_cast<kairosboot::GsiCommand>(INT32_MAX));
  CHECK(!invalid_gsi.has_value());
  CHECK(invalid_gsi.error().status() == KB_E_INVALID_ARGUMENT);
  const auto invalid_snapshot = context->snapshot_update_async(
      invalid_target,
      static_cast<kairosboot::SnapshotUpdateCommand>(INT32_MAX));
  CHECK(!invalid_snapshot.has_value());
  CHECK(invalid_snapshot.error().status() == KB_E_INVALID_ARGUMENT);

  const auto empty_logical =
      context->create_logical_partition_async(invalid_target, "", 0);
  CHECK(!empty_logical.has_value());
  CHECK(empty_logical.error().status() == KB_E_INVALID_ARGUMENT);
  const auto injected_logical =
      context->delete_logical_partition_async(invalid_target, "system:other");
  CHECK(!injected_logical.has_value());
  CHECK(injected_logical.error().status() == KB_E_INVALID_ARGUMENT);
  const auto control_logical = context->resize_logical_partition_async(
      invalid_target, std::string_view{"bad\nname", 8}, 1);
  CHECK(!control_logical.has_value());
  CHECK(control_logical.error().status() == KB_E_INVALID_ARGUMENT);
  const auto oversized_logical = context->create_logical_partition_async(
      invalid_target, std::string(4096, 'x'), UINT64_MAX);
  CHECK(!oversized_logical.has_value());
  CHECK(oversized_logical.error().status() == KB_E_INVALID_ARGUMENT);

  auto devices = context->devices();
  CHECK(devices.has_value());
  for (std::size_t index = 0; index < devices->size(); ++index) {
    static_cast<void>(devices->serial(index));
    static_cast<void>(devices->usb_path(index));
    static_cast<void>(devices->product(index));
  }

  const auto missing_update_package =
      std::filesystem::path{"kairosboot-hermetic-update-package-does-not-exist"};
  auto update_operation = context->update_package_async(
      kairosboot::DeviceSelector{"tcp:127.0.0.1:1"},
      missing_update_package);
  CHECK(update_operation.has_value());
  auto update_waited = update_operation->wait();
  CHECK(!update_waited.has_value());
  CHECK(update_waited.error().status() == KB_E_IO);
  CHECK(update_waited.error().device_identifier() == "tcp:127.0.0.1:1");
  CHECK(update_waited.error().message().find("update package") !=
        std::string::npos);
  CHECK(update_waited.error().transfer_state() == KB_TRANSFER_NOT_SENT);

  bool update_callback_called = false;
  bool update_callback_stage_valid = false;
  kairosboot::UpdateOptions cancel_update;
  cancel_update.progress =
      [&update_callback_called,
       &update_callback_stage_valid](const kairosboot::Progress &progress) {
        update_callback_called = true;
        update_callback_stage_valid = progress.stage == "preflight";
        return kairosboot::ProgressAction::Cancel;
      };
  auto cancelled_update = context->update_package_async(
      kairosboot::DeviceSelector{"udp:127.0.0.1:1"},
      missing_update_package, cancel_update);
  CHECK(cancelled_update.has_value());
  auto cancelled_wait = cancelled_update->wait(std::stop_token{});
  CHECK(!cancelled_wait.has_value());
  CHECK(cancelled_wait.error().status() == KB_E_CANCELLED);
  CHECK(update_callback_called);
  CHECK(update_callback_stage_valid);

  auto blocking_update = context->update_package(
      kairosboot::DeviceSelector{"tcp:127.0.0.1:1"},
      missing_update_package);
  CHECK(!blocking_update.has_value());
  CHECK(blocking_update.error().status() == KB_E_IO);
  CHECK(blocking_update.error().device_identifier() == "tcp:127.0.0.1:1");
  CHECK(blocking_update.error().message().find("update package") !=
        std::string::npos);

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
  auto invalid_format = context->format_partition_async(
      "system", std::optional<std::string_view>{"xfs"});
  CHECK(!invalid_format.has_value());
  CHECK(invalid_format.error().status() == KB_E_INVALID_ARGUMENT);

  auto signature = context->signature_file(
      kairosboot::DeviceSelector{"ABC"},
      std::filesystem::path{"kairosboot-signature-missing.bin"});
  CHECK(!signature.has_value());
  CHECK(signature.error().status() == KB_E_IO);
  CHECK(signature.error().device_identifier() == "ABC");
  return 0;
}
