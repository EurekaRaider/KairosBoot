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
using kairosboot::fleet::PreparedDeviceSession;
using kairosboot::fleet::make_job_plan;
using kairosboot::fleet::preflight_fleet_devices;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferResult;
using kairosboot::protocol::TransportStatus;
using kairosboot::transport::LinuxUsbTopology;
using kairosboot::transport::UsbDeviceInfo;

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

[[nodiscard]] JobPlan plan(std::vector<TargetSpec> specifications) {
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
                .payload = ManifestEraseStep{located("metadata")},
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

[[nodiscard]] DevicePreflightUsbIdentity identity(const UsbDeviceInfo& value) {
    CHECK(value.linux_topology.has_value());
    const auto& topology = *value.linux_topology;
    return {
        .physical_port_path = topology.physical_port_path,
        .root_controller_id = topology.root_controller_id,
        .hub_port_chain = topology.hub_port_chain,
        .bus_number = value.bus_number,
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
};

class RecordingOpener final : public IDevicePreflightSessionOpener {
public:
    std::size_t calls{};
    std::optional<std::size_t> throw_bad_alloc_on_call;
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
        if (failure.has_value()) {
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
    std::optional<DevicePreflightProbeError> failure;

    [[nodiscard]] std::expected<DevicePreflightProbeResult,
                                DevicePreflightProbeError>
    probe(FastbootSession&,
          DevicePreflightTimePoint,
          std::stop_token) override {
        if (failure.has_value()) {
            return std::unexpected(*failure);
        }
        CHECK(calls < results.size());
        return results[calls++];
    }
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
}

void duplicate_physical_and_serial_identities_fail_before_open() {
    auto job = plan({TargetSpec{
        .name = "target-a",
        .serials = {"SERIAL-A"},
        .usb_paths = {},
        .product = "product-a",
    }});
    {
        std::vector snapshot{
            device("SERIAL-A", "usb:1-2", 2U, 2U),
            device("SERIAL-B", "usb:1-2", 2U, 3U),
        };
        RecordingOpener opener;
        SequenceProbe probe;
        auto result = preflight_fleet_devices(
            job, snapshot, opener, probe, deadline());
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
            job, snapshot, opener, probe, deadline());
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

void thirty_two_device_batch_completes_the_live_barrier_before_failure() {
    TargetSpec target{
        .name = "target-a",
        .serials = {},
        .usb_paths = {},
        .product = "product-a",
    };
    std::vector<UsbDeviceInfo> snapshot;
    std::vector<DevicePreflightProbeResult> probe_results;
    snapshot.reserve(32U);
    target.serials.reserve(32U);
    probe_results.reserve(32U);
    for (std::uint8_t index = 1U; index <= 32U; ++index) {
        auto serial = "SERIAL-" + std::to_string(index);
        target.serials.push_back(serial);
        snapshot.push_back(device(std::move(serial),
                                  "usb:1-" + std::to_string(index),
                                  index,
                                  index));
        probe_results.push_back(live(index == 32U ? "wrong-product"
                                                  : "product-a"));
    }
    auto job = plan({std::move(target)});
    RecordingOpener opener;
    SequenceProbe probe{std::move(probe_results)};

    auto result = preflight_fleet_devices(
        job, snapshot, opener, probe, deadline());
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
                                OpenMutation::Fingerprint}) {
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
    const std::array<Test, 11U> tests{
        Test{"selectors are exact, deduplicated, and ignore unrelated devices",
             &selectors_are_exact_deduplicated_and_ignore_unrelated_devices},
        Test{"duplicate physical and serial identities fail before open",
             &duplicate_physical_and_serial_identities_fail_before_open},
        Test{"cross-target multi-namespace match is rejected",
             &cross_target_multi_namespace_match_is_rejected},
        Test{"missing selector and unreliable topology fail closed",
             &missing_selector_and_unreliable_topology_fail_closed},
        Test{"product mismatch withholds batch and reports outcomes",
             &product_mismatch_withholds_the_entire_batch_and_reports_outcomes},
        Test{"32-device batch completes live barrier before failure",
             &thirty_two_device_batch_completes_the_live_barrier_before_failure},
        Test{"open race and probe contract violations withhold the gate",
             &open_race_and_probe_contract_violations_never_publish_a_gate},
        Test{"cancellation, deadline, and exception preserve barrier",
             &cancellation_deadline_and_exception_preserve_the_barrier},
        Test{"prepared order is deterministic across snapshot order",
             &prepared_order_is_deterministic_across_snapshot_order},
        Test{"built-in probe reads product and mode from Fastboot wire",
             &built_in_probe_reads_product_and_mode_from_fastboot_wire},
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
