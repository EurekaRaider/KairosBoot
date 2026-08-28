// SPDX-License-Identifier: MIT
#include "src/fastboot/libusb_reconnect_adapters.hpp"
#include "src/fastboot/primitive_update_device.hpp"
#include "src/fastboot/primitive_service.hpp"

#include "src/fleet/job_plan.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::fastboot::FastbootUsbMode;
using kairosboot::fastboot::LibusbReconnectAdapter;
using kairosboot::fastboot::PrimitiveErrorCode;
using kairosboot::fastboot::PrimitiveService;
using kairosboot::fastboot::PreparedReconnectBinding;
using kairosboot::fastboot::PreparedReconnectBindingErrorCode;
using kairosboot::fastboot::ReconnectCandidate;
using kairosboot::fastboot::ReconnectCoordinator;
using kairosboot::fastboot::ReconnectErrorCode;
using kairosboot::fastboot::ReconnectObservation;
using kairosboot::fastboot::ReconnectOptions;
using kairosboot::fastboot::ReconnectTarget;
using kairosboot::fastboot::ReconnectUsbFingerprintPolicy;
using kairosboot::fastboot::SteadyReconnectWaiter;
using kairosboot::fastboot::make_prepared_reconnect_binding;
using kairosboot::fastboot::bind_initial_libusb_update_session;
using kairosboot::fleet::DevicePreflightOpenError;
using kairosboot::fleet::DevicePreflightProbeError;
using kairosboot::fleet::DevicePreflightProbeResult;
using kairosboot::fleet::DevicePreflightTimePoint;
using kairosboot::fleet::DevicePreflightUsbFingerprint;
using kairosboot::fleet::DevicePreflightUsbIdentity;
using kairosboot::fleet::FlashJobManifest;
using kairosboot::fleet::FastbootDevicePreflightProbe;
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
using kairosboot::fleet::make_job_plan;
using kairosboot::fleet::make_libusb_device_preflight_session_opener;
using kairosboot::fleet::preflight_fleet_devices;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::SessionOptions;
using kairosboot::protocol::SessionState;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferResult;
using kairosboot::protocol::TransportStatus;
using kairosboot::transport::LibusbFunctions;
using kairosboot::transport::LibusbRuntime;
using kairosboot::transport::LibusbRuntimeErrorKind;
using kairosboot::transport::LinuxUsbTopology;
using kairosboot::transport::MacUsbTopology;
using kairosboot::transport::MacUsbTopologyDeviceQuery;
using kairosboot::transport::MacUsbTopologyDeviceResult;
using kairosboot::transport::MacUsbTopologyError;
using kairosboot::transport::BufferBudget;
using kairosboot::transport::TransferRingConfig;
using kairosboot::transport::UsbDeviceInfo;
using kairosboot::transport::UsbInterfaceFilter;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            throw TestFailure(std::string{"line "} +                          \
                              std::to_string(__LINE__) + ": " #expression);   \
        }                                                                       \
    } while (false)

template <typename Predicate>
void wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw TestFailure("timed out waiting for fake libusb event");
        }
        std::this_thread::sleep_for(1ms);
    }
}

template <typename Value>
concept PublicBindingConstructor = requires { Value{ReconnectTarget{}}; };

static_assert(!std::is_copy_constructible_v<PreparedReconnectBinding>);
static_assert(!PublicBindingConstructor<PreparedReconnectBinding>);

class FakeLibusb final : public std::enable_shared_from_this<FakeLibusb> {
public:
    struct Event final {
        libusb_transfer* transfer{};
        libusb_transfer_status status{LIBUSB_TRANSFER_ERROR};
        int actual_length{};
    };

    FakeLibusb() {
        version_.major = 1;
        version_.minor = 0;
        version_.micro = 30;
        descriptor_.idVendor = 0x18D1U;
        descriptor_.idProduct = 0x4EE0U;
        descriptor_.iSerialNumber = 1U;
        descriptor_.bNumConfigurations = 1U;
        endpoints_[0].bEndpointAddress = 0x01U;
        endpoints_[0].bmAttributes = LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
        endpoints_[0].wMaxPacketSize = 512U;
        endpoints_[1].bEndpointAddress = 0x81U;
        endpoints_[1].bmAttributes = LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK;
        endpoints_[1].wMaxPacketSize = 512U;
        alternate_.bInterfaceNumber = 0U;
        alternate_.bAlternateSetting = 0U;
        alternate_.bNumEndpoints = 2U;
        alternate_.bInterfaceClass = 0xFFU;
        alternate_.bInterfaceSubClass = 0x42U;
        alternate_.bInterfaceProtocol = 0x03U;
        alternate_.endpoint = endpoints_;
        interface_.altsetting = &alternate_;
        interface_.num_altsetting = 1;
        config_.bConfigurationValue = 1U;
        config_.bNumInterfaces = 1U;
        config_.interface = &interface_;
        rebuild_device_list();
    }

