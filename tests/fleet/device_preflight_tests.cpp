// SPDX-License-Identifier: MIT
#include "src/fleet/device_preflight.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::FastbootUsbMode;
using kairosboot::fleet::DevicePreflightError;
using kairosboot::fleet::DevicePreflightErrorKind;
using kairosboot::fleet::DevicePreflightOpenError;
using kairosboot::fleet::DevicePreflightOpenErrorCode;
using kairosboot::fleet::DevicePreflightOutcomeCode;
using kairosboot::fleet::DevicePreflightProbeError;
using kairosboot::fleet::DevicePreflightProbeErrorCode;
using kairosboot::fleet::DevicePreflightProbeResult;
using kairosboot::fleet::DevicePreflightTimePoint;
using kairosboot::fleet::DevicePreflightUsbFingerprint;
using kairosboot::fleet::DevicePreflightUsbIdentity;
using kairosboot::fleet::FastbootDevicePreflightProbe;
using kairosboot::fleet::FlashJobManifest;
using kairosboot::fleet::IDevicePreflightProbe;
using kairosboot::fleet::IDevicePreflightSessionOpener;
using kairosboot::fleet::JobPlan;
using kairosboot::fleet::LocatedManifestString;
using kairosboot::fleet::ManifestArtifact;
using kairosboot::fleet::ManifestEraseStep;
using kairosboot::fleet::ManifestPolicy;
using kairosboot::fleet::ManifestSelector;
using kairosboot::fleet::ManifestSourceLocation;
using kairosboot::fleet::ManifestStep;
using kairosboot::fleet::ManifestTarget;
using kairosboot::fleet::OpenedDevicePreflightSession;
using kairosboot::fleet::PreparedDeviceBatch;
using kairosboot::fleet::PreparedDeviceBatchConsumption;
using kairosboot::fleet::PreparedDeviceBatchConsumptionError;
using kairosboot::fleet::PreparedDeviceSession;
using kairosboot::fleet::make_job_plan;
using kairosboot::fleet::preflight_fleet_devices;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferResult;
using kairosboot::protocol::TransportStatus;
using kairosboot::transport::LinuxUsbTopology;
using kairosboot::transport::MacUsbInterfaceFingerprint;
using kairosboot::transport::MacUsbTopology;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::WindowsUsbInterfaceFingerprint;
using kairosboot::transport::WindowsUsbTopology;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw CheckFailure(std::string("check failed at line ") +         \
                               std::to_string(__LINE__) + ": " #condition);    \
        }                                                                       \
    } while (false)

static_assert(!std::is_copy_constructible_v<PreparedDeviceBatch>);
static_assert(!std::is_copy_assignable_v<PreparedDeviceBatch>);
static_assert(std::is_nothrow_move_constructible_v<PreparedDeviceBatch>);
static_assert(!std::is_copy_constructible_v<PreparedDeviceSession>);
static_assert(std::is_nothrow_move_constructible_v<PreparedDeviceSession>);

template <typename Value>
concept HasPublicTakeSession = requires(Value&& value) {
    std::move(value).take_session();
};

template <typename Value>
concept HasPublicTakeDevices = requires(Value&& value) {
    std::move(value).take_devices();
};

static_assert(!HasPublicTakeSession<PreparedDeviceSession>);
static_assert(!HasPublicTakeDevices<PreparedDeviceBatch>);
static_assert(!HasPublicTakeDevices<PreparedDeviceBatchConsumption>);

inline constexpr ManifestSourceLocation kLocation{1U, 1U};
inline constexpr auto kDeadlineBudget = std::chrono::seconds{5};

[[nodiscard]] LocatedManifestString located(std::string value) {
    return {.value = std::move(value), .location = kLocation};
}

struct TargetSpec final {
    std::string name;
    std::vector<std::string> serials;
    std::vector<std::string> usb_paths;
    std::string product;
};

[[nodiscard]] std::vector<LocatedManifestString> located_values(
    std::vector<std::string> values) {
    std::vector<LocatedManifestString> result;
    result.reserve(values.size());
    for (auto& value : values) {
        result.push_back(located(std::move(value)));
    }
    return result;
}

[[nodiscard]] JobPlan plan(
    std::vector<TargetSpec> specifications,
    std::string erase_partition = "metadata") {
    std::vector<ManifestTarget> targets;
    targets.reserve(specifications.size());
    for (auto& specification : specifications) {
        targets.push_back(ManifestTarget{
            .location = kLocation,
            .name = located(std::move(specification.name)),
            .selector = ManifestSelector{
                .location = kLocation,
                .serials = located_values(std::move(specification.serials)),
                .usb_paths =
                    located_values(std::move(specification.usb_paths)),
            },
            .expected_product = located(std::move(specification.product)),
            .steps = {ManifestStep{
                .location = kLocation,
                .payload = ManifestEraseStep{
                    located(erase_partition)},
            }},
        });
    }
    FlashJobManifest manifest{
        .location = kLocation,
        .api_version = located("kairosboot.io/v1"),
        .kind = located("FlashJob"),
        .source_sha256 = {},
        .artifacts = {ManifestArtifact{
            .location = kLocation,
            .id = located("unused"),
            .path = located("images/unused.img"),
            .sha256 = located(std::string(64U, '1')),
        }},
        .targets = std::move(targets),
        .policy = ManifestPolicy{},
    };
    auto result = make_job_plan(std::move(manifest));
    CHECK(result.has_value());
    return std::move(*result);
}

[[nodiscard]] UsbDeviceInfo device(std::string serial,
                                   std::string physical_path,
                                   const std::uint8_t port,
                                   const std::uint8_t address) {
    UsbDeviceInfo result{
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .bus_number = 1U,
        .device_address = address,
        .backend_session_id = address,
        .configuration_value = 1U,
        .port_path = {port},
        .serial_utf8 = std::move(serial),
        .interface_number = 0U,
        .alternate_setting = 0U,
        .interface_class = 0xFFU,
        .interface_subclass = 0x42U,
        .interface_protocol = 0x03U,
        .bulk_out_endpoint = 0x01U,
        .bulk_out_max_packet_size = 512U,
        .bulk_in_endpoint = 0x81U,
        .bulk_in_max_packet_size = 512U,
        .linux_topology = std::nullopt,
        .linux_topology_error = std::nullopt,
        .windows_topology = std::nullopt,
        .windows_topology_error = std::nullopt,
        .macos_topology = std::nullopt,
        .macos_topology_error = std::nullopt,
    };
    result.linux_topology = LinuxUsbTopology{
        .physical_port_path = std::move(physical_path),
        .root_controller_id = "linux-sysfs:/devices/pci0000:00/controller0",
        .hub_port_chain = result.port_path,
        .vendor_id = result.vendor_id,
        .product_id = result.product_id,
        .bus_number = result.bus_number,
        .device_address = result.device_address,
        .serial_utf8 = result.serial_utf8.empty()
            ? std::nullopt
            : std::optional<std::string>{result.serial_utf8},
        .product_utf8 = std::string{"misleading USB descriptor product"},
        .sysfs_device_path = "/sys/bus/usb/devices/1-" +
            std::to_string(port),
    };
    return result;
}

