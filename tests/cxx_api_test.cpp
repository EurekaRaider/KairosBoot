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
static_assert(std::is_same_v<
              decltype(kairosboot::FlashOptions{}.sparse_limit_bytes),
              std::uint64_t>);
static_assert(std::is_same_v<decltype(kairosboot::FlashOptions{}.force), bool>);
static_assert(std::is_same_v<
              decltype(kairosboot::FlashOptions{}.filesystem_options),
              kairosboot::FilesystemOptions>);
static_assert(std::is_same_v<decltype(kairosboot::FlashOptions{}.disable_verity),
                             bool>);
static_assert(std::is_same_v<
              decltype(kairosboot::FlashOptions{}.disable_verification), bool>);
static_assert(std::is_same_v<decltype(kairosboot::UpdateOptions{}.timeout),
                             std::chrono::milliseconds>);
static_assert(std::is_same_v<
              decltype(kairosboot::UpdateOptions{}.disable_verity), bool>);
static_assert(std::is_same_v<
              decltype(kairosboot::UpdateOptions{}.disable_verification), bool>);
static_assert(std::is_same_v<decltype(kairosboot::FlashOptions{}.slot),
                             std::optional<std::string>>);
static_assert(std::is_same_v<decltype(kairosboot::UpdateOptions{}.timeout),
                             std::chrono::milliseconds>);
static_assert(std::is_same_v<decltype(kairosboot::UpdateOptions{}.active_slot),
                             std::optional<std::string>>);
static_assert(std::is_same_v<
              decltype(kairosboot::UpdateOptions{}.sparse_limit_bytes),
              std::uint64_t>);
static_assert(std::is_same_v<decltype(kairosboot::UpdateOptions{}.force), bool>);
static_assert(std::is_same_v<
              decltype(kairosboot::UpdateOptions{}.disable_super_optimization),
              bool>);
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

static_assert(!std::is_copy_constructible_v<kairosboot::Device>);
static_assert(std::is_nothrow_move_constructible_v<kairosboot::Device>);

template <typename T>
concept ContextHasLegacyPrimitive = requires(T &context) {
  context.getvar("product");
};

template <typename T>
concept ContextHasLegacyFlash = requires(T &context,
                                         std::filesystem::path image) {
  context.flash_file("boot", image);
};

template <typename T>
concept ContextHasLegacyUpdate = requires(T &context,
                                          std::filesystem::path package) {
  context.update_package(package);
};

template <typename T>
concept DeviceHasBatchFlash = requires(
    std::span<T *const> devices, std::filesystem::path image) {
  T::flash_file_batch_async(devices, "boot", image);
};

template <typename T>
concept DeviceHasBatchUpdate = requires(
    std::span<T *const> devices, std::filesystem::path package) {
  T::update_package_batch_async(devices, package);
};