    [[nodiscard]] LibusbFunctions functions() {
        const auto self = shared_from_this();
        LibusbFunctions table;
        table.get_version = [self] { return &self->version_; };
        table.init = [self](libusb_context** context) {
            *context = self->context();
            return LIBUSB_SUCCESS;
        };
        table.exit = [self](libusb_context*) { ++self->exit_calls; };
        table.handle_events = [self](libusb_context*, timeval* timeout, int*) {
            return self->handle_one_event(timeout);
        };
        table.interrupt_events = [self](libusb_context*) {
            {
                const std::lock_guard lock(self->mutex_);
                self->interrupted_ = true;
            }
            self->condition_.notify_all();
        };
        table.get_device_list = [self](libusb_context*, libusb_device*** list) {
            *list = self->device_list_.data();
            return static_cast<ssize_t>(self->device_count);
        };
        table.free_device_list = [](libusb_device**, int) {};
        table.get_device_descriptor =
            [self](libusb_device*, libusb_device_descriptor* descriptor) {
                {
                    std::unique_lock lock(self->descriptor_mutex_);
                    if (self->block_descriptor) {
                        self->descriptor_entered = true;
                        self->descriptor_condition_.notify_all();
                        self->descriptor_condition_.wait(lock, [self] {
                            return self->release_descriptor;
                        });
                    }
                }
                *descriptor = self->descriptor_;
                return LIBUSB_SUCCESS;
            };
        table.get_active_config_descriptor =
            [self](libusb_device*, libusb_config_descriptor** config) {
                *config = &self->config_;
                return LIBUSB_SUCCESS;
            };
        table.get_config_descriptor =
            [self](libusb_device*, std::uint8_t,
                   libusb_config_descriptor** config) {
                *config = &self->config_;
                return LIBUSB_SUCCESS;
            };
        table.free_config_descriptor = [](libusb_config_descriptor*) {};
        table.get_bus_number = [self](libusb_device*) { return self->bus; };
        table.get_device_address =
            [self](libusb_device* device) {
                return static_cast<std::uint8_t>(
                    self->address + self->device_index(device));
            };
        table.get_session_data =
            [self](libusb_device* device) {
                return self->session +
                       static_cast<unsigned long>(self->device_index(device));
            };
        table.get_port_numbers =
            [self](libusb_device* device, std::uint8_t* output,
                   const int length) -> int {
                const auto& ports = self->device_index(device) == 0U
                    ? self->ports
                    : self->second_ports;
                if (length < static_cast<int>(ports.size())) {
                    return LIBUSB_ERROR_OVERFLOW;
                }
                std::copy(ports.begin(), ports.end(), output);
                return static_cast<int>(ports.size());
            };
        table.get_device_string =
            [self](libusb_device* device,
                   libusb_device_string_type,
                   char* output,
                   const int length) -> int {
                const auto& serial = self->device_index(device) == 0U
                    ? self->serial
                    : self->second_serial;
                if (length <= static_cast<int>(serial.size())) {
                    return LIBUSB_ERROR_OVERFLOW;
                }
                std::memcpy(output,
                            serial.c_str(),
                            serial.size() + 1U);
                return static_cast<int>(serial.size() + 1U);
            };
        table.open = [self](libusb_device* device,
                            libusb_device_handle** handle) {
            ++self->open_calls;
            *handle = self->handle(self->device_index(device));
            return LIBUSB_SUCCESS;
        };
        table.get_device = [self](libusb_device_handle* handle) {
            return self->device(self->handle_index(handle));
        };
        table.close = [self](libusb_device_handle*) { ++self->close_calls; };
        table.get_configuration = [](libusb_device_handle*, int* value) {
            *value = 1;
            return LIBUSB_SUCCESS;
        };
        table.set_configuration = [](libusb_device_handle*, int) {
            return LIBUSB_SUCCESS;
        };
        table.claim_interface = [self](libusb_device_handle*, int) {
            ++self->claim_calls;
            return LIBUSB_SUCCESS;
        };
        table.release_interface = [self](libusb_device_handle*, int) {
            ++self->release_calls;
            return LIBUSB_SUCCESS;
        };
        table.set_interface_alt_setting = [](libusb_device_handle*, int, int) {
            return LIBUSB_SUCCESS;
        };
        table.alloc_transfer = [](int) {
            return static_cast<libusb_transfer*>(
                std::calloc(1U, sizeof(libusb_transfer)));
        };
        table.submit_transfer = [self](libusb_transfer* transfer) {
            return self->submit(transfer);
        };
        table.cancel_transfer = [self](libusb_transfer* transfer) {
            return self->cancel(transfer);
        };
        table.free_transfer = [](libusb_transfer* transfer) {
            std::free(transfer);
        };
        table.pin_current_module = [] { return true; };
        table.module_pin_failure = [] {};
        return table;
    }

    void rebuild_device_list() {
        device_list_.fill(nullptr);
        for (std::size_t index = 0U; index < device_count; ++index) {
            device_list_[index] = device(index);
        }
    }

    void set_product_id(const std::uint16_t product_id) noexcept {
        descriptor_.idProduct = product_id;
    }

    void set_interface_number(const std::uint8_t interface_number) noexcept {
        alternate_.bInterfaceNumber = interface_number;
    }

    [[nodiscard]] std::size_t command_count() const {
        const std::lock_guard lock(mutex_);
        return commands.size();
    }

    void wait_for_descriptor() {
        std::unique_lock lock(descriptor_mutex_);
        CHECK(descriptor_condition_.wait_for(
            lock, 2s, [this] { return descriptor_entered; }));
    }

    void unblock_descriptor() {
        {
            const std::lock_guard lock(descriptor_mutex_);
            release_descriptor = true;
        }
        descriptor_condition_.notify_all();
    }

    std::uint8_t bus{2U};
    std::uint8_t address{5U};
    unsigned long session{0x101UL};
    std::vector<std::uint8_t> ports{3U, 4U};
    std::vector<std::uint8_t> second_ports{3U, 5U};
    std::string serial{"SERIAL"};
    std::string second_serial{"SERIAL-B"};
    std::string product_response{"OKAYproduct-a"};
    std::string mode_response{"OKAYyes"};
    std::string serial_response{"OKAYSERIAL"};
    std::size_t device_count{1U};
    bool stall_in{};
    bool block_descriptor{};
    bool descriptor_entered{};
    bool release_descriptor{};
    std::atomic<int> open_calls{};
    std::atomic<int> claim_calls{};
    std::atomic<int> release_calls{};
    std::atomic<int> close_calls{};
    std::atomic<int> exit_calls{};
    std::vector<std::string> commands;

private:
    [[nodiscard]] libusb_context* context() noexcept {
        return reinterpret_cast<libusb_context*>(&context_storage_);
    }
    [[nodiscard]] libusb_device* device(
        const std::size_t index = 0U) noexcept {
        return reinterpret_cast<libusb_device*>(&device_storage_[index]);
    }
    [[nodiscard]] libusb_device_handle* handle(
        const std::size_t index = 0U) noexcept {
        return reinterpret_cast<libusb_device_handle*>(&handle_storage_[index]);
    }
    [[nodiscard]] std::size_t device_index(
        const libusb_device* value) noexcept {
        for (std::size_t index = 0U; index < device_storage_.size(); ++index) {
            if (value == device(index)) {
                return index;
            }
        }
        return 0U;
    }
    [[nodiscard]] std::size_t handle_index(
        const libusb_device_handle* value) noexcept {
        for (std::size_t index = 0U; index < handle_storage_.size(); ++index) {
            if (value == handle(index)) {
                return index;
            }
        }
        return 0U;
    }