[[nodiscard]] UsbDeviceInfo windows_device(std::string serial,
                                           std::string physical_path,
                                           const std::uint8_t port,
                                           const std::uint8_t address) {
    auto result = device(
        std::move(serial), std::move(physical_path), port, address);
    const auto path = result.linux_topology->physical_port_path;
    result.linux_topology.reset();
    result.windows_topology = WindowsUsbTopology{
        .physical_port_path = path,
        .root_controller_id = "windows-pnp:PCI\\VEN_1234&DEV_5678",
        .hub_port_chain = result.port_path,
        .vendor_id = result.vendor_id,
        .product_id = result.product_id,
        .bus_number = result.bus_number,
        .device_address = result.device_address,
        .serial_utf8 = result.serial_utf8,
        .interface_fingerprint = WindowsUsbInterfaceFingerprint{
            .interface_number = result.interface_number,
            .alternate_setting = result.alternate_setting,
            .interface_class = result.interface_class,
            .interface_subclass = result.interface_subclass,
            .interface_protocol = result.interface_protocol,
        },
        .device_instance_id_utf8 = "USB\\VID_18D1&PID_4EE0\\" +
            result.serial_utf8,
        .hub_instance_ids_utf8 = {"USB\\ROOT_HUB30\\0"},
        .location_path_utf8 = "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(" +
            std::to_string(port) + ")",
    };
    return result;
}

[[nodiscard]] UsbDeviceInfo macos_device(std::string serial,
                                         std::string physical_path,
                                         const std::uint8_t port,
                                         const std::uint8_t address) {
    auto result = device(
        std::move(serial), std::move(physical_path), port, address);
    const auto path = result.linux_topology->physical_port_path;
    result.linux_topology.reset();
    result.backend_session_id = 10'000U + address;
    result.macos_topology = MacUsbTopology{
        .physical_port_path = path,
        .root_controller_id = "macos-iokit:/AppleUSBXHCI@14000000",
        .hub_port_chain = result.port_path,
        .registry_entry_id = 20'000U + address,
        .session_id = result.backend_session_id,
        .interface_registry_entry_id = 30'000U + address,
        .location_id = 0x01000000U |
            (static_cast<std::uint32_t>(port) << 16U),
        .vendor_id = result.vendor_id,
        .product_id = result.product_id,
        .bus_number = result.bus_number,
        .device_address = result.device_address,
        .interface_fingerprint = MacUsbInterfaceFingerprint{
            .configuration_value = result.configuration_value,
            .interface_number = result.interface_number,
            .alternate_setting = result.alternate_setting,
            .interface_class = result.interface_class,
            .interface_subclass = result.interface_subclass,
            .interface_protocol = result.interface_protocol,
        },
        .serial_utf8 = result.serial_utf8,
        .product_utf8 = std::string{"misleading USB descriptor product"},
        .registry_path = "IOService:/Mac/device/" + std::to_string(port),
        .interface_registry_path =
            "IOService:/Mac/device/interface/" + std::to_string(port),
        .root_controller_registry_path = "IOService:/Mac/controller",
    };
    return result;
}

[[nodiscard]] DevicePreflightUsbIdentity identity(const UsbDeviceInfo& value) {
    std::variant<LinuxUsbTopology, WindowsUsbTopology, MacUsbTopology>
        platform_attestation;
    std::string physical_port_path;
    std::string root_controller_id;
    std::vector<std::uint8_t> hub_port_chain;
    if (value.linux_topology.has_value()) {
        platform_attestation = *value.linux_topology;
        physical_port_path = value.linux_topology->physical_port_path;
        root_controller_id = value.linux_topology->root_controller_id;
        hub_port_chain = value.linux_topology->hub_port_chain;
    } else if (value.windows_topology.has_value()) {
        platform_attestation = *value.windows_topology;
        physical_port_path = value.windows_topology->physical_port_path;
        root_controller_id = value.windows_topology->root_controller_id;
        hub_port_chain = value.windows_topology->hub_port_chain;
    } else {
        CHECK(value.macos_topology.has_value());
        platform_attestation = *value.macos_topology;
        physical_port_path = value.macos_topology->physical_port_path;
        root_controller_id = value.macos_topology->root_controller_id;
        hub_port_chain = value.macos_topology->hub_port_chain;
    }
    return {
        .physical_port_path = std::move(physical_port_path),
        .root_controller_id = std::move(root_controller_id),
        .hub_port_chain = std::move(hub_port_chain),
        .bus_number = value.bus_number,
        .device_address = value.device_address,
        .backend_session_id = value.backend_session_id,
        .serial = value.serial_utf8.empty()
            ? std::nullopt
            : std::optional<std::string>{value.serial_utf8},
        .usb_fingerprint = DevicePreflightUsbFingerprint{
            .vendor_id = value.vendor_id,
            .product_id = value.product_id,
            .configuration_value = value.configuration_value,
            .interface_number = value.interface_number,
            .alternate_setting = value.alternate_setting,
            .interface_class = value.interface_class,
            .interface_subclass = value.interface_subclass,
            .interface_protocol = value.interface_protocol,
            .bulk_out_endpoint = value.bulk_out_endpoint,
            .bulk_out_max_packet_size = value.bulk_out_max_packet_size,
            .bulk_in_endpoint = value.bulk_in_endpoint,
            .bulk_in_max_packet_size = value.bulk_in_max_packet_size,
        },
        .platform_attestation = std::move(platform_attestation),
    };
}

struct TransportState final {
    std::atomic<bool> closed{};
};

class TrackingTransport final : public ITransportSession {
public:
    explicit TrackingTransport(std::shared_ptr<TransportState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] TransferResult write(
        std::span<const std::byte>, std::chrono::milliseconds) override {
        return failure();
    }

    [[nodiscard]] TransferResult read(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return failure();
    }

    [[nodiscard]] TransferResult read_data(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return failure();
    }

    void request_cancel() noexcept override {}

    void close() noexcept override {
        state_->closed.store(true, std::memory_order_release);
    }

private:
    [[nodiscard]] static TransferResult failure() {
        return {
            .status = TransportStatus::IoError,
            .certainty = TransferCertainty::NotTransferred,
        };
    }

    std::shared_ptr<TransportState> state_;
};

enum class OpenMutation : std::uint8_t {
    None,
    PhysicalPath,
    Serial,
    Fingerprint,
    DeviceAddress,
    BackendSession,
    PlatformGeneration,
};

class RecordingOpener final : public IDevicePreflightSessionOpener {
public:
    std::size_t calls{};
    std::optional<std::size_t> throw_bad_alloc_on_call;
    std::optional<std::size_t> fail_on_call;
    std::optional<DevicePreflightOpenError> failure;
    OpenMutation mutation{OpenMutation::None};
    std::vector<std::string> order;
    std::vector<std::shared_ptr<TransportState>> states;