static_assert(!ContextHasLegacyPrimitive<kairosboot::Context>);
static_assert(!ContextHasLegacyFlash<kairosboot::Context>);
static_assert(!ContextHasLegacyUpdate<kairosboot::Context>);
static_assert(!DeviceHasBatchFlash<kairosboot::Device>);
static_assert(!DeviceHasBatchUpdate<kairosboot::Device>);
static_assert(requires(kairosboot::Context &context,
                       kairosboot::Device &device,
                       kairosboot::CommandOptions options,
                       kairosboot::FlashOptions flash_options,
                       kairosboot::LegacyBootOptions legacy_boot_options,
                       kairosboot::UpdateOptions update_options,
                       std::filesystem::path package,
                       std::filesystem::path kernel,
                       std::span<const std::byte> bytes,
                       kairosboot::FetchRange range) {
  { context.open_device("tcp:127.0.0.1:5554") } ->
      std::same_as<std::expected<kairosboot::Device, kairosboot::Error>>;
  { device.identifier() } -> std::same_as<std::string_view>;
  { device.serial() } -> std::same_as<std::string_view>;
  { device.usb_path() } -> std::same_as<std::string_view>;
  { device.getvar_async("product", options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { device.getvar("product", options) } ->
      std::same_as<std::expected<kairosboot::CommandResult, kairosboot::Error>>;
  { device.update_package_async(package, update_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { device.update_package(package, update_options) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  { device.flash_raw_async("boot", kernel) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { device.flash_raw("boot", kernel) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  { device.flash_vendor_boot_ramdisk_async(
        "vendor_boot", package, "default", std::nullopt, flash_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { device.flash_vendor_boot_ramdisk(
        "vendor_boot", package, "default", std::nullopt, flash_options) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  { device.flash_raw_async("boot", kernel, std::nullopt, std::nullopt,
                           legacy_boot_options, flash_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { device.boot_raw_async(kernel, std::nullopt, std::nullopt,
                          legacy_boot_options, flash_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { device.boot_file_async(package, flash_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  { device.wipe_super_async(package, update_options) } ->
      std::same_as<std::expected<kairosboot::Operation, kairosboot::Error>>;
  device.erase_async("userdata", options);
  device.set_active_async("a", options);
  device.flashing_async(kairosboot::FlashingCommand::Unlock, options);
  device.gsi_async(kairosboot::GsiCommand::Status, options);
  device.snapshot_update_async(kairosboot::SnapshotUpdateCommand::Cancel,
                               options);
  device.create_logical_partition_async("system_ext", 0, options);
  device.delete_logical_partition_async("system_ext", options);
  device.resize_logical_partition_async("system_ext", UINT64_MAX, options);
  device.reboot_async(kairosboot::RebootTarget::Fastboot, options);
  device.continue_boot_async(options);
  device.oem_async("device-info", options);
  device.raw_command_async("getvar:all", options);
  device.signature_file_async(package, options);
  device.boot_async(options);
  device.stage_async(bytes, options);
  device.upload_async(options);
  device.fetch_async("vendor", range, options);
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
    CHECK(defaults->native.sparse_limit_bytes == 0U);
    CHECK(defaults->native.force == 0);
    CHECK(defaults->native.filesystem_options == KB_FILESYSTEM_OPTION_NONE);
    CHECK(defaults->native.progress_callback == nullptr);

    kairosboot::FlashOptions finite;
    finite.timeout = 250ms;
    finite.sparse_limit_bytes = 8U * 1024U * 1024U;
    finite.force = true;
    finite.filesystem_options.casefold = true;
    finite.filesystem_options.compress = true;
    const auto prepared = kairosboot::detail::prepare_flash_options(finite);
    CHECK(prepared.has_value());
    CHECK(prepared->native.timeout_ms == 250U);
    CHECK(prepared->native.sparse_limit_bytes == 8U * 1024U * 1024U);
    CHECK(prepared->native.force == 1);
    CHECK(prepared->native.filesystem_options ==
          (KB_FILESYSTEM_OPTION_CASEFOLD | KB_FILESYSTEM_OPTION_COMPRESS));

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
    options.header_version = 2U;
    options.os_version = "15.0.1";
    options.os_patch_level = "2025-02-05";
    options.dtb = std::filesystem::path{"board.dtb"};
    options.dtb_offset = 0x01200000ULL;
    auto prepared = kairosboot::detail::prepare_legacy_boot_options(options);
    CHECK(prepared.has_value());
    CHECK(prepared->command_line == "console=ttyS0");
    CHECK(prepared->native.page_size == 4096U);
    CHECK(prepared->native.header_version == 2U);
    CHECK(prepared->os_version == "15.0.1");
    CHECK(prepared->os_patch_level == "2025-02-05");
    CHECK(prepared->dtb_path == "board.dtb");
    CHECK(prepared->native.dtb_offset == 0x01200000ULL);
    kairosboot::detail::bind_legacy_boot_option_strings(*prepared);
    CHECK(std::string_view{prepared->native.os_version} == "15.0.1");
    CHECK(std::string_view{prepared->native.os_patch_level} == "2025-02-05");
    CHECK(std::string_view{prepared->native.dtb_path} == "board.dtb");

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
    CHECK(defaults->native.sparse_limit_bytes == 0U);
    CHECK(defaults->native.force == 0);
    CHECK(defaults->native.disable_super_optimization == 0);
    CHECK(defaults->native.progress_callback == nullptr);

    kairosboot::UpdateOptions finite;
    finite.timeout = 625ms;
    finite.wipe = true;
    finite.sparse_limit_bytes = 16U * 1024U * 1024U;
    finite.force = true;
    finite.filesystem_options.projid = true;
    finite.disable_super_optimization = true;
    const auto prepared = kairosboot::detail::prepare_update_options(finite);
    CHECK(prepared.has_value());
    CHECK(prepared->native.timeout_ms == 625U);
    CHECK(prepared->native.wipe == 1);
    CHECK(prepared->native.sparse_limit_bytes == 16U * 1024U * 1024U);
    CHECK(prepared->native.force == 1);
    CHECK(prepared->native.filesystem_options == KB_FILESYSTEM_OPTION_PROJID);
    CHECK(prepared->native.disable_super_optimization == 1);

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
  auto vendor_context = kairosboot::Context::create(
      kairosboot::ContextOptions{.usb_vendor_id = 0x18d1U});
  CHECK(vendor_context.has_value());

  const auto invalid_selector = context->open_device("unknown:device");
  CHECK(!invalid_selector.has_value());
  CHECK(invalid_selector.error().status() == KB_E_INVALID_ARGUMENT);
  CHECK(invalid_selector.error().device_identifier() == "unknown:device");

  auto tcp_device = context->open_device("tcp:127.0.0.1:1");
  CHECK(tcp_device.has_value());
  auto udp_device = context->open_device("udp:127.0.0.1:1");
  CHECK(udp_device.has_value());
  auto tcp_default_device = context->open_device("tcp:127.0.0.1");
  CHECK(tcp_default_device.has_value());
  CHECK(tcp_device->identifier() == "tcp:127.0.0.1:1");

  kairosboot::UpdateOptions invalid_update_timeout;
  invalid_update_timeout.timeout = -1ms;
  const auto rejected_update_timeout = tcp_device->update_package_async(
      std::filesystem::path{"unused-update-package"}, invalid_update_timeout);
  CHECK(!rejected_update_timeout.has_value());
  CHECK(rejected_update_timeout.error().status() == KB_E_INVALID_ARGUMENT);

  kairosboot::UpdateOptions invalid_update_slot;
  invalid_update_slot.active_slot = "b";
  const auto rejected_update_slot = tcp_device->update_package_async(
      std::filesystem::path{"unused-update-package"}, invalid_update_slot);
  CHECK(!rejected_update_slot.has_value());
  CHECK(rejected_update_slot.error().status() == KB_E_INVALID_ARGUMENT);

  kairosboot::FlashOptions invalid_flash_slot;
  invalid_flash_slot.active_slot = "b";
  auto rejected_flash_slot = kairosboot::detail::prepare_flash_options(
      invalid_flash_slot);
  CHECK(!rejected_flash_slot.has_value());
  CHECK(rejected_flash_slot.error().status() == KB_E_INVALID_ARGUMENT);

  auto missing_wipe = tcp_device->wipe_super(
      std::filesystem::path{
          "kairosboot-hermetic-super-empty-does-not-exist.img"});
  CHECK(!missing_wipe.has_value());
  CHECK(missing_wipe.error().status() == KB_E_IO);
  CHECK(missing_wipe.error().transfer_state() == KB_TRANSFER_NOT_SENT);

  const auto invalid_flashing = tcp_device->flashing_async(
      static_cast<kairosboot::FlashingCommand>(INT32_MAX));
  CHECK(!invalid_flashing.has_value());
  CHECK(invalid_flashing.error().status() == KB_E_INVALID_ARGUMENT);
  const auto invalid_gsi = tcp_device->gsi_async(
      static_cast<kairosboot::GsiCommand>(INT32_MAX));
  CHECK(!invalid_gsi.has_value());
  CHECK(invalid_gsi.error().status() == KB_E_INVALID_ARGUMENT);
  const auto invalid_snapshot = tcp_device->snapshot_update_async(
      static_cast<kairosboot::SnapshotUpdateCommand>(INT32_MAX));
  CHECK(!invalid_snapshot.has_value());
  CHECK(invalid_snapshot.error().status() == KB_E_INVALID_ARGUMENT);

  const auto empty_logical = tcp_device->create_logical_partition_async("", 0);
  CHECK(!empty_logical.has_value());
  CHECK(empty_logical.error().status() == KB_E_INVALID_ARGUMENT);
  const auto injected_logical =
      tcp_device->delete_logical_partition_async("system:other");
  CHECK(!injected_logical.has_value());
  CHECK(injected_logical.error().status() == KB_E_INVALID_ARGUMENT);
  const auto control_logical = tcp_device->resize_logical_partition_async(
      std::string_view{"bad\nname", 8}, 1);
  CHECK(!control_logical.has_value());
  CHECK(control_logical.error().status() == KB_E_INVALID_ARGUMENT);
  const auto oversized_logical = tcp_device->create_logical_partition_async(
      std::string(4096, 'x'), UINT64_MAX);
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
  auto update_operation = tcp_device->update_package_async(missing_update_package);
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
  auto cancelled_update =
      udp_device->update_package_async(missing_update_package, cancel_update);
  CHECK(cancelled_update.has_value());
  auto cancelled_wait = cancelled_update->wait(std::stop_token{});
  CHECK(!cancelled_wait.has_value());
  CHECK(cancelled_wait.error().status() == KB_E_CANCELLED);
  CHECK(update_callback_called);
  CHECK(update_callback_stage_valid);

  auto blocking_update = tcp_device->update_package(missing_update_package);
  CHECK(!blocking_update.has_value());
  CHECK(blocking_update.error().status() == KB_E_IO);
  CHECK(blocking_update.error().device_identifier() == "tcp:127.0.0.1:1");
  CHECK(blocking_update.error().message().find("update package") !=
        std::string::npos);

  auto operation = tcp_device->flash_file_async(
      "system", std::filesystem::path{"kairosboot-test-does-not-exist.img"});
  CHECK(!operation.has_value());
  CHECK(operation.error().status() == KB_E_IO);
  CHECK(operation.error().transfer_state() == KB_TRANSFER_NOT_SENT);

  auto flash = tcp_default_device->flash_file(
      "system",
      std::filesystem::path{"kairosboot-test-does-not-exist.img"});
  CHECK(!flash.has_value());
  CHECK(flash.error().status() == KB_E_IO);
  CHECK(flash.error().device_identifier() == "tcp:127.0.0.1");
  auto invalid_format = tcp_device->format_partition_async(
      "system", std::optional<std::string_view>{"xfs"});
  CHECK(!invalid_format.has_value());
  CHECK(invalid_format.error().status() == KB_E_INVALID_ARGUMENT);

  auto signature = tcp_default_device->signature_file(
      std::filesystem::path{"kairosboot-signature-missing.bin"});
  CHECK(!signature.has_value());
  CHECK(signature.error().status() == KB_E_IO);
  CHECK(signature.error().device_identifier() == "tcp:127.0.0.1");
  return 0;
}