    [[nodiscard]] int submit(libusb_transfer* transfer) {
        std::lock_guard lock(mutex_);
        active_.insert(transfer);
        const bool inbound =
            (transfer->endpoint & LIBUSB_ENDPOINT_IN) == LIBUSB_ENDPOINT_IN;
        if (!inbound) {
            const std::string command(
                reinterpret_cast<const char*>(transfer->buffer),
                static_cast<std::size_t>(transfer->length));
            commands.push_back(command);
            if (command == "getvar:product") {
                pending_response_ = product_response;
            } else if (command == "getvar:is-userspace") {
                pending_response_ = mode_response;
            } else if (command == "getvar:serialno") {
                pending_response_ = serial_response;
            } else {
                pending_response_ = "FAILunexpected command";
            }
            events_.push_back(Event{
                transfer, LIBUSB_TRANSFER_COMPLETED, transfer->length});
            condition_.notify_all();
            return LIBUSB_SUCCESS;
        }
        if (stall_in) {
            return LIBUSB_SUCCESS;
        }
        const auto bytes = std::min(
            pending_response_.size(),
            static_cast<std::size_t>(transfer->length));
        std::memcpy(transfer->buffer, pending_response_.data(), bytes);
        events_.push_back(Event{
            transfer,
            LIBUSB_TRANSFER_COMPLETED,
            static_cast<int>(bytes),
        });
        condition_.notify_all();
        return LIBUSB_SUCCESS;
    }

    [[nodiscard]] int cancel(libusb_transfer* transfer) {
        std::lock_guard lock(mutex_);
        if (!active_.contains(transfer)) {
            return LIBUSB_ERROR_NOT_FOUND;
        }
        if (std::ranges::any_of(events_, [transfer](const Event& event) {
                return event.transfer == transfer;
            })) {
            // libusb invokes one callback per submission. If completion is
            // already queued, cancellation loses the race and must not create
            // a second callback for the same transfer.
            return LIBUSB_ERROR_NOT_FOUND;
        }
        events_.push_back(
            Event{transfer, LIBUSB_TRANSFER_CANCELLED, 0});
        condition_.notify_all();
        return LIBUSB_SUCCESS;
    }

    [[nodiscard]] int handle_one_event(timeval* timeout) {
        std::unique_lock lock(mutex_);
        const auto duration = timeout == nullptr
            ? 100ms
            : std::chrono::seconds(timeout->tv_sec) +
                std::chrono::microseconds(timeout->tv_usec);
        condition_.wait_for(lock, duration, [this] {
            return interrupted_ || !events_.empty();
        });
        if (interrupted_) {
            interrupted_ = false;
            return LIBUSB_ERROR_INTERRUPTED;
        }
        if (events_.empty()) {
            return LIBUSB_SUCCESS;
        }
        const auto event = events_.front();
        events_.pop_front();
        active_.erase(event.transfer);
        lock.unlock();
        event.transfer->status = event.status;
        event.transfer->actual_length = event.actual_length;
        event.transfer->callback(event.transfer);
        return LIBUSB_SUCCESS;
    }

    std::byte context_storage_{};
    std::array<std::byte, 4U> device_storage_{};
    std::array<std::byte, 4U> handle_storage_{};
    libusb_version version_{};
    libusb_device_descriptor descriptor_{};
    libusb_endpoint_descriptor endpoints_[2]{};
    libusb_interface_descriptor alternate_{};
    libusb_interface interface_{};
    libusb_config_descriptor config_{};
    std::array<libusb_device*, 4U> device_list_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool interrupted_{};
    std::deque<Event> events_;
    std::unordered_set<libusb_transfer*> active_;
    std::string pending_response_;
    std::mutex descriptor_mutex_;
    std::condition_variable descriptor_condition_;
};

[[nodiscard]] std::shared_ptr<LibusbRuntime> create_runtime(
    const std::shared_ptr<FakeLibusb>& fake) {
    auto runtime = LibusbRuntime::create(fake->functions());
    CHECK(runtime.has_value());
    return *runtime;
}

[[nodiscard]] std::shared_ptr<LibusbRuntime> create_runtime(
    LibusbFunctions functions) {
    auto runtime = LibusbRuntime::create(std::move(functions));
    CHECK(runtime.has_value());
    return *runtime;
}

// Fleet preflight normalization requires one complete platform topology per
// enumerated device. Mirror the runtime's fake macOS resolver so the
// LibusbDevicePreflightSessionOpener can open the synthetic device.
[[nodiscard]] LibusbFunctions topology_functions(
    const std::shared_ptr<FakeLibusb>& fake) {
    auto functions = fake->functions();
    functions.resolve_macos_topology =
        [](const std::span<const MacUsbTopologyDeviceQuery> devices,
           const auto, const std::stop_token) {
            const auto& query = devices.front().interfaces.front();
            return std::expected<std::vector<MacUsbTopologyDeviceResult>,
                                 MacUsbTopologyError>{
                std::vector<MacUsbTopologyDeviceResult>{
                    std::vector<MacUsbTopology>{MacUsbTopology{
                        .physical_port_path = "usb:2-3.4",
                        .root_controller_id =
                            "macos-iokit:0000000000000011",
                        .hub_port_chain = query.port_numbers,
                        .registry_entry_id = 0x21U,
                        .session_id = query.session_id,
                        .interface_registry_entry_id = 0x41U,
                        .location_id = 0x02340000U,
                        .vendor_id = query.vendor_id,
                        .product_id = query.product_id,
                        .bus_number = query.bus_number,
                        .device_address = query.device_address,
                        .interface_fingerprint = query.interface_fingerprint,
                        .serial_utf8 = query.serial_utf8,
                        .product_utf8 = std::nullopt,
                        .registry_path = "IOService:/USB/device",
                        .interface_registry_path =
                            "IOService:/USB/device/interface",
                        .root_controller_registry_path =
                            "IOService:/USB/controller",
                    }}}};
        };
    return functions;
}