    [[nodiscard]] std::expected<OpenedDevicePreflightSession,
                                DevicePreflightOpenError>
    open(const UsbDeviceInfo& value,
         DevicePreflightTimePoint,
         std::stop_token) override {
        ++calls;
        if (throw_bad_alloc_on_call == calls) {
            throw std::bad_alloc{};
        }
        if (failure.has_value() &&
            (!fail_on_call.has_value() || *fail_on_call == calls)) {
            return std::unexpected(*failure);
        }
        auto opened_identity = identity(value);
        order.push_back(opened_identity.physical_port_path);
        switch (mutation) {
            case OpenMutation::None:
                break;
            case OpenMutation::PhysicalPath:
                opened_identity.physical_port_path.append(".changed");
                break;
            case OpenMutation::Serial:
                opened_identity.serial = "CHANGED";
                break;
            case OpenMutation::Fingerprint:
                ++opened_identity.usb_fingerprint.product_id;
                break;
            case OpenMutation::DeviceAddress:
                ++opened_identity.device_address;
                break;
            case OpenMutation::BackendSession:
                ++opened_identity.backend_session_id;
                break;
            case OpenMutation::PlatformGeneration:
                std::visit(
                    [](auto& topology) {
                        using Topology = std::remove_cvref_t<decltype(topology)>;
                        if constexpr (std::is_same_v<Topology,
                                                     LinuxUsbTopology>) {
                            topology.sysfs_device_path.append(".changed");
                        } else if constexpr (std::is_same_v<
                                                 Topology,
                                                 WindowsUsbTopology>) {
                            topology.device_instance_id_utf8.append(".changed");
                        } else {
                            ++topology.session_id;
                        }
                    },
                    opened_identity.platform_attestation);
                break;
        }
        auto state = std::make_shared<TransportState>();
        states.push_back(state);
        return OpenedDevicePreflightSession{
            .verified_usb_identity = std::move(opened_identity),
            .session = std::make_unique<FastbootSession>(
                std::make_unique<TrackingTransport>(std::move(state))),
        };
    }
};

class SequenceProbe final : public IDevicePreflightProbe {
public:
    SequenceProbe() = default;
    explicit SequenceProbe(std::vector<DevicePreflightProbeResult> values)
        : results(std::move(values)) {}

    std::size_t calls{};
    std::vector<DevicePreflightProbeResult> results;
    std::optional<std::size_t> fail_on_call;
    std::optional<DevicePreflightProbeError> failure;

    [[nodiscard]] std::expected<DevicePreflightProbeResult,
                                DevicePreflightProbeError>
    probe(FastbootSession&,
          DevicePreflightTimePoint,
          std::stop_token) override {
        ++calls;
        if (failure.has_value() &&
            (!fail_on_call.has_value() || *fail_on_call == calls)) {
            return std::unexpected(*failure);
        }
        CHECK(calls <= results.size());
        return results[calls - 1U];
    }
};

class StopAtCallProbe final : public IDevicePreflightProbe {
public:
    StopAtCallProbe(std::stop_source& cancellation,
                    const std::size_t stop_on_call) noexcept
        : cancellation_(&cancellation), stop_on_call_(stop_on_call) {}

    std::size_t calls{};

    [[nodiscard]] std::expected<DevicePreflightProbeResult,
                                DevicePreflightProbeError>
    probe(FastbootSession&,
          DevicePreflightTimePoint,
          std::stop_token) override {
        ++calls;
        if (calls == stop_on_call_) {
            cancellation_->request_stop();
        }
        return DevicePreflightProbeResult{
            .product = "product-a",
            .mode = FastbootUsbMode::Bootloader,
            .product_query_completed = true,
            .mode_query_completed = true,
        };
    }

private:
    std::stop_source* cancellation_{};
    std::size_t stop_on_call_{};
};

class DeadlineAtCallProbe final : public IDevicePreflightProbe {
public:
    explicit DeadlineAtCallProbe(const std::size_t expire_on_call) noexcept
        : expire_on_call_(expire_on_call) {}

    std::size_t calls{};

    [[nodiscard]] std::expected<DevicePreflightProbeResult,
                                DevicePreflightProbeError>
    probe(FastbootSession&,
          const DevicePreflightTimePoint probe_deadline,
          std::stop_token) override {
        ++calls;
        if (calls == expire_on_call_) {
            while (kairosboot::fleet::DevicePreflightClock::now() <
                   probe_deadline) {
                std::this_thread::yield();
            }
        }
        return DevicePreflightProbeResult{
            .product = "product-a",
            .mode = FastbootUsbMode::Bootloader,
            .product_query_completed = true,
            .mode_query_completed = true,
        };
    }

private:
    std::size_t expire_on_call_{};
};

[[nodiscard]] DevicePreflightProbeResult live(
    std::string product,
    const FastbootUsbMode mode = FastbootUsbMode::Bootloader) {
    return {
        .product = std::move(product),
        .mode = mode,
        .product_query_completed = true,
        .mode_query_completed = true,
    };
}

[[nodiscard]] auto deadline() {
    return kairosboot::fleet::DevicePreflightClock::now() + kDeadlineBudget;
}

void selectors_are_exact_deduplicated_and_ignore_unrelated_devices() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {"usb:1-2"},
        .product = "product-a",
    }});
    std::vector snapshot{
        device("UNRELATED", "usb:1-9", 9U, 9U),
        device("SERIAL-A", "usb:1-2", 2U, 2U),
    };
    RecordingOpener opener;
    SequenceProbe probe{{live("product-a", FastbootUsbMode::Fastbootd)}};

    auto result = preflight_fleet_devices(
        job, snapshot, opener, probe, deadline());
    CHECK(result.has_value());
    CHECK(result->plan_sha256() == job.sha256());
    CHECK(result->devices().size() == 1U);
    CHECK(opener.calls == 1U);
    CHECK(probe.calls == 1U);
    const auto& prepared = result->devices().front();
    CHECK(prepared.target_index() == 0U);
    CHECK(prepared.observed_mode() == FastbootUsbMode::Fastbootd);
    CHECK(prepared.usb_identity().physical_port_path == "usb:1-2");
    CHECK(prepared.usb_identity().root_controller_id ==
          "linux-sysfs:/devices/pci0000:00/controller0");
    CHECK(prepared.usb_identity().hub_port_chain ==
          std::vector<std::uint8_t>{2U});
    CHECK(prepared.usb_identity().device_address == 2U);
    CHECK(prepared.usb_identity().backend_session_id == 2U);
}

