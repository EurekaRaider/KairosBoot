// SPDX-License-Identifier: MIT

#include "src/api/fleet_run.hpp"
#include "src/kairosboot_internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class ScriptMode {
    Success = 0,
    Failure = 1,
    WaitForCancel = 2,
};

enum class ProductionScriptMode {
    Success = 0,
    NoDevice = 1,
    Ambiguous = 2,
    ProductMismatch = 3,
    ContinueFailure = 4,
};

struct ProductionTransportState final {
    std::mutex mutex;
    std::string pending_response;
    std::shared_ptr<kairosboot::transport::TransferPermitProvider> permits;
    kairosboot::transport::TransferRingConfig ring{};
    bool fail_flash{};
    bool cancelled{};
    std::size_t permit_bind_count{};
    std::size_t payload_count{};
};

std::mutex g_production_mutex;
std::vector<std::shared_ptr<ProductionTransportState>> g_production_states;
std::size_t g_production_dependency_calls{};

class ProductionTransport final
    : public kairosboot::protocol::ITransportSession,
      public kairosboot::protocol::IStreamingTransportSession,
      public kairosboot::transport::ITransferPermitConfigurableTransport {
public:
    explicit ProductionTransport(
        std::shared_ptr<ProductionTransportState> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] kairosboot::protocol::TransferResult write(
        const std::span<const std::byte> bytes,
        std::chrono::milliseconds) override {
        const std::string command(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::scoped_lock lock(state_->mutex);
        if (state_->cancelled) {
            return transfer_result(
                kairosboot::protocol::TransportStatus::Cancelled, 0U);
        }
        if (command.starts_with("download:")) {
            state_->pending_response = "DATA" + command.substr(9U);
        } else if (command == "getvar:has-slot:system") {
            state_->pending_response = "OKAYno";
        } else if (command == "getvar:max-download-size") {
            state_->pending_response = "OKAY1048576";
        } else if (command == "flash:system" && state_->fail_flash) {
            state_->pending_response = "FAILscripted flash rejection";
        } else {
            state_->pending_response = "OKAYyes";
        }
        return transfer_result(
            kairosboot::protocol::TransportStatus::Ok, bytes.size());
    }

    [[nodiscard]] kairosboot::protocol::TransferResult read(
        const std::span<std::byte> destination,
        std::chrono::milliseconds) override {
        std::scoped_lock lock(state_->mutex);
        if (state_->cancelled) {
            return transfer_result(
                kairosboot::protocol::TransportStatus::Cancelled, 0U);
        }
        if (state_->pending_response.empty()) {
            return transfer_result(
                kairosboot::protocol::TransportStatus::IoError, 0U);
        }
        const auto count =
            std::min(destination.size(), state_->pending_response.size());
        std::memcpy(destination.data(), state_->pending_response.data(), count);
        const bool truncated = count != state_->pending_response.size();
        state_->pending_response.clear();
        auto result = transfer_result(
            kairosboot::protocol::TransportStatus::Ok, count);
        result.truncated = truncated;
        return result;
    }

    [[nodiscard]] kairosboot::protocol::TransferResult read_data(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return transfer_result(
            kairosboot::protocol::TransportStatus::IoError, 0U);
    }

    [[nodiscard]] kairosboot::protocol::TransferResult write_source(
        const std::shared_ptr<kairosboot::protocol::ITransferSource> source,
        const std::chrono::milliseconds timeout,
        const kairosboot::protocol::TransferProgressObserver& observer) override {
        if (source == nullptr ||
            source->size() > std::numeric_limits<std::size_t>::max()) {
            return transfer_result(
                kairosboot::protocol::TransportStatus::IoError, 0U);
        }
        std::shared_ptr<kairosboot::transport::TransferPermitProvider> permits;
        {
            std::scoped_lock lock(state_->mutex);
            permits = state_->permits;
        }
        std::uint64_t offset{};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (offset < source->size()) {
            {
                std::scoped_lock lock(state_->mutex);
                if (state_->cancelled) {
                    return transfer_result(
                        kairosboot::protocol::TransportStatus::Cancelled,
                        static_cast<std::size_t>(offset));
                }
            }
            const auto remaining = static_cast<std::size_t>(
                source->size() - offset);
            if (permits == nullptr) {
                std::vector<std::byte> bytes(remaining);
                if (!source->read_exact(offset, bytes)) {
                    return transfer_result(
                        kairosboot::protocol::TransportStatus::IoError,
                        static_cast<std::size_t>(offset));
                }
                offset = source->size();
            } else {
                auto permit = permits->try_acquire(remaining);
                if (!permit) {
                    const auto generation = permits->readiness_generation();
                    if (permits->wait_for_ready(generation, deadline) !=
                        kairosboot::transport::TransferPermitWaitResult::ready) {
                        return transfer_result(
                            kairosboot::protocol::TransportStatus::Cancelled,
                            static_cast<std::size_t>(offset));
                    }
                    continue;
                }
                if (!source->read_exact(offset, permit->bytes())) {
                    permit->settle(
                        kairosboot::transport::TransferPermitSettlement::
                            not_submitted);
                    return transfer_result(
                        kairosboot::protocol::TransportStatus::IoError,
                        static_cast<std::size_t>(offset));
                }
                offset += permit->size();
                permit->settle(
                    kairosboot::transport::TransferPermitSettlement::
                        fully_transferred);
            }
            if (observer &&
                observer(offset, source->size()) ==
                    kairosboot::protocol::TransferProgressAction::cancel) {
                return transfer_result(
                    kairosboot::protocol::TransportStatus::Cancelled,
                    static_cast<std::size_t>(offset));
            }
        }
        {
            std::scoped_lock lock(state_->mutex);
            ++state_->payload_count;
            state_->pending_response = "OKAY";
        }
        return transfer_result(
            kairosboot::protocol::TransportStatus::Ok,
            static_cast<std::size_t>(source->size()));
    }

    [[nodiscard]] bool configure_transfer_permits(
        std::shared_ptr<kairosboot::transport::TransferPermitProvider> provider,
        const kairosboot::transport::TransferRingConfig config) noexcept override {
        if (provider == nullptr || config.chunk_size == 0U ||
            config.depth == 0U) {
            return false;
        }
        std::scoped_lock lock(state_->mutex);
        state_->permits = std::move(provider);
        state_->ring = config;
        ++state_->permit_bind_count;
        return true;
    }

    void request_cancel() noexcept override {
        std::shared_ptr<kairosboot::transport::TransferPermitProvider> permits;
        {
            std::scoped_lock lock(state_->mutex);
            state_->cancelled = true;
            permits = state_->permits;
        }
        if (permits != nullptr) {
            permits->cancel_wait();
        }
    }

    void close() noexcept override { request_cancel(); }

private:
    [[nodiscard]] static kairosboot::protocol::TransferResult transfer_result(
        const kairosboot::protocol::TransportStatus status,
        const std::size_t transferred) {
        return {
            .status = status,
            .transferred = transferred,
            .certainty = transferred == 0U &&
                    status != kairosboot::protocol::TransportStatus::Ok
                ? kairosboot::protocol::TransferCertainty::NotTransferred
                : status == kairosboot::protocol::TransportStatus::Ok
                    ? kairosboot::protocol::TransferCertainty::FullyTransferred
                    : kairosboot::protocol::TransferCertainty::PartialOrUnknown,
            .truncated = false,
            .detail = {},
            .native_code = 0,
        };
    }

    std::shared_ptr<ProductionTransportState> state_;
};

[[nodiscard]] kairosboot::transport::UsbDeviceInfo production_device(
    const std::size_t index,
    const std::size_t controller,
    std::string serial) {
    const auto address = static_cast<std::uint8_t>(index + 1U);
    kairosboot::transport::UsbDeviceInfo result{
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .bus_number = 1U,
        .device_address = address,
        .backend_session_id = address,
        .configuration_value = 1U,
        .port_path = {address},
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
    result.linux_topology = kairosboot::transport::LinuxUsbTopology{
        .physical_port_path = "usb:1-" + std::to_string(index + 1U),
        .root_controller_id =
            "linux-sysfs:/controller" + std::to_string(controller),
        .hub_port_chain = result.port_path,
        .vendor_id = result.vendor_id,
        .product_id = result.product_id,
        .bus_number = result.bus_number,
        .device_address = result.device_address,
        .serial_utf8 = result.serial_utf8,
        .product_utf8 = std::nullopt,
        .sysfs_device_path =
            "/sys/bus/usb/devices/1-" + std::to_string(index + 1U),
    };
    return result;
}

[[nodiscard]] kairosboot::fleet::DevicePreflightUsbIdentity
production_identity(const kairosboot::transport::UsbDeviceInfo& device) {
    return {
        .physical_port_path = device.linux_topology->physical_port_path,
        .root_controller_id = device.linux_topology->root_controller_id,
        .hub_port_chain = device.port_path,
        .bus_number = device.bus_number,
        .device_address = device.device_address,
        .backend_session_id = device.backend_session_id,
        .serial = device.serial_utf8,
        .usb_fingerprint = {
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
        },
        .platform_attestation = *device.linux_topology,
    };
}

class ProductionOpener final
    : public kairosboot::fleet::IDevicePreflightSessionOpener {
public:
    explicit ProductionOpener(const bool fail_first) noexcept
        : fail_first_(fail_first) {}

    [[nodiscard]] std::expected<
        kairosboot::fleet::OpenedDevicePreflightSession,
        kairosboot::fleet::DevicePreflightOpenError>
    open(const kairosboot::transport::UsbDeviceInfo& device,
         kairosboot::fleet::DevicePreflightTimePoint,
         std::stop_token) override {
        auto state = std::make_shared<ProductionTransportState>();
        state->fail_flash = fail_first_ && device.device_address == 1U;
        {
            std::scoped_lock lock(g_production_mutex);
            g_production_states.push_back(state);
        }
        return kairosboot::fleet::OpenedDevicePreflightSession{
            .verified_usb_identity = production_identity(device),
            .session = std::make_unique<kairosboot::protocol::FastbootSession>(
                std::make_unique<ProductionTransport>(std::move(state))),
        };
    }

private:
    bool fail_first_{};
};

class ProductionProbe final : public kairosboot::fleet::IDevicePreflightProbe {
public:
    explicit ProductionProbe(const bool mismatch) noexcept
        : mismatch_(mismatch) {}

    [[nodiscard]] std::expected<
        kairosboot::fleet::DevicePreflightProbeResult,
        kairosboot::fleet::DevicePreflightProbeError>
    probe(kairosboot::protocol::FastbootSession&,
          kairosboot::fleet::DevicePreflightTimePoint,
          std::stop_token) override {
        return kairosboot::fleet::DevicePreflightProbeResult{
            .product = mismatch_ ? "wrong-product" : "product_a",
            .mode = kairosboot::fastboot::FastbootUsbMode::Bootloader,
            .product_query_completed = true,
            .mode_query_completed = true,
        };
    }

private:
    bool mismatch_{};
};

[[nodiscard]] kairosboot::fleet::ReportDeviceSpec report_spec() {
    return {
        .identifier = "usb:1-1",
        .serial = std::string{"SERIAL-01"},
        .usb_path = std::string{"usb:1-1"},
        .target = "product-a",
        .expected_product = "product_a",
        .observed_product = std::string{"product_a"},
        .steps = {
            kairosboot::fleet::ReportStepSpec{
                .operation = kairosboot::fleet::ReportOperation::Flash,
                .partition = std::string{"system"},
                .artifact = std::string{"system"},
                .slot = std::nullopt,
                .reboot_target = std::nullopt,
                .oem_command = std::nullopt,
                .bytes_total = 4096U,
            },
        },
    };
}

[[nodiscard]] kairosboot::fleet::FleetActorExecutionError cancelled_error(
    const std::size_t device_index) {
    return {
        .kind = kairosboot::fleet::FleetActorExecutionErrorKind::Cancelled,
        .message = "scripted fleet device cancelled",
        .device_index = device_index,
        .step_index = 0U,
        .completed_steps = 0U,
        .completed_data_bytes = 0U,
        .completed_child_tasks_in_step = 0U,
        .total_child_tasks_in_step = 1U,
        .device_error = std::nullopt,
    };
}

}  // namespace

// This test target compiles fleet_run.cpp directly so it can install internal
// scripted seams, while kb_context_create is provided by the shared library.
// Production links the real bridge from kairosboot.cpp; the scripted tests
// replace device dependencies before this fallback could be reached.
namespace kairosboot::api {

std::expected<std::shared_ptr<transport::LibusbRuntime>, OperationErrorPayload>
acquire_fleet_usb_runtime(kb_context_t&) {
    return std::unexpected(OperationErrorPayload{
        .status = KB_E_INTERNAL,
        .message = "scripted fleet test did not install device dependencies",
        .native_code = 0,
        .transfer_state = KB_TRANSFER_NOT_SENT,
        .device_identifier = {},
        .device_message = {},
        .command_messages = {},
        .inbound_expected = std::nullopt,
        .inbound_transferred = 0U,
        .inbound_transfer_state = KB_TRANSFER_NOT_SENT,
        .session_poisoned = false,
    });
}

}  // namespace kairosboot::api

extern "C" void kb_test_set_fleet_script(const int mode) {
    const auto script = static_cast<ScriptMode>(mode);
    kairosboot::api::set_fleet_run_prepare_factory(
        [script](const kairosboot::fleet::JobPlan&, const std::stop_token token)
            -> std::expected<kairosboot::api::FleetRunPrepared,
                             kairosboot::api::FleetRunPrepareError> {
            kairosboot::fleet::FleetCoordinatorDeviceExecutor executor =
                [script, token](
                    const std::size_t device_index,
                    const kairosboot::fastboot::UpdateOperationContext& context,
                    const kairosboot::fleet::FleetActorExecutionObserver& observer)
                -> std::expected<
                    kairosboot::fleet::FleetActorDeviceExecution,
                    kairosboot::fleet::FleetActorExecutionError> {
                if (observer) {
                    observer({
                        .kind = kairosboot::fleet::
                            FleetActorExecutionEventKind::DeviceStarted,
                        .device_index = device_index,
                        .step_index = std::nullopt,
                        .completed_steps = 0U,
                        .completed_data_bytes = 0U,
                    });
                }
                if (script == ScriptMode::WaitForCancel) {
                    while (!context.cancellation.stop_requested()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds{1});
                    }
                    return std::unexpected(cancelled_error(device_index));
                }
                if (context.cancellation.stop_requested() ||
                    token.stop_requested()) {
                    return std::unexpected(cancelled_error(device_index));
                }
                if (script == ScriptMode::Failure) {
                    return std::unexpected(
                        kairosboot::fleet::FleetActorExecutionError{
                            .kind = kairosboot::fleet::
                                FleetActorExecutionErrorKind::DeviceTaskFailed,
                            .message = "scripted device rejected flash",
                            .device_index = device_index,
                            .step_index = 0U,
                            .completed_steps = 0U,
                            .completed_data_bytes = 0U,
                            .completed_child_tasks_in_step = 0U,
                            .total_child_tasks_in_step = 1U,
                            .device_error = std::nullopt,
                        });
                }
                if (observer) {
                    observer({
                        .kind = kairosboot::fleet::
                            FleetActorExecutionEventKind::DeviceCompleted,
                        .device_index = device_index,
                        .step_index = 0U,
                        .completed_steps = 1U,
                        .completed_data_bytes = 4096U,
                    });
                }
                return kairosboot::fleet::FleetActorDeviceExecution{
                    .completed_steps = 1U,
                    .completed_data_bytes = 4096U,
                };
            };
            return kairosboot::api::FleetRunPrepared{
                .report_specs = {report_spec()},
                .executor = std::move(executor),
                .actor_batch = {},
            };
        });
}