[[nodiscard]] UsbInterfaceFilter fastboot_filter() {
    UsbInterfaceFilter filter;
    filter.interface_class = 0xFFU;
    filter.interface_subclass = 0x42U;
    filter.interface_protocol = 0x03U;
    return filter;
}

void test_cancellable_passive_enumeration() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    UsbInterfaceFilter filter;
    filter.interface_class = 0xFFU;
    filter.interface_subclass = 0x42U;
    filter.interface_protocol = 0x03U;

    std::stop_source cancelled;
    cancelled.request_stop();
    auto pre_cancelled = runtime->enumerate(
        filter,
        std::chrono::steady_clock::now() + 1s,
        cancelled.get_token());
    CHECK(!pre_cancelled.has_value());
    CHECK(pre_cancelled.error().kind ==
          LibusbRuntimeErrorKind::operation_cancelled);

    auto expired = runtime->enumerate(
        filter, std::chrono::steady_clock::now() - 1ms, {});
    CHECK(!expired.has_value());
    CHECK(expired.error().kind ==
          LibusbRuntimeErrorKind::operation_timed_out);

    fake->block_descriptor = true;
    std::stop_source during_cancel;
    std::optional<std::expected<std::vector<UsbDeviceInfo>,
                                kairosboot::transport::LibusbRuntimeError>>
        cancelled_result;
    std::jthread worker([&] {
        cancelled_result = runtime->enumerate(
            filter,
            std::chrono::steady_clock::now() + 2s,
            during_cancel.get_token());
    });
    fake->wait_for_descriptor();
    during_cancel.request_stop();
    fake->unblock_descriptor();
    worker.join();
    CHECK(cancelled_result.has_value());
    CHECK(!cancelled_result->has_value());
    CHECK(cancelled_result->error().kind ==
          LibusbRuntimeErrorKind::operation_cancelled);

    fake->block_descriptor = true;
    fake->descriptor_entered = false;
    fake->release_descriptor = false;
    std::optional<std::expected<std::vector<UsbDeviceInfo>,
                                kairosboot::transport::LibusbRuntimeError>>
        timed_result;
    std::jthread deadline_worker([&] {
        timed_result = runtime->enumerate(
            filter, std::chrono::steady_clock::now() + 10ms, {});
    });
    fake->wait_for_descriptor();
    std::this_thread::sleep_for(20ms);
    fake->unblock_descriptor();
    deadline_worker.join();
    CHECK(timed_result.has_value());
    CHECK(!timed_result->has_value());
    CHECK(timed_result->error().kind ==
          LibusbRuntimeErrorKind::operation_timed_out);

    fake->block_descriptor = false;
    const auto legacy = runtime->enumerate(filter);
    CHECK(legacy.has_value());
    CHECK(legacy->size() == 1U);
    runtime->stop();
}

void test_discovery_open_probe_and_ownership() {
    auto fake = std::make_shared<FakeLibusb>();
    fake->bus = 0U;
    auto runtime = create_runtime(fake);
    auto adapter = LibusbReconnectAdapter::create(runtime);
    CHECK(adapter.has_value());
    auto discovered = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(discovered.has_value());
    CHECK(discovered->size() == 1U);
    CHECK(discovered->front().physical_port.bus_number == 0U);
    CHECK(discovered->front().physical_port.ports ==
          std::vector<std::uint8_t>({3U, 4U}));

    // USB address and backend session are transient and may legitimately
    // change while the physical port and complete stable fingerprint remain.
    fake->address = 9U;
    fake->session = 0x202UL;
    auto opened = (*adapter)->open(
        discovered->front(),
        std::chrono::steady_clock::now() + 2s,
        {});
    CHECK(opened.has_value());
    CHECK(opened->verified_identity.product == "product-a");
    CHECK(opened->verified_identity.mode == FastbootUsbMode::Fastbootd);
    CHECK(opened->verified_identity.serial ==
          std::optional<std::string>{"SERIAL"});
    CHECK(opened->session->state() == SessionState::Ready);
    CHECK(opened->outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(fake->commands == std::vector<std::string>({
        "getvar:product", "getvar:is-userspace", "getvar:serialno"}));
    opened->session.reset();
    CHECK(fake->claim_calls == 1);
    CHECK(fake->release_calls == 1);
    CHECK(fake->close_calls == 1);
    adapter->reset();
    runtime->stop();
}

void test_preflight_session_retains_absolute_deadline_after_probe() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(topology_functions(fake));
    auto snapshot = runtime->enumerate(
        fastboot_filter(), std::chrono::steady_clock::now() + 2s, {});
    CHECK(snapshot.has_value());
    CHECK(snapshot->size() == 1U);

    auto opener = make_libusb_device_preflight_session_opener(
        runtime, std::make_shared<BufferBudget>(8U * 1024U * 1024U),
        TransferRingConfig{}, SessionOptions{});
    CHECK(opener.has_value());
    const auto operation_deadline =
        std::chrono::steady_clock::now() + 200ms;
    auto opened = (*opener)->open(
        snapshot->front(), operation_deadline, {});
    CHECK(opened.has_value());
    FastbootDevicePreflightProbe probe;
    auto probed = probe.probe(
        *opened->session, operation_deadline, {});
    CHECK(probed.has_value());
    CHECK(fake->command_count() == 2U);

    std::this_thread::sleep_until(operation_deadline);
    PrimitiveService service(*opened->session);
    auto late = service.getvar("product");
    CHECK(!late.has_value());
    CHECK(late.error().code == PrimitiveErrorCode::Timeout);
    CHECK(late.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(fake->command_count() == 2U);
    opened->session.reset();
    opener->reset();
    runtime->stop();
}

void test_reconnect_replacement_retains_absolute_deadline() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    auto adapter = LibusbReconnectAdapter::create(runtime);
    CHECK(adapter.has_value());
    auto discovered = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(discovered.has_value());
    CHECK(discovered->size() == 1U);

    const auto operation_deadline =
        std::chrono::steady_clock::now() + 200ms;
    auto opened = (*adapter)->open(
        discovered->front(), operation_deadline, {});
    CHECK(opened.has_value());
    CHECK(fake->command_count() == 3U);
    std::this_thread::sleep_until(operation_deadline);

    PrimitiveService service(*opened->session);
    auto late = service.getvar("product");
    CHECK(!late.has_value());
    CHECK(late.error().code == PrimitiveErrorCode::Timeout);
    CHECK(late.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(fake->command_count() == 3U);
    opened->session.reset();
    adapter->reset();
    runtime->stop();
}

void test_failed_discovery_does_not_invalidate_another_open_capability() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    auto adapter = LibusbReconnectAdapter::create(runtime);
    CHECK(adapter.has_value());
    auto discovered = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(discovered.has_value());
    std::stop_source cancelled;
    cancelled.request_stop();
    auto failed = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s,
        cancelled.get_token());
    CHECK(!failed.has_value());
    auto stale_open = (*adapter)->open(
        discovered->front(),
        std::chrono::steady_clock::now() + 2s,
        {});
    CHECK(stale_open.has_value());
    stale_open->session.reset();
    CHECK(fake->open_calls == 1);
    adapter->reset();
    runtime->stop();
}