void unmatched_invalid_and_duplicate_devices_are_ignored() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    auto broken = device("BROKEN", "usb:1-9", 9U, 9U);
    broken.linux_topology.reset();
    broken.linux_topology_error = kairosboot::transport::LinuxUsbTopologyError{
        .kind = kairosboot::transport::LinuxUsbTopologyErrorKind::IoError,
        .stage = kairosboot::transport::LinuxUsbTopologyStage::Lookup,
        .native_code = 5,
        .path = "/sys/broken",
        .message = "unrelated topology error",
    };
    std::vector snapshot{
        device("SERIAL-A", "usb:1-2", 2U, 2U),
        device("DUPLICATE", "usb:1-8", 8U, 8U),
        device("DUPLICATE", "usb:1-8", 8U, 10U),
        std::move(broken),
    };
    RecordingOpener opener;
    SequenceProbe probe{{live("product-a")}};

    auto result = preflight_fleet_devices(
        job, snapshot, opener, probe, deadline());
    CHECK(result.has_value());
    CHECK(result->devices().size() == 1U);
    CHECK(opener.calls == 1U);
    CHECK(probe.calls == 1U);
}

void unmatched_snapshot_size_does_not_consume_candidate_budget() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    const auto unrelated = device("UNRELATED", "usb:1-9", 9U, 9U);
    std::vector<UsbDeviceInfo> snapshot;
    snapshot.reserve(4'098U);
    for (std::size_t index = 0U; index < 4'097U; ++index) {
        snapshot.push_back(unrelated);
    }
    snapshot.push_back(device("SERIAL-A", "usb:1-2", 2U, 2U));
    RecordingOpener opener;
    SequenceProbe probe{{live("product-a")}};

    auto result = preflight_fleet_devices(
        job, snapshot, opener, probe, deadline());
    CHECK(result.has_value());
    CHECK(result->devices().size() == 1U);
    CHECK(opener.calls == 1U);
    CHECK(probe.calls == 1U);
}

void matching_candidate_budget_is_enforced() {
    std::vector<TargetSpec> targets;
    std::vector<UsbDeviceInfo> snapshot;
    targets.reserve(17U);
    snapshot.reserve(4'097U);
    std::size_t next_device = 0U;
    for (std::size_t target_index = 0U;
         target_index < 17U;
         ++target_index) {
        TargetSpec target{
            .name = "target-" + std::to_string(target_index),
            .serials = {},
            .usb_paths = {},
            .product = "product-a",
        };
        const auto target_count =
            std::min<std::size_t>(256U, 4'097U - next_device);
        target.serials.reserve(target_count);
        for (std::size_t offset = 0U;
             offset < target_count;
             ++offset, ++next_device) {
            auto serial = "SERIAL-" + std::to_string(next_device);
            target.serials.push_back(serial);
            const auto first_port = static_cast<std::uint8_t>(
                next_device / 255U + 1U);
            const auto second_port = static_cast<std::uint8_t>(
                next_device % 255U + 1U);
            const auto physical_path = "usb:1-" +
                std::to_string(first_port) + "." +
                std::to_string(second_port);
            auto value = device(
                std::move(serial),
                physical_path,
                second_port,
                static_cast<std::uint8_t>(next_device % 127U + 1U));
            value.backend_session_id = next_device + 1U;
            value.port_path = {first_port, second_port};
            value.linux_topology->hub_port_chain = value.port_path;
            value.linux_topology->sysfs_device_path =
                "/sys/bus/usb/devices/1-" +
                std::to_string(first_port) + "." +
                std::to_string(second_port);
            snapshot.push_back(std::move(value));
        }
        targets.push_back(std::move(target));
    }
    CHECK(snapshot.size() == 4'097U);
    auto job = plan(std::move(targets));
    RecordingOpener opener;
    SequenceProbe probe;

    auto result = preflight_fleet_devices(
        job,
        snapshot,
        opener,
        probe,
        kairosboot::fleet::DevicePreflightClock::now() +
            std::chrono::seconds{30});
    CHECK(!result);
    CHECK(result.error().kind ==
          DevicePreflightErrorKind::SnapshotLimitExceeded);
    CHECK(opener.calls == 0U);
    CHECK(probe.calls == 0U);
}

void duplicate_physical_and_serial_identities_fail_before_open() {
    auto serial_job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    {
        auto path_job = plan({TargetSpec{
            .name = "target-a",
            .serials = {},
            .usb_paths = {"usb:1-2"},
            .product = "product-a",
        }});
        std::vector snapshot{
            device("SERIAL-A", "usb:1-2", 2U, 2U),
            device("SERIAL-B", "usb:1-2", 2U, 3U),
        };
        RecordingOpener opener;
        SequenceProbe probe;
        auto result = preflight_fleet_devices(
            path_job, snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(result.error().kind ==
              DevicePreflightErrorKind::DuplicatePhysicalPath);
        CHECK(opener.calls == 0U);
    }
    {
        std::vector snapshot{
            device("SERIAL-A", "usb:1-2", 2U, 2U),
            device("SERIAL-A", "usb:1-3", 3U, 3U),
        };
        RecordingOpener opener;
        SequenceProbe probe;
        auto result = preflight_fleet_devices(
            serial_job, snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(result.error().kind == DevicePreflightErrorKind::DuplicateSerial);
        CHECK(opener.calls == 0U);
    }
}

void cross_target_multi_namespace_match_is_rejected() {
    auto job = plan({
        TargetSpec{
            .name = "by-serial",
            .serials = {"SERIAL-A"},
            .usb_paths = {},
            .product = "product-a",
        },
        TargetSpec{
            .name = "by-port",
            .serials = {},
            .usb_paths = {"usb:1-2"},
            .product = "product-a",
        },
    });
    std::vector snapshot{device("SERIAL-A", "usb:1-2", 2U, 2U)};
    RecordingOpener opener;
    SequenceProbe probe;
    auto result = preflight_fleet_devices(
        job, snapshot, opener, probe, deadline());
    CHECK(!result);
    CHECK(result.error().kind ==
          DevicePreflightErrorKind::DeviceMatchesMultipleTargets);
    CHECK(opener.calls == 0U);
}

void missing_selector_and_unreliable_topology_fail_closed() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"MISSING"},
        .usb_paths = {},
        .product = "product-a",
    }});
    std::vector reliable{device("SERIAL-A", "usb:1-2", 2U, 2U)};
    RecordingOpener opener;
    SequenceProbe probe;
    auto missing = preflight_fleet_devices(
        job, reliable, opener, probe, deadline());
    CHECK(!missing);
    CHECK(missing.error().kind ==
          DevicePreflightErrorKind::MissingSelectorDevice);
    CHECK(opener.calls == 0U);

    auto topology_job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    reliable[0].linux_topology->root_controller_id.clear();
    auto unreliable = preflight_fleet_devices(
        topology_job, reliable, opener, probe, deadline());
    CHECK(!unreliable);
    CHECK(unreliable.error().kind ==
          DevicePreflightErrorKind::UnreliableTopology);
    CHECK(opener.calls == 0U);
}

