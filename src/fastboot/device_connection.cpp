// SPDX-License-Identifier: MIT
#include "src/fastboot/device_connection.hpp"

#include "src/fastboot/primitive_service.hpp"
#include "src/transport/usb_fastboot.hpp"

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <ranges>
#include <span>
#include <thread>
#include <utility>

namespace kairosboot::fastboot {
namespace {

[[nodiscard]] DevicePreflightOpenError open_error(
    const DevicePreflightOpenErrorCode code,
    std::string message,
    const int native_code = 0) {
    return {
        .code = code,
        .message = std::move(message),
        .native_code = native_code,
        .outbound_certainty = protocol::TransferCertainty::NotTransferred,
    };
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
    return open_error(code,
                      "unable to open the selected Fastboot USB interface",
                      source.native_code);
}

[[nodiscard]] bool valid_ports(
    const std::span<const std::uint8_t> ports) noexcept {
    return !ports.empty() &&
        ports.size() <= transport::kMaximumUsbTopologyDepth &&
        std::ranges::all_of(ports, [](const std::uint8_t port) {
            return port != 0U;
        });
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

[[nodiscard]] std::string canonical_physical_path(
    const std::uint8_t bus_number,
    const std::span<const std::uint8_t> ports) {
    std::string result = "usb:" + std::to_string(bus_number) + '-';
    for (std::size_t index = 0U; index < ports.size(); ++index) {
        if (index != 0U) {
            result.push_back('.');
        }
        result.append(std::to_string(ports[index]));
    }
    return result;
}

[[nodiscard]] std::expected<DevicePreflightUsbIdentity,
                            DevicePreflightOpenError>
verified_identity(const transport::UsbDeviceInfo& device) {
    const bool valid_bus = device.bus_number != 0U ||
        (device.macos_topology.has_value() &&
         !device.linux_topology.has_value() &&
         !device.windows_topology.has_value());
    const auto topology_count =
        static_cast<unsigned int>(device.linux_topology.has_value()) +
        static_cast<unsigned int>(device.windows_topology.has_value()) +
        static_cast<unsigned int>(device.macos_topology.has_value());
    if (device.vendor_id == 0U || device.product_id == 0U || !valid_bus ||
        device.device_address == 0U || device.configuration_value == 0U ||
        !valid_ports(device.port_path) ||
        device.bulk_out_endpoint == 0U ||
        (device.bulk_out_endpoint & 0x80U) != 0U ||
        (device.bulk_in_endpoint & 0x80U) == 0U ||
        device.bulk_out_max_packet_size == 0U ||
        device.bulk_in_max_packet_size == 0U || topology_count != 1U ||
        device.linux_topology_error.has_value() ||
        device.windows_topology_error.has_value() ||
        device.macos_topology_error.has_value()) {
        return std::unexpected(open_error(
            DevicePreflightOpenErrorCode::TransportFailure,
            "verified USB identity is incomplete or malformed"));
    }

    std::string physical_port_path;
    std::string root_controller_id;
    std::vector<std::uint8_t> hub_port_chain;
    std::variant<transport::LinuxUsbTopology,
                 transport::WindowsUsbTopology,
                 transport::MacUsbTopology>
        attestation;
    std::optional<std::string> topology_serial;
    bool topology_valid = false;
    const auto expected_physical_path =
        canonical_physical_path(device.bus_number, device.port_path);
    if (device.linux_topology.has_value()) {
        const auto& topology = *device.linux_topology;
        physical_port_path = topology.physical_port_path;
        root_controller_id = topology.root_controller_id;
        hub_port_chain = topology.hub_port_chain;
        topology_serial = topology.serial_utf8;
        attestation = topology;
        topology_valid = topology.physical_port_path == expected_physical_path &&
            topology.root_controller_id.starts_with("linux-sysfs:") &&
            !topology.sysfs_device_path.empty() &&
            topology.vendor_id == device.vendor_id &&
            topology.product_id == device.product_id &&
            topology.bus_number == device.bus_number &&
            topology.device_address == device.device_address;
    } else if (device.windows_topology.has_value()) {
        const auto& topology = *device.windows_topology;
        physical_port_path = topology.physical_port_path;
        root_controller_id = topology.root_controller_id;
        hub_port_chain = topology.hub_port_chain;
        topology_serial = topology.serial_utf8;
        attestation = topology;
        topology_valid = topology.physical_port_path == expected_physical_path &&
            topology.root_controller_id.starts_with("windows-pnp:") &&
            !topology.device_instance_id_utf8.empty() &&
            !topology.location_path_utf8.empty() &&
            device.backend_session_id != 0U &&
            topology.vendor_id == device.vendor_id &&
            topology.product_id == device.product_id &&
            topology.bus_number == device.bus_number &&
            topology.device_address == device.device_address &&
            topology.interface_fingerprint.interface_number ==
                device.interface_number &&
            topology.interface_fingerprint.alternate_setting ==
                device.alternate_setting &&
            topology.interface_fingerprint.interface_class ==
                device.interface_class &&
            topology.interface_fingerprint.interface_subclass ==
                device.interface_subclass &&
            topology.interface_fingerprint.interface_protocol ==
                device.interface_protocol;
    } else {
        const auto& topology = *device.macos_topology;
        physical_port_path = topology.physical_port_path;
        root_controller_id = topology.root_controller_id;
        hub_port_chain = topology.hub_port_chain;
        topology_serial = topology.serial_utf8;
        attestation = topology;
        topology_valid = topology.physical_port_path == expected_physical_path &&
            topology.root_controller_id.starts_with("macos-iokit:") &&
            topology.hub_port_chain.size() <=
                transport::kMaximumMacUsbTopologyDepth &&
            topology.vendor_id == device.vendor_id &&
            topology.product_id == device.product_id &&
            topology.bus_number == device.bus_number &&
            topology.device_address == device.device_address &&
            topology.session_id != 0U &&
            topology.session_id == device.backend_session_id &&
            topology.registry_entry_id != 0U &&
            topology.interface_registry_entry_id != 0U &&
            topology.location_id != 0U &&
            !topology.registry_path.empty() &&
            !topology.interface_registry_path.empty() &&
            !topology.root_controller_registry_path.empty() &&
            topology.interface_fingerprint.configuration_value ==
                device.configuration_value &&
            topology.interface_fingerprint.interface_number ==
                device.interface_number &&
            topology.interface_fingerprint.alternate_setting ==
                device.alternate_setting &&
            topology.interface_fingerprint.interface_class ==
                device.interface_class &&
            topology.interface_fingerprint.interface_subclass ==
                device.interface_subclass &&
            topology.interface_fingerprint.interface_protocol ==
                device.interface_protocol;
    }

    const auto serial = device.serial_utf8.empty()
        ? std::optional<std::string>{}
        : std::optional<std::string>{device.serial_utf8};
    if (!topology_valid || physical_port_path.empty() ||
        root_controller_id.empty() ||
        hub_port_chain != device.port_path || topology_serial != serial) {
        return std::unexpected(open_error(
            DevicePreflightOpenErrorCode::TransportFailure,
            "platform topology does not match the verified USB identity"));
    }

    return DevicePreflightUsbIdentity{
        .physical_port_path = std::move(physical_port_path),
        .root_controller_id = std::move(root_controller_id),
        .hub_port_chain = std::move(hub_port_chain),
        .bus_number = device.bus_number,
        .device_address = device.device_address,
        .backend_session_id = device.backend_session_id,
        .serial = serial,
        .usb_fingerprint = fingerprint(device),
        .platform_attestation = std::move(attestation),
    };
}

class LibusbDevicePreflightSessionOpener final
    : public IDevicePreflightSessionOpener {
public:
    LibusbDevicePreflightSessionOpener(
        std::shared_ptr<transport::LibusbRuntime> runtime,
        transport::UsbFastbootTransportOptions options,
        const protocol::SessionOptions session_options) noexcept
        : runtime_(std::move(runtime)), options_(std::move(options)),
          session_options_(session_options) {}

    [[nodiscard]] std::expected<OpenedDevicePreflightSession,
                                DevicePreflightOpenError>
    open(const transport::UsbDeviceInfo& device,
         const DevicePreflightTimePoint deadline,
         const std::stop_token cancellation) override {
        auto opened = runtime_->open_bulk_out_verified(
            device, deadline, cancellation);
        if (!opened) {
            return std::unexpected(runtime_open_error(opened.error()));
        }
        auto identity = verified_identity(opened->verified_identity());
        if (!identity) {
            return std::unexpected(std::move(identity.error()));
        }
        auto transport = transport::UsbFastbootTransport::adopt_verified(
            std::move(*opened), [&] {
                auto options = options_;
                options.absolute_deadline = options.absolute_deadline.has_value()
                    ? std::min(*options.absolute_deadline, deadline)
                    : deadline;
                return options;
            }());
        if (!transport) {
            return std::unexpected(runtime_open_error(transport.error()));
        }
        return OpenedDevicePreflightSession{
            .verified_usb_identity = std::move(*identity),
            .session = std::make_unique<protocol::FastbootSession>(
                std::move(*transport), session_options_),
        };
    }

private:
    std::shared_ptr<transport::LibusbRuntime> runtime_;
    transport::UsbFastbootTransportOptions options_;
    protocol::SessionOptions session_options_;
};

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
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool finished_{};
    std::atomic<bool> deadline_fired_{};
    std::jthread deadline_thread_;
};

[[nodiscard]] DevicePreflightProbeError primitive_probe_error(
    PrimitiveError source,
    const bool deadline_fired,
    const std::stop_token cancellation) {
    auto code = DevicePreflightProbeErrorCode::ProtocolFailure;
    if (cancellation.stop_requested() ||
        source.code == PrimitiveErrorCode::Cancelled) {
        code = DevicePreflightProbeErrorCode::Cancelled;
    } else if (deadline_fired) {
        code = DevicePreflightProbeErrorCode::DeadlineExceeded;
    } else if (source.code == PrimitiveErrorCode::DeviceFail) {
        code = DevicePreflightProbeErrorCode::DeviceRejected;
    }
    return {
        .code = code,
        .message = std::move(source.message),
        .native_code = source.native_code,
        .outbound_certainty = source.outbound_certainty,
    };
}

[[nodiscard]] protocol::TransferCertainty aggregate_certainty(
    const protocol::TransferCertainty aggregate,
    const protocol::TransferCertainty next) noexcept {
    if (aggregate == protocol::TransferCertainty::PartialOrUnknown ||
        next == protocol::TransferCertainty::PartialOrUnknown) {
        return protocol::TransferCertainty::PartialOrUnknown;
    }
    if (aggregate == protocol::TransferCertainty::FullyTransferred ||
        next == protocol::TransferCertainty::FullyTransferred) {
        return protocol::TransferCertainty::FullyTransferred;
    }
    return protocol::TransferCertainty::NotTransferred;
}

}  // namespace

std::expected<std::unique_ptr<IDevicePreflightSessionOpener>,
              DevicePreflightOpenError>
make_libusb_device_preflight_session_opener(
    std::shared_ptr<transport::LibusbRuntime> runtime,
    std::shared_ptr<transport::BufferBudget> buffer_budget,
    const transport::TransferRingConfig data_ring,
    const protocol::SessionOptions session_options) noexcept {
    if (runtime == nullptr || !runtime->running() || buffer_budget == nullptr ||
        data_ring.chunk_size == 0U || data_ring.depth == 0U) {
        return std::unexpected(open_error(
            DevicePreflightOpenErrorCode::TransportFailure,
            "device connection requires a running libusb runtime"));
    }
    try {
        transport::UsbFastbootTransportOptions options;
        options.buffer_budget = std::move(buffer_budget);
        options.data_ring = data_ring;
        return std::unique_ptr<IDevicePreflightSessionOpener>(
            new LibusbDevicePreflightSessionOpener(
                std::move(runtime), std::move(options), session_options));
    } catch (const std::bad_alloc&) {
        return std::unexpected(open_error(
            DevicePreflightOpenErrorCode::ResourceExhausted,
            "unable to allocate the libusb device opener",
            LIBUSB_ERROR_NO_MEM));
    } catch (...) {
        return std::unexpected(open_error(
            DevicePreflightOpenErrorCode::UnexpectedFailure,
            "unable to create the libusb device opener",
            LIBUSB_ERROR_OTHER));
    }
}

std::expected<DevicePreflightProbeResult, DevicePreflightProbeError>
FastbootDevicePreflightProbe::probe(
    protocol::FastbootSession& session,
    const DevicePreflightTimePoint deadline,
    const std::stop_token cancellation) {
    auto certainty = protocol::TransferCertainty::NotTransferred;
    bool in_flight = false;
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
        PrimitiveService service(session);
        in_flight = true;
        auto product = service.getvar("product");
        in_flight = false;
        if (!product) {
            auto error = primitive_probe_error(
                std::move(product.error()), guard.deadline_fired(), cancellation);
            error.outbound_certainty = aggregate_certainty(
                certainty, error.outbound_certainty);
            return std::unexpected(std::move(error));
        }
        certainty = aggregate_certainty(certainty, product->outbound_certainty);
        if (product->terminal.payload.empty()) {
            return std::unexpected(DevicePreflightProbeError{
                .code = DevicePreflightProbeErrorCode::InvalidResponse,
                .message = "Fastboot getvar:product returned an empty identity",
                .outbound_certainty = certainty,
            });
        }
        if (cancellation.stop_requested() || guard.deadline_fired() ||
            DevicePreflightClock::now() >= deadline) {
            return std::unexpected(DevicePreflightProbeError{
                .code = cancellation.stop_requested()
                    ? DevicePreflightProbeErrorCode::Cancelled
                    : DevicePreflightProbeErrorCode::DeadlineExceeded,
                .message = cancellation.stop_requested()
                    ? "Fastboot identity probe was cancelled"
                    : "Fastboot identity probe deadline expired",
                .outbound_certainty = certainty,
            });
        }

        in_flight = true;
        auto mode = service.getvar("is-userspace");
        in_flight = false;
        auto observed_mode = FastbootUsbMode::Bootloader;
        if (!mode) {
            certainty = aggregate_certainty(
                certainty, mode.error().outbound_certainty);
            if (mode.error().code != PrimitiveErrorCode::DeviceFail) {
                auto error = primitive_probe_error(
                    std::move(mode.error()), guard.deadline_fired(), cancellation);
                error.outbound_certainty = certainty;
                return std::unexpected(std::move(error));
            }
        } else {
            certainty = aggregate_certainty(certainty, mode->outbound_certainty);
            if (mode->terminal.payload == "yes") {
                observed_mode = FastbootUsbMode::Fastbootd;
            } else if (mode->terminal.payload != "no") {
                return std::unexpected(DevicePreflightProbeError{
                    .code = DevicePreflightProbeErrorCode::InvalidResponse,
                    .message = "Fastboot is-userspace must be exactly 'yes' or 'no'",
                    .outbound_certainty = certainty,
                });
            }
        }
        if (cancellation.stop_requested() || guard.deadline_fired() ||
            DevicePreflightClock::now() >= deadline) {
            return std::unexpected(DevicePreflightProbeError{
                .code = cancellation.stop_requested()
                    ? DevicePreflightProbeErrorCode::Cancelled
                    : DevicePreflightProbeErrorCode::DeadlineExceeded,
                .message = cancellation.stop_requested()
                    ? "Fastboot identity probe was cancelled"
                    : "Fastboot identity probe deadline expired",
                .outbound_certainty = certainty,
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
            .outbound_certainty = in_flight
                ? protocol::TransferCertainty::PartialOrUnknown
                : certainty,
        });
    } catch (...) {
        return std::unexpected(DevicePreflightProbeError{
            .code = DevicePreflightProbeErrorCode::UnexpectedFailure,
            .outbound_certainty = in_flight
                ? protocol::TransferCertainty::PartialOrUnknown
                : certainty,
        });
    }
}

}  // namespace kairosboot::fastboot