void test_discovery_open_capabilities_are_attempt_local_and_one_shot() {
    auto fake = std::make_shared<FakeLibusb>();
    auto runtime = create_runtime(fake);
    auto adapter = LibusbReconnectAdapter::create(runtime);
    CHECK(adapter.has_value());

    fake->ports = {3U, 4U};
    auto first = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(first.has_value());
    CHECK(first->size() == 1U);

    fake->ports = {7U};
    auto second = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(second.has_value());
    CHECK(second->size() == 1U);

    // A later discovery must neither overwrite nor authorize the first
    // attempt. Both capabilities may be opened out of discovery order, but
    // each is consumed exactly once.
    std::stop_source cancelled;
    cancelled.request_stop();
    auto cancelled_second = (*adapter)->open(
        second->front(),
        std::chrono::steady_clock::now() + 2s,
        cancelled.get_token());
    CHECK(!cancelled_second.has_value());
    CHECK(cancelled_second.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(fake->open_calls == 0);

    auto opened_second = (*adapter)->open(
        second->front(),
        std::chrono::steady_clock::now() + 2s,
        {});
    CHECK(opened_second.has_value());
    opened_second->session.reset();

    fake->ports = {3U, 4U};
    auto opened_first = (*adapter)->open(
        first->front(),
        std::chrono::steady_clock::now() + 2s,
        {});
    CHECK(opened_first.has_value());
    opened_first->session.reset();

    auto replay = (*adapter)->open(
        first->front(),
        std::chrono::steady_clock::now() + 2s,
        {});
    CHECK(!replay.has_value());
    CHECK(replay.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(fake->open_calls == 2);
    adapter->reset();
    runtime->stop();
}

void test_each_discovered_device_has_an_independent_open_capability() {
    auto fake = std::make_shared<FakeLibusb>();
    fake->device_count = 2U;
    fake->second_serial = fake->serial;
    fake->rebuild_device_list();
    auto runtime = create_runtime(fake);
    auto adapter = LibusbReconnectAdapter::create(runtime);
    CHECK(adapter.has_value());
    auto candidates = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(candidates.has_value());
    CHECK(candidates->size() == 2U);

    // Multiple devices in one passive snapshot carry distinct one-shot
    // handles, so opening either record never performs a global key lookup
    // that could consume or cross-wire the other device.
    auto first = (*adapter)->open(
        (*candidates)[0], std::chrono::steady_clock::now() + 2s, {});
    CHECK(first.has_value());
    first->session.reset();
    auto second = (*adapter)->open(
        (*candidates)[1], std::chrono::steady_clock::now() + 2s, {});
    CHECK(second.has_value());
    second->session.reset();
    CHECK(fake->open_calls == 2);
    CHECK(fake->release_calls == 2);
    CHECK(fake->close_calls == 2);
    adapter->reset();
    runtime->stop();
}

void test_fail_closed_usb_identity_changes_and_duplicates() {
    const auto changed_open = [](const auto& mutate) {
        auto fake = std::make_shared<FakeLibusb>();
        auto runtime = create_runtime(fake);
        auto adapter = LibusbReconnectAdapter::create(runtime);
        CHECK(adapter.has_value());
        auto discovered = (*adapter)->discover(
            std::chrono::steady_clock::now() + 2s, {});
        CHECK(discovered.has_value());
        mutate(*fake);
        auto opened = (*adapter)->open(
            discovered->front(),
            std::chrono::steady_clock::now() + 2s,
            {});
        CHECK(!opened.has_value());
        CHECK(opened.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(fake->commands.empty());
        adapter->reset();
        runtime->stop();
    };
    changed_open([](FakeLibusb& fake) { fake.ports = {7U}; });
    changed_open([](FakeLibusb& fake) { fake.serial = "OTHER"; });
    changed_open([](FakeLibusb& fake) { fake.set_product_id(0x4EE1U); });

    auto fake = std::make_shared<FakeLibusb>();
    fake->device_count = 2U;
    fake->second_ports = fake->ports;
    fake->second_serial = fake->serial;
    fake->rebuild_device_list();
    auto runtime = create_runtime(fake);
    auto adapter = LibusbReconnectAdapter::create(runtime);
    CHECK(adapter.has_value());
    auto candidates = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(candidates.has_value());
    CHECK(candidates->size() == 2U);
    ReconnectTarget target{
        .physical_port = candidates->front().physical_port,
        .serial = candidates->front().serial,
        .usb_fingerprint = candidates->front().usb_fingerprint,
        .product = "product-a",
        .previous_mode = FastbootUsbMode::Bootloader,
        .required_mode = FastbootUsbMode::Fastbootd,
        .preceding_operation_certainty = TransferCertainty::FullyTransferred,
    };
    SteadyReconnectWaiter waiter;
    ReconnectCoordinator coordinator(**adapter, **adapter, waiter);
    auto reconnected = coordinator.reconnect(
        target,
        std::chrono::steady_clock::now() + 1s,
        ReconnectOptions{
            .initial_backoff = 1ms,
            .maximum_backoff = 1ms,
            .maximum_discovered_devices = 4U,
            .maximum_discovery_attempts = 1U,
            .maximum_open_attempts = 1U,
        });
    CHECK(!reconnected.has_value());
    CHECK(reconnected.error().code ==
          ReconnectErrorCode::AmbiguousPhysicalPort);
    CHECK(fake->open_calls == 0);
    adapter->reset();
    runtime->stop();
}

void test_coordinator_revalidates_live_product_and_mode() {
    const auto run = [](std::string product,
                        std::string mode,
                        const ReconnectErrorCode expected_code,
                        const ReconnectObservation expected_observation) {
        auto fake = std::make_shared<FakeLibusb>();
        fake->product_response = std::move(product);
        fake->mode_response = std::move(mode);
        auto runtime = create_runtime(fake);
        auto adapter = LibusbReconnectAdapter::create(runtime);
        CHECK(adapter.has_value());
        auto candidates = (*adapter)->discover(
            std::chrono::steady_clock::now() + 2s, {});
        CHECK(candidates.has_value());
        ReconnectTarget target{
            .physical_port = candidates->front().physical_port,
            .serial = candidates->front().serial,
            .usb_fingerprint = candidates->front().usb_fingerprint,
            .product = "product-a",
            .previous_mode = FastbootUsbMode::Bootloader,
            .required_mode = FastbootUsbMode::Fastbootd,
            .preceding_operation_certainty =
                TransferCertainty::FullyTransferred,
        };
        SteadyReconnectWaiter waiter;
        ReconnectCoordinator coordinator(**adapter, **adapter, waiter);
        auto result = coordinator.reconnect(
            target,
            std::chrono::steady_clock::now() + 1s,
            ReconnectOptions{
                .initial_backoff = 1ms,
                .maximum_backoff = 1ms,
                .maximum_discovered_devices = 2U,
                .maximum_discovery_attempts = 1U,
                .maximum_open_attempts = 1U,
            });
        CHECK(!result.has_value());
        CHECK(result.error().code == expected_code);
        CHECK(result.error().last_observation == expected_observation);
        CHECK(fake->release_calls == 1);
        CHECK(fake->close_calls == 1);
        adapter->reset();
        runtime->stop();
    };
    run("OKAYother-product",
        "OKAYyes",
        ReconnectErrorCode::ProductMismatch,
        ReconnectObservation::CandidatePresent);
    run("OKAYproduct-a",
        "OKAYno",
        ReconnectErrorCode::AttemptLimitExceeded,
        ReconnectObservation::PreviousModePresent);
}

void test_mode_transition_accepts_changed_descriptor_only_with_policy() {
    auto fake = std::make_shared<FakeLibusb>();
    fake->mode_response = "OKAYno";
    auto runtime = create_runtime(fake);
    auto adapter = LibusbReconnectAdapter::create(runtime);
    CHECK(adapter.has_value());
    auto initial = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(initial.has_value());
    CHECK(initial->size() == 1U);

    ReconnectTarget target{
        .physical_port = initial->front().physical_port,
        .serial = initial->front().serial,
        .usb_fingerprint = initial->front().usb_fingerprint,
        .product = "product-a",
        .previous_mode = FastbootUsbMode::Bootloader,
        .required_mode = FastbootUsbMode::Fastbootd,
        .usb_fingerprint_policy =
            ReconnectUsbFingerprintPolicy::AllowChangeWithLiveIdentity,
        .preceding_operation_certainty = TransferCertainty::FullyTransferred,
    };
    fake->set_product_id(0x4EE1U);
    fake->set_interface_number(2U);
    fake->mode_response = "OKAYyes";

    SteadyReconnectWaiter waiter;
    ReconnectCoordinator coordinator(**adapter, **adapter, waiter);
    auto reconnected = coordinator.reconnect(
        target,
        std::chrono::steady_clock::now() + 1s,
        ReconnectOptions{
            .initial_backoff = 1ms,
            .maximum_backoff = 1ms,
            .maximum_discovered_devices = 2U,
            .maximum_discovery_attempts = 1U,
            .maximum_open_attempts = 1U,
        });
    CHECK(reconnected.has_value());
    CHECK(reconnected->identity.usb_fingerprint.product_id == 0x4EE1U);
    CHECK(reconnected->identity.usb_fingerprint.interface_number == 2U);
    CHECK(reconnected->identity.mode == FastbootUsbMode::Fastbootd);
    reconnected->session.reset();
    adapter->reset();
    runtime->stop();
}

void test_live_product_mode_serial_and_interruption_fail_closed() {
    auto fake = std::make_shared<FakeLibusb>();
    fake->product_response = "OKAYother-product";
    fake->mode_response = "OKAYno";
    fake->serial_response = "FAILunsupported";
    auto runtime = create_runtime(fake);
    auto adapter = LibusbReconnectAdapter::create(runtime);
    CHECK(adapter.has_value());
    auto candidates = (*adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(candidates.has_value());
    auto opened = (*adapter)->open(
        candidates->front(),
        std::chrono::steady_clock::now() + 2s,
        {});
    CHECK(opened.has_value());
    CHECK(opened->verified_identity.product == "other-product");
    CHECK(opened->verified_identity.mode == FastbootUsbMode::Bootloader);
    CHECK(opened->verified_identity.serial == candidates->front().serial);
    opened->session.reset();
    adapter->reset();
    runtime->stop();

    auto stalled = std::make_shared<FakeLibusb>();
    stalled->stall_in = true;
    auto stalled_runtime = create_runtime(stalled);
    auto stalled_adapter = LibusbReconnectAdapter::create(stalled_runtime);
    CHECK(stalled_adapter.has_value());
    auto stalled_candidates = (*stalled_adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(stalled_candidates.has_value());
    std::stop_source cancellation;
    std::optional<std::expected<kairosboot::fastboot::OpenedReconnectSession,
                                kairosboot::fastboot::ReconnectOpenError>>
        result;
    std::jthread opener([&] {
        result = (*stalled_adapter)->open(
            stalled_candidates->front(),
            std::chrono::steady_clock::now() + 2s,
            cancellation.get_token());
    });
    wait_until([&] { return stalled->command_count() != 0U; });
    cancellation.request_stop();
    opener.join();
    CHECK(result.has_value());
    CHECK(!result->has_value());
    CHECK(result->error().outbound_certainty !=
          TransferCertainty::NotTransferred);
    CHECK(stalled->release_calls == 1);
    CHECK(stalled->close_calls == 1);
    stalled_adapter->reset();
    stalled_runtime->stop();

    auto timed = std::make_shared<FakeLibusb>();
    timed->stall_in = true;
    auto timed_runtime = create_runtime(timed);
    auto timed_adapter = LibusbReconnectAdapter::create(timed_runtime);
    CHECK(timed_adapter.has_value());
    auto timed_candidates = (*timed_adapter)->discover(
        std::chrono::steady_clock::now() + 2s, {});
    CHECK(timed_candidates.has_value());
    auto timed_result = (*timed_adapter)->open(
        timed_candidates->front(),
        std::chrono::steady_clock::now() + 20ms,
        {});
    CHECK(!timed_result.has_value());
    CHECK(timed_result.error().outbound_certainty !=
          TransferCertainty::NotTransferred);
    CHECK(timed->release_calls == 1);
    CHECK(timed->close_calls == 1);
    timed_adapter->reset();
    timed_runtime->stop();
}

void test_final_interruption_handoff_never_publishes_sticky_cancel() {
    for (std::size_t iteration = 0U; iteration < 32U; ++iteration) {
        auto fake = std::make_shared<FakeLibusb>();
        auto runtime = create_runtime(fake);
        auto adapter = LibusbReconnectAdapter::create(runtime);
        CHECK(adapter.has_value());
        auto candidates = (*adapter)->discover(
            std::chrono::steady_clock::now() + 2s, {});
        CHECK(candidates.has_value());

        std::stop_source cancellation;
        std::atomic<bool> open_finished{};
        std::jthread interrupter([&] {
            while (!open_finished.load(std::memory_order_acquire) &&
                   fake->command_count() < 3U) {
                std::this_thread::yield();
            }
            if (!open_finished.load(std::memory_order_acquire)) {
                if ((iteration % 2U) != 0U) {
                    std::this_thread::yield();
                }
                cancellation.request_stop();
            }
        });
        auto opened = (*adapter)->open(
            candidates->front(),
            std::chrono::steady_clock::now() + 2s,
            cancellation.get_token());
        open_finished.store(true, std::memory_order_release);
        interrupter.join();

        // Cancellation may linearize on either side of the final handoff. A
        // successful handoff must remain usable and must never carry the
        // guard's sticky cancellation bit into the caller-owned session.
        if (opened.has_value()) {
            auto probe = opened->session->command("getvar:product");
            CHECK(probe.has_value());
            opened->session.reset();
        }
        adapter->reset();
        runtime->stop();
    }
}

inline constexpr ManifestSourceLocation kLocation{1U, 1U};

[[nodiscard]] LocatedManifestString located(std::string value) {
    return {.value = std::move(value), .location = kLocation};
}

[[nodiscard]] JobPlan one_device_plan() {
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
        .targets = {ManifestTarget{
            .location = kLocation,
            .name = located("target"),
            .selector = ManifestSelector{
                .location = kLocation,
                .serials = {located("SERIAL")},
                .usb_paths = {},
            },
            .expected_product = located("product-a"),
            .steps = {ManifestStep{
                .location = kLocation,
                .payload = ManifestEraseStep{located("metadata")},
            }},
        }},
        .policy = ManifestPolicy{},
    };
    auto plan = make_job_plan(std::move(manifest));
    CHECK(plan.has_value());
    return std::move(*plan);
}

[[nodiscard]] UsbDeviceInfo preflight_device() {
    UsbDeviceInfo device{
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .bus_number = 1U,
        .device_address = 5U,
        .backend_session_id = 0x101U,
        .configuration_value = 1U,
        .port_path = {3U, 4U},
        .serial_utf8 = "SERIAL",
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
    device.linux_topology = LinuxUsbTopology{
        .physical_port_path = "usb:1-3.4",
        .root_controller_id = "linux-sysfs:/controller",
        .hub_port_chain = device.port_path,
        .vendor_id = device.vendor_id,
        .product_id = device.product_id,
        .bus_number = device.bus_number,
        .device_address = device.device_address,
        .serial_utf8 = device.serial_utf8,
        .product_utf8 = std::nullopt,
        .sysfs_device_path = "/sys/bus/usb/devices/1-3.4",
    };
    return device;
}

class NullTransport final : public ITransportSession {
public:
    [[nodiscard]] TransferResult write(
        std::span<const std::byte>, std::chrono::milliseconds) override {
        return {.status = TransportStatus::IoError,
                .certainty = TransferCertainty::NotTransferred};
    }
    [[nodiscard]] TransferResult read(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return {.status = TransportStatus::IoError,
                .certainty = TransferCertainty::NotTransferred};
    }
    [[nodiscard]] TransferResult read_data(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return {.status = TransportStatus::IoError,
                .certainty = TransferCertainty::NotTransferred};
    }
    void request_cancel() noexcept override {}
    void close() noexcept override {}
};

class PreflightOpener final : public IDevicePreflightSessionOpener {
public:
    [[nodiscard]] std::expected<OpenedDevicePreflightSession,
                                DevicePreflightOpenError>
    open(const UsbDeviceInfo& device,
         DevicePreflightTimePoint,
         std::stop_token) override {
        const auto& topology = *device.linux_topology;
        return OpenedDevicePreflightSession{
            .verified_usb_identity = DevicePreflightUsbIdentity{
                .physical_port_path = topology.physical_port_path,
                .root_controller_id = topology.root_controller_id,
                .hub_port_chain = topology.hub_port_chain,
                .bus_number = device.bus_number,
                .device_address = device.device_address,
                .backend_session_id = device.backend_session_id,
                .serial = device.serial_utf8,
                .usb_fingerprint = DevicePreflightUsbFingerprint{
                    .vendor_id = device.vendor_id,
                    .product_id = device.product_id,
                    .configuration_value = device.configuration_value,
                    .interface_number = device.interface_number,
                    .alternate_setting = device.alternate_setting,
                    .interface_class = device.interface_class,
                    .interface_subclass = device.interface_subclass,
                    .interface_protocol = device.interface_protocol,
                    .bulk_out_endpoint = device.bulk_out_endpoint,
                    .bulk_out_max_packet_size =
                        device.bulk_out_max_packet_size,
                    .bulk_in_endpoint = device.bulk_in_endpoint,
                    .bulk_in_max_packet_size =
                        device.bulk_in_max_packet_size,
                },
                .platform_attestation = topology,
            },
            .session = std::make_unique<FastbootSession>(
                std::make_unique<NullTransport>()),
        };
    }
};

class PreflightProbe final : public IDevicePreflightProbe {
public:
    [[nodiscard]] std::expected<DevicePreflightProbeResult,
                                DevicePreflightProbeError>
    probe(FastbootSession&,
          DevicePreflightTimePoint,
          std::stop_token) override {
        return DevicePreflightProbeResult{
            .product = "product-a",
            .mode = FastbootUsbMode::Bootloader,
            .product_query_completed = true,
            .mode_query_completed = true,
        };
    }
};

void test_public_update_binding_accepts_only_verified_bootloader_start() {
    const auto device = preflight_device();
    PreflightOpener opener;
    PreflightProbe probe;
    auto opened = opener.open(device, std::chrono::steady_clock::now() + 2s, {});
    CHECK(opened.has_value());
    auto probed = probe.probe(*opened->session,
                              std::chrono::steady_clock::now() + 2s, {});
    CHECK(probed.has_value());
    auto binding = bind_initial_libusb_update_session(
        std::move(*opened), *probed);
    CHECK(binding.has_value());

    auto rejected_open =
        opener.open(device, std::chrono::steady_clock::now() + 2s, {});
    CHECK(rejected_open.has_value());
    auto fastbootd = *probed;
    fastbootd.mode = FastbootUsbMode::Fastbootd;
    auto rejected = bind_initial_libusb_update_session(
        std::move(*rejected_open), fastbootd);
    CHECK(!rejected.has_value());
}

void test_prepared_binding_is_unforgeable_and_fail_closed() {
    const auto plan = one_device_plan();
    const std::array snapshot{preflight_device()};
    PreflightOpener opener;
    PreflightProbe probe;
    auto prepared = preflight_fleet_devices(
        plan,
        snapshot,
        opener,
        probe,
        std::chrono::steady_clock::now() + 2s);
    CHECK(prepared.has_value());
    CHECK(prepared->devices().size() == 1U);
    auto binding = make_prepared_reconnect_binding(
        prepared->devices().front());
    CHECK(binding.has_value());

    auto target = binding->target_after_transition(
        FastbootUsbMode::Fastbootd,
        SessionState::Ready,
        TransferCertainty::FullyTransferred);
    CHECK(target.has_value());
    CHECK(target->physical_port.bus_number == 1U);
    CHECK(target->physical_port.ports ==
          std::vector<std::uint8_t>({3U, 4U}));
    CHECK(target->product == "product-a");
    CHECK(target->previous_mode == FastbootUsbMode::Bootloader);
    CHECK(target->required_mode == FastbootUsbMode::Fastbootd);
    CHECK(target->usb_fingerprint_policy ==
          ReconnectUsbFingerprintPolicy::AllowChangeWithLiveIdentity);

    auto same_mode = binding->target_after_transition(
        FastbootUsbMode::Bootloader,
        SessionState::Ready,
        TransferCertainty::FullyTransferred);
    CHECK(same_mode.has_value());
    CHECK(same_mode->usb_fingerprint_policy ==
          ReconnectUsbFingerprintPolicy::Exact);

    auto poisoned = binding->target_after_transition(
        FastbootUsbMode::Fastbootd,
        SessionState::Poisoned,
        TransferCertainty::FullyTransferred);
    CHECK(!poisoned.has_value());
    CHECK(poisoned.error().code ==
          PreparedReconnectBindingErrorCode::UnsafeSessionState);
    auto uncertain = binding->target_after_transition(
        FastbootUsbMode::Fastbootd,
        SessionState::Ready,
        TransferCertainty::PartialOrUnknown);
    CHECK(!uncertain.has_value());
    CHECK(uncertain.error().code ==
          PreparedReconnectBindingErrorCode::UnsafeTransferOutcome);
    auto not_sent = binding->target_after_transition(
        FastbootUsbMode::Fastbootd,
        SessionState::Ready,
        TransferCertainty::NotTransferred);
    CHECK(!not_sent.has_value());
    CHECK(not_sent.error().code ==
          PreparedReconnectBindingErrorCode::UnsafeTransferOutcome);
}

}  // namespace

int main() {
    try {
        test_cancellable_passive_enumeration();
        test_discovery_open_probe_and_ownership();
        test_preflight_session_retains_absolute_deadline_after_probe();
        test_reconnect_replacement_retains_absolute_deadline();
        test_failed_discovery_does_not_invalidate_another_open_capability();
        test_discovery_open_capabilities_are_attempt_local_and_one_shot();
        test_each_discovered_device_has_an_independent_open_capability();
        test_fail_closed_usb_identity_changes_and_duplicates();
        test_coordinator_revalidates_live_product_and_mode();
        test_mode_transition_accepts_changed_descriptor_only_with_policy();
        test_live_product_mode_serial_and_interruption_fail_closed();
        test_final_interruption_handoff_never_publishes_sticky_cancel();
        test_public_update_binding_accepts_only_verified_bootloader_start();
        test_prepared_binding_is_unforgeable_and_fail_closed();
    } catch (const std::exception& error) {
        std::cerr << "libusb reconnect adapter test failed: " << error.what()
                  << '\n';
        return 1;
    }
    return 0;
}