void zero_bus_is_accepted_only_for_macos_topology() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {},
        .usb_paths = {"usb:0-2"},
        .product = "product-a",
    }});
    auto serial_job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});

    const auto macos_zero_bus = [] {
        auto result = macos_device("SERIAL-A", "usb:0-2", 2U, 2U);
        result.bus_number = 0U;
        result.macos_topology->bus_number = 0U;
        result.macos_topology->location_id = 2U << 16U;
        return result;
    };
    const auto expect_unreliable = [&serial_job](UsbDeviceInfo candidate) {
        std::vector snapshot{std::move(candidate)};
        RecordingOpener opener;
        SequenceProbe probe;
        auto result = preflight_fleet_devices(
            serial_job, snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(result.error().kind ==
              DevicePreflightErrorKind::UnreliableTopology);
        CHECK(opener.calls == 0U);
    };

    std::vector macos_snapshot{macos_zero_bus()};
    RecordingOpener macos_opener;
    SequenceProbe macos_probe{{live("product-a")}};
    auto macos_result = preflight_fleet_devices(
        job, macos_snapshot, macos_opener, macos_probe, deadline());
    CHECK(macos_result.has_value());
    CHECK(macos_opener.calls == 1U);

    auto linux = device("SERIAL-A", "usb:0-2", 2U, 2U);
    linux.bus_number = 0U;
    linux.linux_topology->bus_number = 0U;
    expect_unreliable(std::move(linux));

    auto windows = windows_device("SERIAL-A", "usb:0-2", 2U, 2U);
    windows.bus_number = 0U;
    windows.windows_topology->bus_number = 0U;
    expect_unreliable(std::move(windows));

    auto mixed = macos_zero_bus();
    mixed.linux_topology = device(
        "SERIAL-A", "usb:0-2", 2U, 2U).linux_topology;
    mixed.linux_topology->bus_number = 0U;
    mixed.linux_topology->physical_port_path = "usb:0-2";
    expect_unreliable(std::move(mixed));

    auto missing = macos_zero_bus();
    missing.macos_topology.reset();
    expect_unreliable(std::move(missing));

    auto macos_error = macos_zero_bus();
    macos_error.macos_topology_error =
        kairosboot::transport::MacUsbTopologyError{};
    expect_unreliable(std::move(macos_error));

    auto linux_error = macos_zero_bus();
    linux_error.linux_topology_error =
        kairosboot::transport::LinuxUsbTopologyError{};
    expect_unreliable(std::move(linux_error));

    auto windows_error = macos_zero_bus();
    windows_error.windows_topology_error =
        kairosboot::transport::WindowsUsbTopologyError{};
    expect_unreliable(std::move(windows_error));
}

void product_mismatch_withholds_the_entire_batch_and_reports_outcomes() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A", "SERIAL-B"},
        .usb_paths = {},
        .product = "product-a",
    }});
    std::vector snapshot{
        device("SERIAL-B", "usb:1-3", 3U, 3U),
        device("SERIAL-A", "usb:1-2", 2U, 2U),
    };
    RecordingOpener opener;
    SequenceProbe probe{{live("product-a"), live("wrong-product")}};
    auto result = preflight_fleet_devices(
        job, snapshot, opener, probe, deadline());
    CHECK(!result);
    CHECK(result.error().kind == DevicePreflightErrorKind::ProductMismatch);
    CHECK(result.error().outcomes.size() == 2U);
    CHECK(result.error().outcomes[0].code ==
          DevicePreflightOutcomeCode::Ready);
    CHECK(result.error().outcomes[1].code ==
          DevicePreflightOutcomeCode::ProductMismatch);
    CHECK(opener.order == std::vector<std::string>({"usb:1-2", "usb:1-3"}));
    CHECK(opener.states.size() == 2U);
    CHECK(std::ranges::all_of(opener.states, [](const auto& state) {
        return state->closed.load(std::memory_order_acquire);
    }));
}

struct ThirtyTwoDeviceInput final {
    JobPlan job;
    std::vector<UsbDeviceInfo> snapshot;
};

[[nodiscard]] ThirtyTwoDeviceInput thirty_two_device_input() {
    TargetSpec target{
        .name = "target-a",
        .serials = {},
        .usb_paths = {},
        .product = "product-a",
    };
    std::vector<UsbDeviceInfo> snapshot;
    snapshot.reserve(32U);
    target.serials.reserve(32U);
    for (std::uint8_t index = 1U; index <= 32U; ++index) {
        auto serial = "SERIAL-" + std::to_string(index);
        target.serials.push_back(serial);
        snapshot.push_back(device(std::move(serial),
                                  "usb:1-" + std::to_string(index),
                                  index,
                                  index));
    }
    return {
        .job = plan({std::move(target)}),
        .snapshot = std::move(snapshot),
    };
}

void thirty_two_device_batch_completes_the_live_barrier_before_failure() {
    auto input = thirty_two_device_input();
    std::vector<DevicePreflightProbeResult> probe_results;
    probe_results.reserve(32U);
    for (std::uint8_t index = 1U; index <= 32U; ++index) {
        probe_results.push_back(live(index == 32U ? "wrong-product"
                                                  : "product-a"));
    }
    RecordingOpener opener;
    SequenceProbe probe{std::move(probe_results)};

    auto result = preflight_fleet_devices(
        input.job, input.snapshot, opener, probe, deadline());
    CHECK(!result);
    CHECK(result.error().kind == DevicePreflightErrorKind::ProductMismatch);
    CHECK(result.error().outcomes.size() == 32U);
    CHECK(opener.calls == 32U);
    CHECK(probe.calls == 32U);
    CHECK(opener.states.size() == 32U);
    CHECK(std::ranges::all_of(opener.states, [](const auto& state) {
        return state->closed.load(std::memory_order_acquire);
    }));
}

