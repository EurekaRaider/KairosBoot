// SPDX-License-Identifier: MIT
#include "src/fleet/device_preflight.hpp"

#include "src/fastboot/primitive_service.hpp"
#include "src/transport/usb_fastboot.hpp"

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kairosboot::fleet {
namespace {

inline constexpr std::size_t kMaximumPreflightDevices = 4'096U;

struct NormalizedSnapshotDevice final {
    const transport::UsbDeviceInfo* source{};
    std::size_t snapshot_index{};
    DevicePreflightUsbIdentity identity;
};

struct SelectedDevice final {
    std::size_t target_index{};
    std::size_t device_index{};
};

[[nodiscard]] DevicePreflightError error(
    const DevicePreflightErrorKind kind,
    const DevicePreflightStage stage,
    std::string message,
    const std::optional<std::size_t> target_index = std::nullopt,
    const std::optional<std::size_t> snapshot_index = std::nullopt) {
    return {
        .kind = kind,
        .stage = stage,
        .message = std::move(message),
        .target_index = target_index,
        .snapshot_index = snapshot_index,
        .native_code = 0,
        .outbound_certainty = protocol::TransferCertainty::NotTransferred,
        .open_error = std::nullopt,
        .probe_error = std::nullopt,
        .outcomes = {},
    };
}

[[nodiscard]] std::optional<DevicePreflightError> interrupted(
    const DevicePreflightTimePoint deadline,
    const std::stop_token cancellation,
    const DevicePreflightStage stage) {
    if (cancellation.stop_requested()) {
        return error(DevicePreflightErrorKind::Cancelled,
                     stage,
                     "fleet device preflight was cancelled");
    }
    if (DevicePreflightClock::now() >= deadline) {
        return error(DevicePreflightErrorKind::DeadlineExceeded,
                     stage,
                     "fleet device preflight deadline expired");
    }
    return std::nullopt;
}

[[nodiscard]] bool valid_ports(
    const std::span<const std::uint8_t> ports) noexcept {
    return !ports.empty() &&
        ports.size() <= transport::kMaximumUsbTopologyDepth &&
        std::ranges::none_of(ports, [](const std::uint8_t port) {
            return port == 0U;
        });
}

[[nodiscard]] bool canonical_physical_path_matches(
    const std::string_view value,
    const std::uint8_t bus_number,
    const std::span<const std::uint8_t> ports) noexcept {
    if (!value.starts_with("usb:") || !valid_ports(ports)) {
        return false;
    }
    std::size_t offset = 4U;
    const auto consume_number = [&value, &offset](const std::uint8_t number) {
        std::array<char, 3U> expected{};
        const auto converted =
            std::to_chars(expected.data(), expected.data() + expected.size(), number);
        const auto length = static_cast<std::size_t>(
            converted.ptr - expected.data());
        if (converted.ec != std::errc{} || offset + length > value.size() ||
            value.substr(offset, length) !=
                std::string_view{expected.data(), length}) {
            return false;
        }
        offset += length;
        return true;
    };
    if (!consume_number(bus_number) || offset >= value.size() ||
        value[offset++] != '-') {
        return false;
    }
    for (std::size_t index = 0U; index < ports.size(); ++index) {
        if (index != 0U &&
            (offset >= value.size() || value[offset++] != '.')) {
            return false;
        }
        if (!consume_number(ports[index])) {
            return false;
        }
    }
    return offset == value.size();
}

[[nodiscard]] DevicePreflightUsbFingerprint fingerprint(
    const transport::UsbDeviceInfo& device) noexcept {
    return {
        .vendor_id = device.vendor_id,
        .product_id = device.product_id,
        .configuration_value = device.configuration_value,
        .interface_number = device.interface_number,
        .alternate_setting = device.alternate_setting,
        .interface_class = device.interface_class,
        .interface_subclass = device.interface_subclass,
        .interface_protocol = device.interface_protocol,
        .bulk_out_endpoint = device.bulk_out_endpoint,
        .bulk_out_max_packet_size = device.bulk_out_max_packet_size,
        .bulk_in_endpoint = device.bulk_in_endpoint,
        .bulk_in_max_packet_size = device.bulk_in_max_packet_size,
    };
}

[[nodiscard]] bool valid_transport_snapshot(
    const transport::UsbDeviceInfo& device) noexcept {
    const bool valid_bus_number = device.bus_number != 0U ||
        (device.macos_topology.has_value() &&
         !device.linux_topology.has_value() &&
         !device.windows_topology.has_value());
    return device.vendor_id != 0U && device.product_id != 0U &&
        valid_bus_number && device.device_address != 0U &&
        device.configuration_value != 0U && valid_ports(device.port_path) &&
        device.bulk_out_endpoint != 0U &&
        (device.bulk_out_endpoint & 0x80U) == 0U &&
        (device.bulk_in_endpoint & 0x80U) != 0U &&
        device.bulk_out_max_packet_size != 0U &&
        device.bulk_in_max_packet_size != 0U;
}

[[nodiscard]] bool common_topology_matches(
    const transport::UsbDeviceInfo& device,
    const std::string_view physical_port_path,
    const std::string_view root_controller_id,
    const std::span<const std::uint8_t> hub_port_chain,
    const std::uint16_t vendor_id,
    const std::uint16_t product_id,
    const std::uint8_t bus_number,
    const std::uint8_t device_address,
    const std::optional<std::string>& serial) noexcept {
    return !physical_port_path.empty() && !root_controller_id.empty() &&
        valid_ports(hub_port_chain) &&
        canonical_physical_path_matches(
            physical_port_path, bus_number, hub_port_chain) &&
        hub_port_chain.size() == device.port_path.size() &&
        std::ranges::equal(hub_port_chain, device.port_path) &&
        vendor_id == device.vendor_id && product_id == device.product_id &&
        bus_number == device.bus_number &&
        device_address == device.device_address &&
        (serial.has_value() ? std::string_view{*serial} : std::string_view{}) ==
            device.serial_utf8;
}

[[nodiscard]] bool windows_interface_matches(
    const transport::UsbDeviceInfo& device,
    const transport::WindowsUsbInterfaceFingerprint& value) noexcept {
    return value.interface_number == device.interface_number &&
        value.alternate_setting == device.alternate_setting &&
        value.interface_class == device.interface_class &&
        value.interface_subclass == device.interface_subclass &&
        value.interface_protocol == device.interface_protocol;
}

[[nodiscard]] bool macos_interface_matches(
    const transport::UsbDeviceInfo& device,
    const transport::MacUsbInterfaceFingerprint& value) noexcept {
    return value.configuration_value == device.configuration_value &&
        value.interface_number == device.interface_number &&
        value.alternate_setting == device.alternate_setting &&
        value.interface_class == device.interface_class &&
        value.interface_subclass == device.interface_subclass &&
        value.interface_protocol == device.interface_protocol;
}

[[nodiscard]] std::expected<DevicePreflightUsbIdentity,
                            DevicePreflightError>
normalize_snapshot_device(const transport::UsbDeviceInfo& device,
                          const std::size_t snapshot_index) {
    if (!valid_transport_snapshot(device)) {
        return std::unexpected(error(
            DevicePreflightErrorKind::UnreliableTopology,
            DevicePreflightStage::Snapshot,
            "USB transport snapshot is incomplete or malformed",
            std::nullopt,
            snapshot_index));
    }

    const auto topology_count =
        static_cast<unsigned int>(device.linux_topology.has_value()) +
        static_cast<unsigned int>(device.windows_topology.has_value()) +
        static_cast<unsigned int>(device.macos_topology.has_value());
    const bool has_topology_error = device.linux_topology_error.has_value() ||
        device.windows_topology_error.has_value() ||
        device.macos_topology_error.has_value();
    if (topology_count != 1U || has_topology_error) {
        return std::unexpected(error(
            DevicePreflightErrorKind::UnreliableTopology,
            DevicePreflightStage::Snapshot,
            "USB snapshot does not contain one complete platform topology",
            std::nullopt,
            snapshot_index));
    }

    std::string physical_port_path;
    std::string root_controller_id;
    std::vector<std::uint8_t> hub_port_chain;
    std::variant<transport::LinuxUsbTopology,
                 transport::WindowsUsbTopology,
                 transport::MacUsbTopology>
        platform_attestation;
    bool topology_matches = false;
    if (device.linux_topology.has_value()) {
        const auto& topology = *device.linux_topology;
        topology_matches = common_topology_matches(
            device,
            topology.physical_port_path,
            topology.root_controller_id,
            topology.hub_port_chain,
            topology.vendor_id,
            topology.product_id,
            topology.bus_number,
            topology.device_address,
            topology.serial_utf8) &&
            topology.root_controller_id.starts_with("linux-sysfs:") &&
            !topology.sysfs_device_path.empty();
        physical_port_path = topology.physical_port_path;
        root_controller_id = topology.root_controller_id;
        hub_port_chain = topology.hub_port_chain;
        platform_attestation = topology;
    } else if (device.windows_topology.has_value()) {
        const auto& topology = *device.windows_topology;
        topology_matches = common_topology_matches(
            device,
            topology.physical_port_path,
            topology.root_controller_id,
            topology.hub_port_chain,
            topology.vendor_id,
            topology.product_id,
            topology.bus_number,
            topology.device_address,
            topology.serial_utf8) &&
            windows_interface_matches(device, topology.interface_fingerprint) &&
            device.backend_session_id != 0U &&
            topology.root_controller_id.starts_with("windows-pnp:") &&
            !topology.device_instance_id_utf8.empty() &&
            !topology.location_path_utf8.empty();
        physical_port_path = topology.physical_port_path;
        root_controller_id = topology.root_controller_id;
        hub_port_chain = topology.hub_port_chain;
        platform_attestation = topology;
    } else {
        const auto& topology = *device.macos_topology;
        topology_matches = common_topology_matches(
            device,
            topology.physical_port_path,
            topology.root_controller_id,
            topology.hub_port_chain,
            topology.vendor_id,
            topology.product_id,
            topology.bus_number,
            topology.device_address,
            topology.serial_utf8) &&
            macos_interface_matches(device, topology.interface_fingerprint) &&
            topology.root_controller_id.starts_with("macos-iokit:") &&
            topology.hub_port_chain.size() <=
                transport::kMaximumMacUsbTopologyDepth &&
            topology.registry_entry_id != 0U && topology.session_id != 0U &&
            topology.session_id == device.backend_session_id &&
            topology.interface_registry_entry_id != 0U &&
            topology.location_id != 0U &&
            !topology.registry_path.empty() &&
            !topology.interface_registry_path.empty() &&
            !topology.root_controller_registry_path.empty();
        physical_port_path = topology.physical_port_path;
        root_controller_id = topology.root_controller_id;
        hub_port_chain = topology.hub_port_chain;
        platform_attestation = topology;
    }
    if (!topology_matches) {
        return std::unexpected(error(
            DevicePreflightErrorKind::UnreliableTopology,
            DevicePreflightStage::Snapshot,
            "platform topology does not match the complete libusb snapshot",
            std::nullopt,
            snapshot_index));
    }

    return DevicePreflightUsbIdentity{
        .physical_port_path = std::move(physical_port_path),
        .root_controller_id = std::move(root_controller_id),
        .hub_port_chain = std::move(hub_port_chain),
        .bus_number = device.bus_number,
        .device_address = device.device_address,
        .backend_session_id = device.backend_session_id,
        .serial = device.serial_utf8.empty()
            ? std::nullopt
            : std::optional<std::string>{device.serial_utf8},
        .usb_fingerprint = fingerprint(device),
        .platform_attestation = std::move(platform_attestation),
    };
}

[[nodiscard]] bool raw_selector_candidate(
    const transport::UsbDeviceInfo& device,
    const std::unordered_set<std::string_view>& requested_serials,
    const std::unordered_set<std::string_view>& requested_paths) {
    if (requested_serials.contains(device.serial_utf8)) {
        return true;
    }
    return (device.linux_topology.has_value() &&
            requested_paths.contains(
                device.linux_topology->physical_port_path)) ||
        (device.windows_topology.has_value() &&
         requested_paths.contains(
             device.windows_topology->physical_port_path)) ||
        (device.macos_topology.has_value() &&
         requested_paths.contains(
             device.macos_topology->physical_port_path));
}

[[nodiscard]] std::string physical_chain_key(
    const DevicePreflightUsbIdentity& identity) {
    std::string key;
    key.reserve(4U + identity.hub_port_chain.size() * 4U);
    key.append(std::to_string(identity.bus_number));
    for (const auto port : identity.hub_port_chain) {
        key.push_back('.');
        key.append(std::to_string(port));
    }
    return key;
}

[[nodiscard]] bool valid_mode(const fastboot::FastbootUsbMode mode) noexcept {
    switch (mode) {
        case fastboot::FastbootUsbMode::Bootloader:
        case fastboot::FastbootUsbMode::Fastbootd:
            return true;
    }
    return false;
}

[[nodiscard]] DevicePreflightError open_error(
    DevicePreflightOpenError source,
    const std::size_t target_index,
    const std::size_t snapshot_index) {
    auto kind = DevicePreflightErrorKind::OpenFailed;
    if (source.code == DevicePreflightOpenErrorCode::Cancelled) {
        kind = DevicePreflightErrorKind::Cancelled;
    } else if (source.code ==
               DevicePreflightOpenErrorCode::DeadlineExceeded) {
        kind = DevicePreflightErrorKind::DeadlineExceeded;
    } else if (source.code ==
               DevicePreflightOpenErrorCode::ResourceExhausted) {
        kind = DevicePreflightErrorKind::ResourceExhausted;
    }
    auto result = error(kind,
                        DevicePreflightStage::Opening,
                        source.message,
                        target_index,
                        snapshot_index);
    result.native_code = source.native_code;
    result.outbound_certainty = source.outbound_certainty;
    result.open_error = std::move(source);
    return result;
}

[[nodiscard]] DevicePreflightOpenError runtime_open_error(
    const transport::LibusbRuntimeError& source) {
    auto code = DevicePreflightOpenErrorCode::TransportFailure;
    using transport::LibusbRuntimeErrorKind;
    switch (source.kind) {
        case LibusbRuntimeErrorKind::operation_cancelled:
            code = DevicePreflightOpenErrorCode::Cancelled;
            break;
        case LibusbRuntimeErrorKind::operation_timed_out:
            code = DevicePreflightOpenErrorCode::DeadlineExceeded;
            break;
        case LibusbRuntimeErrorKind::device_not_found:
        case LibusbRuntimeErrorKind::identity_changed:
            code = DevicePreflightOpenErrorCode::NotFound;
            break;
        case LibusbRuntimeErrorKind::interface_busy:
            code = DevicePreflightOpenErrorCode::Busy;
            break;
        default:
            if (source.native_code == LIBUSB_ERROR_ACCESS) {
                code = DevicePreflightOpenErrorCode::PermissionDenied;
            } else if (source.native_code == LIBUSB_ERROR_NOT_SUPPORTED) {
                code = DevicePreflightOpenErrorCode::DriverUnavailable;
            } else if (source.native_code == LIBUSB_ERROR_NO_MEM) {
                code = DevicePreflightOpenErrorCode::ResourceExhausted;
            }
            break;
    }
    return {
        .code = code,
        .message = "unable to open the selected Fastboot USB interface",
        .native_code = source.native_code,
        .outbound_certainty = protocol::TransferCertainty::NotTransferred,
    };
}

class LibusbDevicePreflightSessionOpener final
    : public IDevicePreflightSessionOpener {
public:
    explicit LibusbDevicePreflightSessionOpener(
        std::shared_ptr<transport::LibusbRuntime> runtime,
        transport::UsbFastbootTransportOptions options) noexcept
        : runtime_(std::move(runtime)), options_(std::move(options)) {}

    [[nodiscard]] std::expected<OpenedDevicePreflightSession,
                                DevicePreflightOpenError>
    open(const transport::UsbDeviceInfo& device,
         const DevicePreflightTimePoint deadline,
         const std::stop_token cancellation) override {
        auto verified = runtime_->open_bulk_out_verified(
            device, deadline, cancellation);
        if (!verified) {
            return std::unexpected(runtime_open_error(verified.error()));
        }
        auto identity = normalize_snapshot_device(
            verified->verified_identity(), 0U);
        if (!identity) {
            return std::unexpected(DevicePreflightOpenError{
                .code = DevicePreflightOpenErrorCode::TransportFailure,
                .message = identity.error().message,
                .native_code = identity.error().native_code,
                .outbound_certainty =
                    protocol::TransferCertainty::NotTransferred,
            });
        }
        auto transport = transport::UsbFastbootTransport::adopt_verified(
            std::move(*verified), options_);
        if (!transport) {
            return std::unexpected(runtime_open_error(transport.error()));
        }
        return OpenedDevicePreflightSession{
            .verified_usb_identity = std::move(*identity),
            .session = std::make_unique<protocol::FastbootSession>(
                std::move(*transport)),
        };
    }

private:
    std::shared_ptr<transport::LibusbRuntime> runtime_;
    transport::UsbFastbootTransportOptions options_;
};

[[nodiscard]] DevicePreflightError probe_error(
    DevicePreflightProbeError source,
    const std::size_t target_index,
    const std::size_t snapshot_index) {
    auto kind = DevicePreflightErrorKind::ProbeFailed;
    if (source.code == DevicePreflightProbeErrorCode::Cancelled) {
        kind = DevicePreflightErrorKind::Cancelled;
    } else if (source.code ==
               DevicePreflightProbeErrorCode::DeadlineExceeded) {
        kind = DevicePreflightErrorKind::DeadlineExceeded;
    } else if (source.code ==
               DevicePreflightProbeErrorCode::ResourceExhausted) {
        kind = DevicePreflightErrorKind::ResourceExhausted;
    }
    auto result = error(kind,
                        DevicePreflightStage::LiveIdentity,
                        source.message,
                        target_index,
                        snapshot_index);
    result.native_code = source.native_code;
    result.outbound_certainty = source.outbound_certainty;
    result.probe_error = std::move(source);
    return result;
}

struct CancelSession final {
    protocol::FastbootSession* session{};

    void operator()() const noexcept { session->request_cancel(); }
};

class ProbeInterruptionGuard final {
public:
    ProbeInterruptionGuard(protocol::FastbootSession& session,
                           const DevicePreflightTimePoint deadline,
                           const std::stop_token cancellation)
        : session_(session),
          cancellation_(cancellation, CancelSession{&session_}),
          deadline_thread_([this, deadline](const std::stop_token stop) {
              std::unique_lock lock(mutex_);
              const bool finished = condition_.wait_until(
                  lock, stop, deadline, [this] { return finished_; });
              if (!finished) {
                  deadline_fired_.store(true, std::memory_order_release);
                  session_.request_cancel();
              }
          }) {}

    ProbeInterruptionGuard(const ProbeInterruptionGuard&) = delete;
    ProbeInterruptionGuard& operator=(const ProbeInterruptionGuard&) = delete;

    ~ProbeInterruptionGuard() {
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        deadline_thread_.request_stop();
        condition_.notify_all();
    }

    [[nodiscard]] bool deadline_fired() const noexcept {
        return deadline_fired_.load(std::memory_order_acquire);
    }

private:
    protocol::FastbootSession& session_;
    std::stop_callback<CancelSession> cancellation_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    bool finished_{};
    std::atomic<bool> deadline_fired_{};
    std::jthread deadline_thread_;
};

[[nodiscard]] DevicePreflightProbeError primitive_probe_error(
    fastboot::PrimitiveError source,
    const bool deadline_fired,
    const std::stop_token cancellation) {
    auto code = DevicePreflightProbeErrorCode::ProtocolFailure;
    if (cancellation.stop_requested()) {
        code = DevicePreflightProbeErrorCode::Cancelled;
    } else if (deadline_fired) {
        code = DevicePreflightProbeErrorCode::DeadlineExceeded;
    } else if (source.code == fastboot::PrimitiveErrorCode::Cancelled) {
        code = DevicePreflightProbeErrorCode::Cancelled;
    } else if (source.code == fastboot::PrimitiveErrorCode::DeviceFail) {
        code = DevicePreflightProbeErrorCode::DeviceRejected;
    }
    return {
        .code = code,
        .message = std::move(source.message),
        .native_code = source.native_code,
        .outbound_certainty = source.outbound_certainty,
    };
}

}  // namespace

std::expected<std::unique_ptr<IDevicePreflightSessionOpener>,
              DevicePreflightOpenError>
make_libusb_device_preflight_session_opener(
    std::shared_ptr<transport::LibusbRuntime> runtime,
    std::shared_ptr<transport::BufferBudget> buffer_budget,
    const transport::TransferRingConfig data_ring) noexcept {
    if (runtime == nullptr || !runtime->running() || buffer_budget == nullptr ||
        data_ring.chunk_size == 0U || data_ring.depth == 0U) {
        return std::unexpected(DevicePreflightOpenError{
            .code = DevicePreflightOpenErrorCode::TransportFailure,
            .message = "fleet preflight requires a running libusb runtime",
            .native_code = 0,
            .outbound_certainty =
                protocol::TransferCertainty::NotTransferred,
        });
    }
    try {
        transport::UsbFastbootTransportOptions options;
        options.buffer_budget = std::move(buffer_budget);
        options.data_ring = data_ring;
        return std::unique_ptr<IDevicePreflightSessionOpener>(
            new LibusbDevicePreflightSessionOpener(
                std::move(runtime), std::move(options)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(DevicePreflightOpenError{
            .code = DevicePreflightOpenErrorCode::ResourceExhausted,
            .message = "unable to allocate the libusb fleet preflight opener",
            .native_code = LIBUSB_ERROR_NO_MEM,
            .outbound_certainty =
                protocol::TransferCertainty::NotTransferred,
        });
    } catch (...) {
        return std::unexpected(DevicePreflightOpenError{
            .code = DevicePreflightOpenErrorCode::UnexpectedFailure,
            .message = "unable to create the libusb fleet preflight opener",
            .native_code = LIBUSB_ERROR_OTHER,
            .outbound_certainty =
                protocol::TransferCertainty::NotTransferred,
        });
    }
}

std::expected<DevicePreflightProbeResult, DevicePreflightProbeError>
FastbootDevicePreflightProbe::probe(
    protocol::FastbootSession& session,
    const DevicePreflightTimePoint deadline,
    const std::stop_token cancellation) {
    try {
        if (cancellation.stop_requested()) {
            return std::unexpected(DevicePreflightProbeError{
                .code = DevicePreflightProbeErrorCode::Cancelled,
                .message = "Fastboot identity probe was cancelled",
            });
        }
        if (DevicePreflightClock::now() >= deadline) {
            return std::unexpected(DevicePreflightProbeError{
                .code = DevicePreflightProbeErrorCode::DeadlineExceeded,
                .message = "Fastboot identity probe deadline expired",
            });
        }

        ProbeInterruptionGuard guard(session, deadline, cancellation);
        fastboot::PrimitiveService service(session);
        auto product = service.getvar("product");
        if (!product) {
            return std::unexpected(primitive_probe_error(
                std::move(product.error()),
                guard.deadline_fired(),
                cancellation));
        }
        if (cancellation.stop_requested()) {
            return std::unexpected(DevicePreflightProbeError{
                .code = DevicePreflightProbeErrorCode::Cancelled,
                .message = "Fastboot identity probe was cancelled",
                .outbound_certainty = product->outbound_certainty,
            });
        }
        if (guard.deadline_fired() ||
            DevicePreflightClock::now() >= deadline) {
            return std::unexpected(DevicePreflightProbeError{
                .code = DevicePreflightProbeErrorCode::DeadlineExceeded,
                .message = "Fastboot identity probe deadline expired",
                .outbound_certainty = product->outbound_certainty,
            });
        }
        if (product->terminal.payload.empty()) {
            return std::unexpected(DevicePreflightProbeError{
                .code = DevicePreflightProbeErrorCode::InvalidResponse,
                .message = "Fastboot getvar:product returned an empty identity",
                .outbound_certainty = product->outbound_certainty,
            });
        }

        auto mode = service.getvar("is-userspace");
        auto observed_mode = fastboot::FastbootUsbMode::Bootloader;
        protocol::TransferCertainty mode_certainty{
            protocol::TransferCertainty::FullyTransferred};
        if (!mode) {
            mode_certainty = mode.error().outbound_certainty;
            if (mode.error().code != fastboot::PrimitiveErrorCode::DeviceFail) {
                return std::unexpected(primitive_probe_error(
                    std::move(mode.error()),
                    guard.deadline_fired(),
                    cancellation));
            }
            // Frozen AOSP compatibility: an explicit FAIL for is-userspace is
            // the legacy bootloader-mode result. The query still occurred on
            // the live protocol session.
        } else {
            mode_certainty = mode->outbound_certainty;
            if (mode->terminal.payload == "yes") {
                observed_mode = fastboot::FastbootUsbMode::Fastbootd;
            } else if (mode->terminal.payload != "no") {
                return std::unexpected(DevicePreflightProbeError{
                    .code = DevicePreflightProbeErrorCode::InvalidResponse,
                    .message =
                        "Fastboot is-userspace must be exactly 'yes' or 'no'",
                    .outbound_certainty = mode->outbound_certainty,
                });
            }
        }
        if (cancellation.stop_requested()) {
            return std::unexpected(DevicePreflightProbeError{
                .code = DevicePreflightProbeErrorCode::Cancelled,
                .message = "Fastboot identity probe was cancelled",
                .outbound_certainty = mode_certainty,
            });
        }
        if (guard.deadline_fired() ||
            DevicePreflightClock::now() >= deadline) {
            return std::unexpected(DevicePreflightProbeError{
                .code = DevicePreflightProbeErrorCode::DeadlineExceeded,
                .message = "Fastboot identity probe deadline expired",
                .outbound_certainty = mode_certainty,
            });
        }

        return DevicePreflightProbeResult{
            .product = std::move(product->terminal.payload),
            .mode = observed_mode,
            .product_query_completed = true,
            .mode_query_completed = true,
        };
    } catch (const std::bad_alloc&) {
        return std::unexpected(DevicePreflightProbeError{
            .code = DevicePreflightProbeErrorCode::ResourceExhausted,
            .message = {},
            .native_code = 0,
            .outbound_certainty = protocol::TransferCertainty::NotTransferred,
        });
    } catch (...) {
        return std::unexpected(DevicePreflightProbeError{
            .code = DevicePreflightProbeErrorCode::UnexpectedFailure,
            .message = {},
            .native_code = 0,
            .outbound_certainty = protocol::TransferCertainty::NotTransferred,
        });
    }
}

PreparedDeviceSession::PreparedDeviceSession(
    const std::size_t target_index,
    std::string target_name,
    std::string expected_product,
    std::string observed_product,
    const fastboot::FastbootUsbMode observed_mode,
    DevicePreflightUsbIdentity usb_identity,
    std::unique_ptr<protocol::FastbootSession> session) noexcept
    : target_index_(target_index),
      target_name_(std::move(target_name)),
      expected_product_(std::move(expected_product)),
      observed_product_(std::move(observed_product)),
      observed_mode_(observed_mode),
      usb_identity_(std::move(usb_identity)),
      session_(std::move(session)) {}

std::size_t PreparedDeviceSession::target_index() const noexcept {
    return target_index_;
}

std::string_view PreparedDeviceSession::target_name() const noexcept {
    return target_name_;
}

std::string_view PreparedDeviceSession::expected_product() const noexcept {
    return expected_product_;
}

std::string_view PreparedDeviceSession::observed_product() const noexcept {
    return observed_product_;
}

fastboot::FastbootUsbMode PreparedDeviceSession::observed_mode() const noexcept {
    return observed_mode_;
}

const DevicePreflightUsbIdentity& PreparedDeviceSession::usb_identity() const
    noexcept {
    return usb_identity_;
}

std::unique_ptr<protocol::FastbootSession>
PreparedDeviceSession::take_session() && noexcept {
    return std::move(session_);
}

PreparedDeviceBatchConsumption::PreparedDeviceBatchConsumption(
    image::Sha256Digest plan_sha256,
    std::vector<PreparedDeviceSession> devices) noexcept
    : plan_sha256_(plan_sha256), devices_(std::move(devices)) {}

PreparedDeviceBatchConsumption::PreparedDeviceBatchConsumption(
    PreparedDeviceBatchConsumption&& other) noexcept
    : plan_sha256_(other.plan_sha256_), devices_(std::move(other.devices_)) {
    other.devices_.clear();
}

PreparedDeviceBatchConsumption&
PreparedDeviceBatchConsumption::operator=(
    PreparedDeviceBatchConsumption&& other) noexcept {
    if (this != &other) {
        plan_sha256_ = other.plan_sha256_;
        devices_ = std::move(other.devices_);
        other.devices_.clear();
    }
    return *this;
}

const image::Sha256Digest&
PreparedDeviceBatchConsumption::plan_sha256() const noexcept {
    return plan_sha256_;
}

std::span<const PreparedDeviceSession>
PreparedDeviceBatchConsumption::devices() const noexcept {
    return devices_;
}

std::vector<PreparedDeviceSession>
PreparedDeviceBatchConsumption::take_sessions_for_actor() && noexcept {
    auto result = std::move(devices_);
    devices_.clear();
    return result;
}

PreparedDeviceBatch::PreparedDeviceBatch(
    image::Sha256Digest plan_sha256,
    std::vector<PreparedDeviceSession> devices) noexcept
    : plan_sha256_(plan_sha256), devices_(std::move(devices)) {}

PreparedDeviceBatch::PreparedDeviceBatch(PreparedDeviceBatch&& other) noexcept
    : plan_sha256_(other.plan_sha256_), devices_(std::move(other.devices_)) {
    other.devices_.clear();
}

PreparedDeviceBatch& PreparedDeviceBatch::operator=(
    PreparedDeviceBatch&& other) noexcept {
    if (this != &other) {
        plan_sha256_ = other.plan_sha256_;
        devices_ = std::move(other.devices_);
        other.devices_.clear();
    }
    return *this;
}

const image::Sha256Digest& PreparedDeviceBatch::plan_sha256() const noexcept {
    return plan_sha256_;
}

std::span<const PreparedDeviceSession> PreparedDeviceBatch::devices() const
    noexcept {
    return devices_;
}

std::expected<PreparedDeviceBatchConsumption,
              PreparedDeviceBatchConsumptionError>
PreparedDeviceBatch::consume(
    const image::Sha256Digest& expected_plan_sha256) && noexcept {
    if (expected_plan_sha256 != plan_sha256_) {
        return std::unexpected(
            PreparedDeviceBatchConsumptionError::PlanDigestMismatch);
    }
    if (devices_.empty()) {
        return std::unexpected(
            PreparedDeviceBatchConsumptionError::AlreadyConsumed);
    }
    auto result = PreparedDeviceBatchConsumption{
        plan_sha256_, std::move(devices_)};
    devices_.clear();
    return result;
}

std::expected<PreparedDeviceBatch, DevicePreflightError>
preflight_fleet_devices(
    const JobPlan& plan,
    const std::span<const transport::UsbDeviceInfo> snapshot,
    IDevicePreflightSessionOpener& opener,
    IDevicePreflightProbe& probe,
    const DevicePreflightTimePoint deadline,
    const std::stop_token cancellation) noexcept {
    try {
        if (const auto stopped = interrupted(
                deadline, cancellation, DevicePreflightStage::Validation)) {
            return std::unexpected(*stopped);
        }
        const auto& targets = plan.manifest().targets;
        if (targets.empty()) {
            return std::unexpected(error(
                DevicePreflightErrorKind::InvalidArgument,
                DevicePreflightStage::Validation,
                "JobPlan contains no fleet targets"));
        }

        std::unordered_set<std::string_view> requested_serials;
        std::unordered_set<std::string_view> requested_paths;
        for (const auto& target : targets) {
            for (const auto& selector : target.selector.serials) {
                requested_serials.emplace(selector.value);
            }
            for (const auto& selector : target.selector.usb_paths) {
                requested_paths.emplace(selector.value);
            }
        }

        std::vector<NormalizedSnapshotDevice> devices;
        const auto candidate_capacity =
            std::min(snapshot.size(), kMaximumPreflightDevices);
        devices.reserve(candidate_capacity);
        std::unordered_map<std::string, std::size_t> physical_paths;
        std::unordered_map<std::string, std::size_t> physical_chains;
        std::unordered_map<std::string, std::size_t> serials;
        physical_paths.reserve(candidate_capacity);
        physical_chains.reserve(candidate_capacity);
        serials.reserve(candidate_capacity);
        for (std::size_t index = 0U; index < snapshot.size(); ++index) {
            if (const auto stopped = interrupted(
                    deadline, cancellation, DevicePreflightStage::Snapshot)) {
                return std::unexpected(*stopped);
            }
            if (!raw_selector_candidate(
                    snapshot[index], requested_serials, requested_paths)) {
                continue;
            }
            if (devices.size() >= kMaximumPreflightDevices) {
                return std::unexpected(error(
                    DevicePreflightErrorKind::SnapshotLimitExceeded,
                    DevicePreflightStage::Snapshot,
                    "matching USB candidates exceed the fleet preflight device limit",
                    std::nullopt,
                    index));
            }
            auto identity = normalize_snapshot_device(snapshot[index], index);
            if (!identity) {
                return std::unexpected(std::move(identity.error()));
            }
            const auto device_index = devices.size();
            if (!physical_paths
                     .emplace(identity->physical_port_path, device_index)
                     .second) {
                return std::unexpected(error(
                    DevicePreflightErrorKind::DuplicatePhysicalPath,
                    DevicePreflightStage::Snapshot,
                    "USB snapshot contains a duplicate physical port path",
                    std::nullopt,
                    index));
            }
            const auto chain_key = physical_chain_key(*identity);
            if (!physical_chains.emplace(chain_key, device_index).second) {
                return std::unexpected(error(
                    DevicePreflightErrorKind::UnreliableTopology,
                    DevicePreflightStage::Snapshot,
                    "USB snapshot maps one physical hub chain to multiple paths",
                    std::nullopt,
                    index));
            }
            if (identity->serial.has_value() &&
                !serials.emplace(*identity->serial, device_index).second) {
                return std::unexpected(error(
                    DevicePreflightErrorKind::DuplicateSerial,
                    DevicePreflightStage::Snapshot,
                    "USB serial identifies more than one physical device",
                    std::nullopt,
                    index));
            }
            devices.push_back(
                {&snapshot[index], index, std::move(*identity)});
        }

        std::vector<std::optional<std::size_t>> target_owner(devices.size());
        std::vector<SelectedDevice> selected;
        selected.reserve(devices.size());
        for (std::size_t target_index = 0U;
             target_index < targets.size();
             ++target_index) {
            if (const auto stopped = interrupted(
                    deadline, cancellation, DevicePreflightStage::Selection)) {
                return std::unexpected(*stopped);
            }
            const auto& target = targets[target_index];
            std::vector<std::size_t> target_devices;
            target_devices.reserve(target.selector.serials.size() +
                                   target.selector.usb_paths.size());
            for (const auto& selector : target.selector.serials) {
                const auto found = serials.find(selector.value);
                if (found == serials.end()) {
                    return std::unexpected(error(
                        DevicePreflightErrorKind::MissingSelectorDevice,
                        DevicePreflightStage::Selection,
                        "fleet serial selector did not resolve to one device",
                        target_index));
                }
                target_devices.push_back(found->second);
            }
            for (const auto& selector : target.selector.usb_paths) {
                const auto found = physical_paths.find(selector.value);
                if (found == physical_paths.end()) {
                    return std::unexpected(error(
                        DevicePreflightErrorKind::MissingSelectorDevice,
                        DevicePreflightStage::Selection,
                        "fleet USB path selector did not resolve to one device",
                        target_index));
                }
                target_devices.push_back(found->second);
            }
            std::ranges::sort(target_devices);
            const auto unique_end =
                std::ranges::unique(target_devices).begin();
            target_devices.erase(unique_end, target_devices.end());
            if (target_devices.empty()) {
                return std::unexpected(error(
                    DevicePreflightErrorKind::MissingTargetDevice,
                    DevicePreflightStage::Selection,
                    "fleet target resolved to no devices",
                    target_index));
            }
            for (const auto device_index : target_devices) {
                if (target_owner[device_index].has_value() &&
                    *target_owner[device_index] != target_index) {
                    return std::unexpected(error(
                        DevicePreflightErrorKind::DeviceMatchesMultipleTargets,
                        DevicePreflightStage::Selection,
                        "one physical device matches more than one fleet target",
                        target_index,
                        devices[device_index].snapshot_index));
                }
                target_owner[device_index] = target_index;
                selected.push_back({target_index, device_index});
            }
        }

        std::ranges::sort(selected, [&devices](const SelectedDevice& left,
                                               const SelectedDevice& right) {
            if (left.target_index != right.target_index) {
                return left.target_index < right.target_index;
            }
            const auto& left_identity = devices[left.device_index].identity;
            const auto& right_identity = devices[right.device_index].identity;
            if (left_identity.physical_port_path !=
                right_identity.physical_port_path) {
                return left_identity.physical_port_path <
                    right_identity.physical_port_path;
            }
            return devices[left.device_index].snapshot_index <
                devices[right.device_index].snapshot_index;
        });

        std::vector<PreparedDeviceSession> prepared;
        std::vector<DevicePreflightOutcome> outcomes;
        prepared.reserve(selected.size());
        outcomes.reserve(selected.size());
        bool product_mismatch = false;
        for (const auto& selection : selected) {
            if (const auto stopped = interrupted(
                    deadline, cancellation, DevicePreflightStage::Opening)) {
                return std::unexpected(*stopped);
            }
            const auto& snapshot_device = devices[selection.device_index];
            const auto& target = targets[selection.target_index];
            auto opened = opener.open(*snapshot_device.source,
                                      deadline,
                                      cancellation);
            if (!opened) {
                return std::unexpected(open_error(
                    std::move(opened.error()),
                    selection.target_index,
                    snapshot_device.snapshot_index));
            }
            if (const auto stopped = interrupted(
                    deadline, cancellation, DevicePreflightStage::Opening)) {
                return std::unexpected(*stopped);
            }
            if (opened->session == nullptr) {
                return std::unexpected(error(
                    DevicePreflightErrorKind::OpenContractViolation,
                    DevicePreflightStage::Opening,
                    "exclusive opener returned no Fastboot session",
                    selection.target_index,
                    snapshot_device.snapshot_index));
            }
            if (opened->verified_usb_identity != snapshot_device.identity) {
                return std::unexpected(error(
                    DevicePreflightErrorKind::DeviceChangedDuringOpen,
                    DevicePreflightStage::Opening,
                    "serial, physical path, topology, or USB fingerprint changed during exclusive open",
                    selection.target_index,
                    snapshot_device.snapshot_index));
            }

            auto live = probe.probe(*opened->session, deadline, cancellation);
            if (!live) {
                return std::unexpected(probe_error(
                    std::move(live.error()),
                    selection.target_index,
                    snapshot_device.snapshot_index));
            }
            if (const auto stopped = interrupted(
                    deadline, cancellation, DevicePreflightStage::LiveIdentity)) {
                return std::unexpected(*stopped);
            }
            if (!live->product_query_completed ||
                !live->mode_query_completed || live->product.empty() ||
                !valid_mode(live->mode)) {
                return std::unexpected(error(
                    DevicePreflightErrorKind::ProbeContractViolation,
                    DevicePreflightStage::LiveIdentity,
                    "identity probe did not prove live Fastboot product and mode",
                    selection.target_index,
                    snapshot_device.snapshot_index));
            }

            const bool matches =
                live->product == target.expected_product.value;
            product_mismatch = product_mismatch || !matches;
            outcomes.push_back(DevicePreflightOutcome{
                .code = matches ? DevicePreflightOutcomeCode::Ready
                                : DevicePreflightOutcomeCode::ProductMismatch,
                .target_index = selection.target_index,
                .target_name = target.name.value,
                .usb_identity = snapshot_device.identity,
                .expected_product = target.expected_product.value,
                .observed_product = live->product,
                .observed_mode = live->mode,
            });
            prepared.push_back(PreparedDeviceSession{
                selection.target_index,
                target.name.value,
                target.expected_product.value,
                std::move(live->product),
                live->mode,
                snapshot_device.identity,
                std::move(opened->session),
            });
        }

        if (product_mismatch) {
            auto mismatch = error(
                DevicePreflightErrorKind::ProductMismatch,
                DevicePreflightStage::ProductBarrier,
                "one or more live Fastboot products do not match the JobPlan");
            mismatch.outcomes = std::move(outcomes);
            return std::unexpected(std::move(mismatch));
        }
        return PreparedDeviceBatch{plan.sha256(), std::move(prepared)};
    } catch (const std::bad_alloc&) {
        return std::unexpected(DevicePreflightError{
            .kind = DevicePreflightErrorKind::ResourceExhausted,
            .stage = DevicePreflightStage::Validation,
            .message = {},
            .target_index = std::nullopt,
            .snapshot_index = std::nullopt,
            .native_code = 0,
            .outbound_certainty = protocol::TransferCertainty::NotTransferred,
            .open_error = std::nullopt,
            .probe_error = std::nullopt,
            .outcomes = {},
        });
    } catch (...) {
        return std::unexpected(DevicePreflightError{
            .kind = DevicePreflightErrorKind::UnexpectedFailure,
            .stage = DevicePreflightStage::Validation,
            .message = {},
            .target_index = std::nullopt,
            .snapshot_index = std::nullopt,
            .native_code = 0,
            .outbound_certainty = protocol::TransferCertainty::NotTransferred,
            .open_error = std::nullopt,
            .probe_error = std::nullopt,
            .outcomes = {},
        });
    }
}

}  // namespace kairosboot::fleet