extern "C" void kb_test_set_fleet_production_script(const int mode) {
    const auto script = static_cast<ProductionScriptMode>(mode);
    {
        std::scoped_lock lock(g_production_mutex);
        g_production_states.clear();
        g_production_dependency_calls = 0U;
    }
    kairosboot::api::set_fleet_run_prepare_factory({});
    kairosboot::api::set_fleet_run_device_dependencies_factory(
        [script](kb_context_t&,
                 const kairosboot::fleet::JobPlan& plan,
                 std::chrono::steady_clock::time_point,
                 const std::stop_token cancellation)
            -> std::expected<
                kairosboot::api::FleetRunDeviceDependencies,
                kairosboot::api::FleetRunPrepareError> {
            {
                std::scoped_lock lock(g_production_mutex);
                ++g_production_dependency_calls;
            }
            if (cancellation.stop_requested()) {
                return std::unexpected(kairosboot::api::FleetRunPrepareError{
                    .status = KB_E_CANCELLED,
                    .message = "scripted production dependencies cancelled",
                });
            }

            std::size_t device_count{};
            for (const auto& target : plan.manifest().targets) {
                device_count += target.selector.serials.size();
                device_count += target.selector.usb_paths.size();
            }
            std::vector<kairosboot::transport::UsbDeviceInfo> snapshot;
            if (script == ProductionScriptMode::Ambiguous) {
                snapshot.push_back(production_device(0U, 0U, "SERIAL-000"));
                snapshot.push_back(production_device(1U, 1U, "SERIAL-000"));
            } else if (script != ProductionScriptMode::NoDevice) {
                snapshot.reserve(device_count);
                for (std::size_t index = 0U; index < device_count; ++index) {
                    auto serial = std::string{"SERIAL-"};
                    if (index < 10U) {
                        serial.append("00");
                    } else if (index < 100U) {
                        serial.push_back('0');
                    }
                    serial.append(std::to_string(index));
                    snapshot.push_back(production_device(
                        index, index % 2U, std::move(serial)));
                }
            }

            kairosboot::fleet::FleetExecutionRuntime runtime{
                .buffer_budget =
                    std::make_shared<kairosboot::transport::BufferBudget>(64U),
                .data_ring = {.chunk_size = 4U, .depth = 2U},
                .reconnect_factory = {},
                .reconnect_options = {},
            };
            return kairosboot::api::FleetRunDeviceDependencies{
                .snapshot = std::move(snapshot),
                .opener = std::make_unique<ProductionOpener>(
                    script == ProductionScriptMode::ContinueFailure),
                .probe = std::make_unique<ProductionProbe>(
                    script == ProductionScriptMode::ProductMismatch),
                .execution_runtime = std::move(runtime),
            };
        });
}

extern "C" size_t kb_test_fleet_dependency_calls(void) {
    std::scoped_lock lock(g_production_mutex);
    return g_production_dependency_calls;
}

extern "C" size_t kb_test_fleet_permit_bind_count(void) {
    std::scoped_lock lock(g_production_mutex);
    std::size_t result{};
    for (const auto& state : g_production_states) {
        std::scoped_lock state_lock(state->mutex);
        result += state->permit_bind_count;
    }
    return result;
}

extern "C" size_t kb_test_fleet_payload_count(void) {
    std::scoped_lock lock(g_production_mutex);
    std::size_t result{};
    for (const auto& state : g_production_states) {
        std::scoped_lock state_lock(state->mutex);
        result += state->payload_count;
    }
    return result;
}