void thirty_two_device_terminal_failures_never_publish_a_gate() {
    {
        auto input = thirty_two_device_input();
        RecordingOpener opener;
        opener.fail_on_call = 32U;
        opener.failure = DevicePreflightOpenError{
            .code = DevicePreflightOpenErrorCode::Busy,
            .message = "last device open failed",
            .native_code = 32,
            .outbound_certainty = TransferCertainty::NotTransferred,
        };
        SequenceProbe probe;
        probe.results.assign(31U, live("product-a"));
        auto result = preflight_fleet_devices(
            input.job, input.snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(result.error().kind == DevicePreflightErrorKind::OpenFailed);
        CHECK(result.error().open_error->code ==
              DevicePreflightOpenErrorCode::Busy);
        CHECK(opener.calls == 32U);
        CHECK(probe.calls == 31U);
        CHECK(opener.states.size() == 31U);
        CHECK(std::ranges::all_of(opener.states, [](const auto& state) {
            return state->closed.load(std::memory_order_acquire);
        }));
    }
    {
        auto input = thirty_two_device_input();
        RecordingOpener opener;
        SequenceProbe probe;
        probe.results.assign(31U, live("product-a"));
        probe.fail_on_call = 32U;
        probe.failure = DevicePreflightProbeError{
            .code = DevicePreflightProbeErrorCode::ProtocolFailure,
            .message = "last device probe failed",
            .native_code = 64,
            .outbound_certainty = TransferCertainty::FullyTransferred,
        };
        auto result = preflight_fleet_devices(
            input.job, input.snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(result.error().kind == DevicePreflightErrorKind::ProbeFailed);
        CHECK(result.error().probe_error->code ==
              DevicePreflightProbeErrorCode::ProtocolFailure);
        CHECK(opener.calls == 32U);
        CHECK(probe.calls == 32U);
        CHECK(opener.states.size() == 32U);
        CHECK(std::ranges::all_of(opener.states, [](const auto& state) {
            return state->closed.load(std::memory_order_acquire);
        }));
    }
}

void barrier_handoff_stop_and_deadline_never_publish_a_gate() {
    {
        auto input = thirty_two_device_input();
        RecordingOpener opener;
        std::stop_source cancellation;
        StopAtCallProbe probe{cancellation, 32U};
        auto result = preflight_fleet_devices(
            input.job,
            input.snapshot,
            opener,
            probe,
            deadline(),
            cancellation.get_token());
        CHECK(!result);
        CHECK(result.error().kind == DevicePreflightErrorKind::Cancelled);
        CHECK(opener.calls == 32U);
        CHECK(probe.calls == 32U);
        CHECK(std::ranges::all_of(opener.states, [](const auto& state) {
            return state->closed.load(std::memory_order_acquire);
        }));
    }
    {
        auto input = thirty_two_device_input();
        RecordingOpener opener;
        DeadlineAtCallProbe probe{32U};
        const auto handoff_deadline =
            kairosboot::fleet::DevicePreflightClock::now() +
            std::chrono::milliseconds{250};
        auto result = preflight_fleet_devices(
            input.job,
            input.snapshot,
            opener,
            probe,
            handoff_deadline);
        CHECK(!result);
        CHECK(result.error().kind ==
              DevicePreflightErrorKind::DeadlineExceeded);
        CHECK(opener.calls == 32U);
        CHECK(probe.calls == 32U);
        CHECK(std::ranges::all_of(opener.states, [](const auto& state) {
            return state->closed.load(std::memory_order_acquire);
        }));
    }
}

void open_race_and_probe_contract_violations_never_publish_a_gate() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    std::vector snapshot{device("SERIAL-A", "usb:1-2", 2U, 2U)};
    for (const auto mutation : {OpenMutation::PhysicalPath,
                                OpenMutation::Serial,
                                OpenMutation::Fingerprint,
                                OpenMutation::DeviceAddress,
                                OpenMutation::BackendSession,
                                OpenMutation::PlatformGeneration}) {
        RecordingOpener opener;
        opener.mutation = mutation;
        SequenceProbe probe{{live("product-a")}};
        auto result = preflight_fleet_devices(
            job, snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(result.error().kind ==
              DevicePreflightErrorKind::DeviceChangedDuringOpen);
        CHECK(probe.calls == 0U);
    }

    RecordingOpener opener;
    auto unproven = live("product-a");
    unproven.product_query_completed = false;
    SequenceProbe probe{{std::move(unproven)}};
    auto result = preflight_fleet_devices(
        job, snapshot, opener, probe, deadline());
    CHECK(!result);
    CHECK(result.error().kind ==
          DevicePreflightErrorKind::ProbeContractViolation);
}

void all_platform_generation_attestations_are_retained_and_compared() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    std::vector platform_devices{
        device("SERIAL-A", "usb:1-2", 2U, 2U),
        windows_device("SERIAL-A", "usb:1-2", 2U, 2U),
        macos_device("SERIAL-A", "usb:1-2", 2U, 2U),
    };
    for (const auto& platform_device : platform_devices) {
        std::vector snapshot{platform_device};
        RecordingOpener valid_opener;
        SequenceProbe valid_probe{{live("product-a")}};
        auto valid = preflight_fleet_devices(
            job, snapshot, valid_opener, valid_probe, deadline());
        CHECK(valid.has_value());

        RecordingOpener stale_opener;
        stale_opener.mutation = OpenMutation::PlatformGeneration;
        SequenceProbe stale_probe{{live("product-a")}};
        auto stale = preflight_fleet_devices(
            job, snapshot, stale_opener, stale_probe, deadline());
        CHECK(!stale);
        CHECK(stale.error().kind ==
              DevicePreflightErrorKind::DeviceChangedDuringOpen);
        CHECK(stale_probe.calls == 0U);
    }
}

void cancellation_deadline_and_exception_preserve_the_barrier() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A", "SERIAL-B"},
        .usb_paths = {},
        .product = "product-a",
    }});
    std::vector snapshot{
        device("SERIAL-A", "usb:1-2", 2U, 2U),
        device("SERIAL-B", "usb:1-3", 3U, 3U),
    };
    {
        RecordingOpener opener;
        SequenceProbe probe;
        std::stop_source stop;
        stop.request_stop();
        auto result = preflight_fleet_devices(
            job, snapshot, opener, probe, deadline(), stop.get_token());
        CHECK(!result);
        CHECK(result.error().kind == DevicePreflightErrorKind::Cancelled);
        CHECK(opener.calls == 0U);
    }
    {
        RecordingOpener opener;
        SequenceProbe probe;
        auto result = preflight_fleet_devices(
            job,
            snapshot,
            opener,
            probe,
            kairosboot::fleet::DevicePreflightClock::now());
        CHECK(!result);
        CHECK(result.error().kind ==
              DevicePreflightErrorKind::DeadlineExceeded);
        CHECK(opener.calls == 0U);
    }
    {
        RecordingOpener opener;
        opener.throw_bad_alloc_on_call = 2U;
        SequenceProbe probe{{live("product-a"), live("product-a")}};
        auto result = preflight_fleet_devices(
            job, snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(result.error().kind ==
              DevicePreflightErrorKind::ResourceExhausted);
        CHECK(opener.states.size() == 1U);
        CHECK(opener.states[0]->closed.load(std::memory_order_acquire));
    }
}

void transport_and_probe_error_taxonomies_are_preserved() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    std::vector snapshot{device("SERIAL-A", "usb:1-2", 2U, 2U)};
    constexpr std::array open_codes{
        DevicePreflightOpenErrorCode::Cancelled,
        DevicePreflightOpenErrorCode::DeadlineExceeded,
        DevicePreflightOpenErrorCode::NotFound,
        DevicePreflightOpenErrorCode::Busy,
        DevicePreflightOpenErrorCode::PermissionDenied,
        DevicePreflightOpenErrorCode::DriverUnavailable,
        DevicePreflightOpenErrorCode::TransportFailure,
        DevicePreflightOpenErrorCode::ResourceExhausted,
        DevicePreflightOpenErrorCode::UnexpectedFailure,
    };
    for (const auto code : open_codes) {
        RecordingOpener opener;
        opener.failure = DevicePreflightOpenError{
            .code = code,
            .message = "typed open failure",
            .native_code = 123,
            .outbound_certainty = TransferCertainty::PartialOrUnknown,
        };
        SequenceProbe probe;
        auto result = preflight_fleet_devices(
            job, snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(result.error().open_error.has_value());
        CHECK(!result.error().probe_error.has_value());
        CHECK(result.error().open_error->code == code);
        CHECK(result.error().open_error->message == "typed open failure");
        CHECK(result.error().native_code == 123);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        const auto expected_kind =
            code == DevicePreflightOpenErrorCode::Cancelled
            ? DevicePreflightErrorKind::Cancelled
            : code == DevicePreflightOpenErrorCode::DeadlineExceeded
            ? DevicePreflightErrorKind::DeadlineExceeded
            : code == DevicePreflightOpenErrorCode::ResourceExhausted
            ? DevicePreflightErrorKind::ResourceExhausted
            : DevicePreflightErrorKind::OpenFailed;
        CHECK(result.error().kind == expected_kind);
    }

    constexpr std::array probe_codes{
        DevicePreflightProbeErrorCode::Cancelled,
        DevicePreflightProbeErrorCode::DeadlineExceeded,
        DevicePreflightProbeErrorCode::ProtocolFailure,
        DevicePreflightProbeErrorCode::DeviceRejected,
        DevicePreflightProbeErrorCode::InvalidResponse,
        DevicePreflightProbeErrorCode::ResourceExhausted,
        DevicePreflightProbeErrorCode::UnexpectedFailure,
    };
    for (const auto code : probe_codes) {
        RecordingOpener opener;
        SequenceProbe probe;
        probe.failure = DevicePreflightProbeError{
            .code = code,
            .message = "typed probe failure",
            .native_code = 456,
            .outbound_certainty = TransferCertainty::PartialOrUnknown,
        };
        auto result = preflight_fleet_devices(
            job, snapshot, opener, probe, deadline());
        CHECK(!result);
        CHECK(!result.error().open_error.has_value());
        CHECK(result.error().probe_error.has_value());
        CHECK(result.error().probe_error->code == code);
        CHECK(result.error().probe_error->message == "typed probe failure");
        CHECK(result.error().native_code == 456);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        const auto expected_kind =
            code == DevicePreflightProbeErrorCode::Cancelled
            ? DevicePreflightErrorKind::Cancelled
            : code == DevicePreflightProbeErrorCode::DeadlineExceeded
            ? DevicePreflightErrorKind::DeadlineExceeded
            : code == DevicePreflightProbeErrorCode::ResourceExhausted
            ? DevicePreflightErrorKind::ResourceExhausted
            : DevicePreflightErrorKind::ProbeFailed;
        CHECK(result.error().kind == expected_kind);
    }
}

void prepared_order_is_deterministic_across_snapshot_order() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-B", "SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    auto first_snapshot = std::vector{
        device("SERIAL-B", "usb:1-3", 3U, 3U),
        device("SERIAL-A", "usb:1-2", 2U, 2U),
    };
    auto second_snapshot = first_snapshot;
    std::ranges::reverse(second_snapshot);

    RecordingOpener first_opener;
    SequenceProbe first_probe{{live("product-a"), live("product-a")}};
    auto first = preflight_fleet_devices(
        job, first_snapshot, first_opener, first_probe, deadline());
    CHECK(first.has_value());

    RecordingOpener second_opener;
    SequenceProbe second_probe{{live("product-a"), live("product-a")}};
    auto second = preflight_fleet_devices(
        job, second_snapshot, second_opener, second_probe, deadline());
    CHECK(second.has_value());
    CHECK(first_opener.order ==
          std::vector<std::string>({"usb:1-2", "usb:1-3"}));
    CHECK(second_opener.order == first_opener.order);
}

void prepared_gate_is_plan_bound_and_sessions_are_consumed_once() {
    const std::vector specifications{TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }};
    auto plan_a = plan(specifications, "metadata");
    auto plan_b = plan(specifications, "userdata");
    CHECK(plan_a.sha256() != plan_b.sha256());
    std::vector snapshot{device("SERIAL-A", "usb:1-2", 2U, 2U)};
    RecordingOpener opener;
    SequenceProbe probe{{live("product-a")}};

    auto result = preflight_fleet_devices(
        plan_a, snapshot, opener, probe, deadline());
    CHECK(result.has_value());
    CHECK(result->plan_sha256() == plan_a.sha256());
    CHECK(result->plan_sha256() != plan_b.sha256());

    PreparedDeviceBatch moved_batch = std::move(*result);
    CHECK(result->devices().empty());
    CHECK(moved_batch.plan_sha256() == plan_a.sha256());
    auto wrong_plan =
        std::move(moved_batch).consume(plan_b.sha256());
    CHECK(!wrong_plan);
    CHECK(wrong_plan.error() ==
          PreparedDeviceBatchConsumptionError::PlanDigestMismatch);
    CHECK(moved_batch.devices().size() == 1U);

    auto consumption =
        std::move(moved_batch).consume(plan_a.sha256());
    CHECK(consumption.has_value());
    CHECK(moved_batch.devices().empty());
    auto consumed_again =
        std::move(moved_batch).consume(plan_a.sha256());
    CHECK(!consumed_again);
    CHECK(consumed_again.error() ==
          PreparedDeviceBatchConsumptionError::AlreadyConsumed);

    {
        PreparedDeviceBatchConsumption token = std::move(*consumption);
        CHECK(consumption->devices().empty());
        CHECK(token.plan_sha256() == plan_a.sha256());
        CHECK(token.devices().size() == 1U);
    }
    CHECK(opener.states.size() == 1U);
    CHECK(opener.states[0]->closed.load(std::memory_order_acquire));
}

class ScriptTransport final : public ITransportSession {
public:
    explicit ScriptTransport(std::vector<std::string> responses)
        : responses_(responses.begin(), responses.end()) {}

    [[nodiscard]] TransferResult write(
        const std::span<const std::byte> bytes,
        std::chrono::milliseconds) override {
        writes_.emplace_back(reinterpret_cast<const char*>(bytes.data()),
                             bytes.size());
        return {.transferred = bytes.size()};
    }

    [[nodiscard]] TransferResult read(
        const std::span<std::byte> destination,
        std::chrono::milliseconds) override {
        CHECK(!responses_.empty());
        const auto response = std::move(responses_.front());
        responses_.pop_front();
        CHECK(response.size() <= destination.size());
        std::memcpy(destination.data(), response.data(), response.size());
        return {.transferred = response.size()};
    }

    [[nodiscard]] TransferResult read_data(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return {
            .status = TransportStatus::IoError,
            .certainty = TransferCertainty::NotTransferred,
        };
    }

    void request_cancel() noexcept override { cancelled_.store(true); }
    void close() noexcept override { closed_ = true; }

    [[nodiscard]] const std::vector<std::string>& writes() const noexcept {
        return writes_;
    }

    [[nodiscard]] bool complete() const noexcept { return responses_.empty(); }

private:
    std::deque<std::string> responses_;
    std::vector<std::string> writes_;
    std::atomic<bool> cancelled_{};
    bool closed_{};
};

void built_in_probe_reads_product_and_mode_from_fastboot_wire() {
    auto transport = std::make_unique<ScriptTransport>(
        std::vector<std::string>{"OKAYwire-product", "OKAYyes"});
    auto* observer = transport.get();
    FastbootSession session(std::move(transport));
    FastbootDevicePreflightProbe probe;
    auto result = probe.probe(session, deadline(), {});
    CHECK(result.has_value());
    CHECK(result->product == "wire-product");
    CHECK(result->mode == FastbootUsbMode::Fastbootd);
    CHECK(result->product_query_completed);
    CHECK(result->mode_query_completed);
    CHECK(observer->writes() ==
          std::vector<std::string>({"getvar:product", "getvar:is-userspace"}));
    CHECK(observer->complete());
}

class FailingSecondProbeTransport final : public ITransportSession {
public:
    explicit FailingSecondProbeTransport(const bool throw_on_second)
        : throw_on_second_(throw_on_second) {}

    [[nodiscard]] TransferResult write(
        const std::span<const std::byte> bytes,
        std::chrono::milliseconds) override {
        ++writes_;
        if (writes_ == 2U) {
            if (throw_on_second_) {
                throw std::bad_alloc{};
            }
            return {
                .status = TransportStatus::Timeout,
                .transferred = 0,
                .certainty = TransferCertainty::NotTransferred,
            };
        }
        return {.transferred = bytes.size()};
    }

    [[nodiscard]] TransferResult read(
        const std::span<std::byte> destination,
        std::chrono::milliseconds) override {
        constexpr std::string_view response{"OKAYwire-product"};
        CHECK(response.size() <= destination.size());
        std::memcpy(destination.data(), response.data(), response.size());
        return {.transferred = response.size()};
    }

    [[nodiscard]] TransferResult read_data(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return {
            .status = TransportStatus::IoError,
            .certainty = TransferCertainty::NotTransferred,
        };
    }

    void request_cancel() noexcept override { cancelled_ = true; }
    void close() noexcept override { cancelled_ = true; }

private:
    bool throw_on_second_{};
    std::size_t writes_{};
    bool cancelled_{};
};

void built_in_probe_never_downgrades_prior_or_in_flight_bytes() {
    {
        FastbootSession session(
            std::make_unique<FailingSecondProbeTransport>(false));
        FastbootDevicePreflightProbe probe;
        auto result = probe.probe(session, deadline(), {});
        CHECK(!result.has_value());
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::FullyTransferred);
    }
    {
        FastbootSession session(
            std::make_unique<FailingSecondProbeTransport>(true));
        FastbootDevicePreflightProbe probe;
        auto result = probe.probe(session, deadline(), {});
        CHECK(!result.has_value());
        CHECK(result.error().code ==
              DevicePreflightProbeErrorCode::ResourceExhausted);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
    }
}

class BlockingProbeTransport final : public ITransportSession {
public:
    [[nodiscard]] TransferResult write(
        const std::span<const std::byte> bytes,
        std::chrono::milliseconds) override {
        return {.transferred = bytes.size()};
    }

    [[nodiscard]] TransferResult read(
        std::span<std::byte>, std::chrono::milliseconds) override {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return cancelled_; });
        return {
            .status = TransportStatus::Cancelled,
            .certainty = TransferCertainty::FullyTransferred,
        };
    }

    [[nodiscard]] TransferResult read_data(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return {
            .status = TransportStatus::IoError,
            .certainty = TransferCertainty::NotTransferred,
        };
    }

    void request_cancel() noexcept override {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        condition_.notify_all();
    }

    void close() noexcept override { request_cancel(); }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool cancelled_{};
};

void built_in_probe_interrupts_blocking_io_on_deadline_and_stop() {
    {
        FastbootSession session(std::make_unique<BlockingProbeTransport>());
        FastbootDevicePreflightProbe probe;
        auto result = probe.probe(
            session,
            kairosboot::fleet::DevicePreflightClock::now() +
                std::chrono::milliseconds{50},
            {});
        CHECK(!result);
        CHECK(result.error().code ==
              DevicePreflightProbeErrorCode::DeadlineExceeded);
    }
    {
        FastbootSession session(std::make_unique<BlockingProbeTransport>());
        FastbootDevicePreflightProbe probe;
        std::stop_source cancellation;
        std::jthread canceller([&cancellation] {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            cancellation.request_stop();
        });
        auto result = probe.probe(session, deadline(), cancellation.get_token());
        CHECK(!result);
        CHECK(result.error().code == DevicePreflightProbeErrorCode::Cancelled);
    }
}

}  // namespace

int main() {
    using Test = std::pair<std::string_view, void (*)()>;
    const std::array<Test, 21U> tests{
        Test{"selectors are exact, deduplicated, and ignore unrelated devices",
             &selectors_are_exact_deduplicated_and_ignore_unrelated_devices},
        Test{"unmatched invalid and duplicate devices are ignored",
             &unmatched_invalid_and_duplicate_devices_are_ignored},
        Test{"unmatched snapshot size does not consume candidate budget",
             &unmatched_snapshot_size_does_not_consume_candidate_budget},
        Test{"matching candidate budget is enforced",
             &matching_candidate_budget_is_enforced},
        Test{"duplicate physical and serial identities fail before open",
             &duplicate_physical_and_serial_identities_fail_before_open},
        Test{"cross-target multi-namespace match is rejected",
             &cross_target_multi_namespace_match_is_rejected},
        Test{"missing selector and unreliable topology fail closed",
             &missing_selector_and_unreliable_topology_fail_closed},
        Test{"zero bus is accepted only for macOS topology",
             &zero_bus_is_accepted_only_for_macos_topology},
        Test{"product mismatch withholds batch and reports outcomes",
             &product_mismatch_withholds_the_entire_batch_and_reports_outcomes},
        Test{"32-device batch completes live barrier before failure",
             &thirty_two_device_batch_completes_the_live_barrier_before_failure},
        Test{"32-device terminal failures never publish a gate",
             &thirty_two_device_terminal_failures_never_publish_a_gate},
        Test{"barrier handoff stop and deadline never publish a gate",
             &barrier_handoff_stop_and_deadline_never_publish_a_gate},
        Test{"open race and probe contract violations withhold the gate",
             &open_race_and_probe_contract_violations_never_publish_a_gate},
        Test{"all platform generation attestations are retained and compared",
             &all_platform_generation_attestations_are_retained_and_compared},
        Test{"cancellation, deadline, and exception preserve barrier",
             &cancellation_deadline_and_exception_preserve_the_barrier},
        Test{"transport and probe error taxonomies are preserved",
             &transport_and_probe_error_taxonomies_are_preserved},
        Test{"prepared order is deterministic across snapshot order",
             &prepared_order_is_deterministic_across_snapshot_order},
        Test{"prepared gate is plan bound and sessions are consumed once",
             &prepared_gate_is_plan_bound_and_sessions_are_consumed_once},
        Test{"built-in probe reads product and mode from Fastboot wire",
             &built_in_probe_reads_product_and_mode_from_fastboot_wire},
        Test{"built-in probe preserves aggregate outbound certainty",
             &built_in_probe_never_downgrades_prior_or_in_flight_bytes},
        Test{"built-in probe interrupts blocking I/O on deadline and stop",
             &built_in_probe_interrupts_blocking_io_on_deadline_and_stop},
    };

    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    }
    return 0;
}
