// SPDX-License-Identifier: MIT
#include "src/fleet/device_actor.hpp"
#include "src/image/sparse_flash_plan.hpp"
#include "src/image/sparse_image.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
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
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::fastboot::FastbootUsbMode;
using kairosboot::fastboot::UpdateOperationContext;
using kairosboot::fleet::DevicePreflightProbeError;
using kairosboot::fleet::DevicePreflightProbeResult;
using kairosboot::fleet::DevicePreflightTimePoint;
using kairosboot::fleet::DevicePreflightUsbFingerprint;
using kairosboot::fleet::DevicePreflightUsbIdentity;
using kairosboot::fleet::FlashJobManifest;
using kairosboot::fleet::FleetActorExecutionErrorKind;
using kairosboot::fleet::FleetActorPrepareErrorKind;
using kairosboot::fleet::IDevicePreflightProbe;
using kairosboot::fleet::IDevicePreflightSessionOpener;
using kairosboot::fleet::JobPlan;
using kairosboot::fleet::JobReportBuilder;
using kairosboot::fleet::LocatedManifestString;
using kairosboot::fleet::ManifestActiveSlot;
using kairosboot::fleet::ManifestArtifact;
using kairosboot::fleet::ManifestEraseStep;
using kairosboot::fleet::ManifestFlashSlot;
using kairosboot::fleet::ManifestFlashStep;
using kairosboot::fleet::ManifestOemStep;
using kairosboot::fleet::ManifestPolicy;
using kairosboot::fleet::ManifestRebootStep;
using kairosboot::fleet::ManifestRebootTarget;
using kairosboot::fleet::ManifestSelector;
using kairosboot::fleet::ManifestSetActiveStep;
using kairosboot::fleet::ManifestSourceLocation;
using kairosboot::fleet::ManifestStep;
using kairosboot::fleet::ManifestTarget;
using kairosboot::fleet::OpenedDevicePreflightSession;
using kairosboot::fleet::PreparedDeviceBatchConsumption;
using kairosboot::fleet::PreparedDeviceSession;
using kairosboot::fleet::PreparedFleetActorBatch;
using kairosboot::fleet::make_job_plan;
using kairosboot::fleet::preflight_fleet_artifacts;
using kairosboot::fleet::preflight_fleet_devices;
using kairosboot::fleet::prepare_fleet_device_actors;
using kairosboot::image::Sha256Accumulator;
using kairosboot::image::Sha256Digest;
using kairosboot::image::sha256_hex;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::IStreamingTransportSession;
using kairosboot::protocol::ITransferSource;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferProgressAction;
using kairosboot::protocol::TransferProgressObserver;
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

template <typename Value>
concept HasTakeSession = requires(Value&& value) {
    std::move(value).take_session();
};

template <typename Value>
concept HasSessionGetter = requires(Value& value) {
    value.session();
};

template <typename Value>
concept HasServiceGetter = requires(Value& value) {
    value.service();
};

template <typename Value>
concept HasTaskGetter = requires(Value& value) {
    value.tasks();
};

static_assert(!std::is_copy_constructible_v<PreparedFleetActorBatch>);
static_assert(!std::is_copy_assignable_v<PreparedFleetActorBatch>);
static_assert(std::is_nothrow_move_constructible_v<PreparedFleetActorBatch>);
static_assert(!HasTakeSession<PreparedDeviceSession>);
static_assert(!HasTakeSession<PreparedDeviceBatchConsumption>);
static_assert(!HasSessionGetter<PreparedFleetActorBatch>);
static_assert(!HasServiceGetter<PreparedFleetActorBatch>);
static_assert(!HasTaskGetter<PreparedFleetActorBatch>);

inline constexpr ManifestSourceLocation kLocation{1U, 1U};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> serial{0U};
        path_ = std::filesystem::temp_directory_path() /
            ("kairosboot-device-actor-" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()) +
             "-" + std::to_string(serial.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] LocatedManifestString located(std::string value) {
    return {.value = std::move(value), .location = kLocation};
}

[[nodiscard]] std::string digest_for(const std::string_view bytes) {
    Sha256Accumulator accumulator;
    accumulator.update(std::as_bytes(std::span(bytes.data(), bytes.size())));
    return sha256_hex(accumulator.finish());
}

void write_bytes(const std::filesystem::path& path,
                 const std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

void append_u16(std::string& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xffU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xffU));
}

void append_u32(std::string& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<char>(value & 0xffU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<char>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<char>((value >> 24U) & 0xffU));
}

[[nodiscard]] std::string one_block_sparse_image() {
    constexpr std::uint32_t block_size = 4096U;
    std::string bytes;
    bytes.reserve(28U + 12U + block_size);
    append_u32(bytes, kairosboot::image::kAndroidSparseMagic);
    append_u16(bytes, kairosboot::image::kAndroidSparseMajorVersion);
    append_u16(bytes, 0U);
    append_u16(bytes, 28U);
    append_u16(bytes, 12U);
    append_u32(bytes, block_size);
    append_u32(bytes, 1U);
    append_u32(bytes, 1U);
    append_u32(bytes, 0U);
    append_u16(bytes, kairosboot::image::kSparseChunkRaw);
    append_u16(bytes, 0U);
    append_u32(bytes, 1U);
    append_u32(bytes, 12U + block_size);
    for (std::uint32_t index = 0U; index < block_size; ++index) {
        bytes.push_back(static_cast<char>((index * 13U + 7U) % 251U));
    }
    return bytes;
}

[[nodiscard]] std::string three_block_raw_image() {
    std::string bytes(3U * 4096U, '\0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((index * 17U + 3U) % 251U);
    }
    return bytes;
}

[[nodiscard]] ManifestStep flash_step(
    std::string partition,
    std::string artifact,
    const std::optional<ManifestFlashSlot> slot = std::nullopt) {
    return {
        .location = kLocation,
        .payload = ManifestFlashStep{
            .partition = located(std::move(partition)),
            .artifact = located(std::move(artifact)),
            .slot = slot,
            .slot_location = slot ? std::optional{kLocation} : std::nullopt,
        },
    };
}

[[nodiscard]] ManifestStep erase_step(std::string partition) {
    return {
        .location = kLocation,
        .payload = ManifestEraseStep{located(std::move(partition))},
    };
}

[[nodiscard]] ManifestStep active_step(const ManifestActiveSlot slot) {
    return {
        .location = kLocation,
        .payload = ManifestSetActiveStep{
            .slot = slot,
            .slot_location = kLocation,
        },
    };
}

[[nodiscard]] ManifestStep oem_step(std::string command) {
    return {
        .location = kLocation,
        .payload = ManifestOemStep{located(std::move(command))},
    };
}

[[nodiscard]] ManifestStep reboot_step(
    const ManifestRebootTarget target = ManifestRebootTarget::System) {
    return {
        .location = kLocation,
        .payload = ManifestRebootStep{
            .target = target,
            .target_location = kLocation,
        },
    };
}

[[nodiscard]] JobPlan make_plan(
    std::vector<std::string> serials,
    std::vector<ManifestStep> steps,
    const std::string_view payload,
    std::string target_name = "target-a") {
    std::vector<LocatedManifestString> selectors;
    selectors.reserve(serials.size());
    for (auto& serial : serials) {
        selectors.push_back(located(std::move(serial)));
    }
    FlashJobManifest manifest{
        .location = kLocation,
        .api_version = located("kairosboot.io/v1"),
        .kind = located("FlashJob"),
        .source_sha256 = Sha256Digest{},
        .artifacts = {ManifestArtifact{
            .location = kLocation,
            .id = located("image"),
            .path = located("images/image.img"),
            .sha256 = located(digest_for(payload)),
        }},
        .targets = {ManifestTarget{
            .location = kLocation,
            .name = located(std::move(target_name)),
            .selector = ManifestSelector{
                .location = kLocation,
                .serials = std::move(selectors),
                .usb_paths = {},
            },
            .expected_product = located("product-a"),
            .steps = std::move(steps),
        }},
        .policy = ManifestPolicy{},
    };
    auto plan = make_job_plan(std::move(manifest));
    CHECK(plan.has_value());
    return std::move(*plan);
}

struct TransportState final {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> commands;
    std::vector<std::string> payloads;
    std::string pending_response;
    std::string fail_getvar;
    std::string fail_command;
    std::string poison_command;
    std::string block_command;
    bool blocked{};
    bool release_block{};
    bool closed{};
    bool cancellation_requested{};
    std::uint64_t maximum_download_size{1024U * 1024U};
    bool slotted{true};
    std::string current_slot{"a"};
};

class AutomaticTransport final : public ITransportSession,
                                 public IStreamingTransportSession {
public:
    explicit AutomaticTransport(std::shared_ptr<TransportState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] TransferResult write(
        const std::span<const std::byte> bytes,
        std::chrono::milliseconds) override {
        const std::string command(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::unique_lock lock(state_->mutex);
        state_->commands.push_back(command);
        if (command == state_->block_command) {
            state_->blocked = true;
            state_->condition.notify_all();
            state_->condition.wait(lock, [&] { return state_->release_block; });
        }
        if (command == state_->poison_command) {
            return {
                .status = TransportStatus::IoError,
                .transferred = command.empty() ? 0U : 1U,
                .certainty = TransferCertainty::PartialOrUnknown,
                .truncated = false,
                .detail = "injected partial command write",
                .native_code = 0,
            };
        }
        if (command.starts_with("download:")) {
            state_->pending_response = "DATA" + command.substr(9U);
        } else if (command.starts_with("getvar:")) {
            const auto name = command.substr(7U);
            if (name == state_->fail_getvar) {
                state_->pending_response = "FAILinjected getvar failure";
            } else if (name == "has-slot:boot") {
                state_->pending_response =
                    state_->slotted ? "OKAYyes" : "OKAYno";
            } else if (name == "slot-count") {
                state_->pending_response = "OKAY2";
            } else if (name == "current-slot") {
                state_->pending_response = "OKAY" + state_->current_slot;
            } else if (name == "max-download-size") {
                state_->pending_response =
                    "OKAY" + std::to_string(state_->maximum_download_size);
            } else {
                state_->pending_response = "OKAYyes";
            }
        } else if (command == state_->fail_command) {
            state_->pending_response = "FAILinjected command failure";
        } else {
            state_->pending_response = "OKAY";
        }
        return {
            .status = TransportStatus::Ok,
            .transferred = bytes.size(),
            .certainty = TransferCertainty::FullyTransferred,
            .truncated = false,
            .detail = {},
            .native_code = 0,
        };
    }

    [[nodiscard]] TransferResult read(
        const std::span<std::byte> destination,
        std::chrono::milliseconds) override {
        std::lock_guard lock(state_->mutex);
        if (state_->pending_response.empty()) {
            return {
                .status = TransportStatus::IoError,
                .transferred = 0U,
                .certainty = TransferCertainty::NotTransferred,
                .truncated = false,
                .detail = "no scripted response",
                .native_code = 0,
            };
        }
        const auto count = std::min(destination.size(),
                                    state_->pending_response.size());
        std::memcpy(destination.data(), state_->pending_response.data(), count);
        const bool truncated = count != state_->pending_response.size();
        state_->pending_response.clear();
        return {
            .status = TransportStatus::Ok,
            .transferred = count,
            .certainty = TransferCertainty::FullyTransferred,
            .truncated = truncated,
            .detail = {},
            .native_code = 0,
        };
    }

    [[nodiscard]] TransferResult read_data(
        std::span<std::byte>, std::chrono::milliseconds) override {
        return {
            .status = TransportStatus::IoError,
            .transferred = 0U,
            .certainty = TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = "unexpected inbound DATA",
            .native_code = 0,
        };
    }

    [[nodiscard]] TransferResult write_source(
        const std::shared_ptr<ITransferSource> source,
        std::chrono::milliseconds,
        const TransferProgressObserver& observer) override {
        if (source == nullptr ||
            source->size() > std::numeric_limits<std::size_t>::max()) {
            return {
                .status = TransportStatus::IoError,
                .transferred = 0U,
                .certainty = TransferCertainty::NotTransferred,
                .truncated = false,
                .detail = {},
                .native_code = 0,
            };
        }
        std::string payload(static_cast<std::size_t>(source->size()), '\0');
        if (!source->read_exact(
                0U, std::as_writable_bytes(std::span(payload)))) {
            return {
                .status = TransportStatus::IoError,
                .transferred = 0U,
                .certainty = TransferCertainty::NotTransferred,
                .truncated = false,
                .detail = "unable to read prepared source",
                .native_code = 0,
            };
        }
        if (observer &&
            observer(source->size(), source->size()) ==
                TransferProgressAction::cancel) {
            return {
                .status = TransportStatus::Cancelled,
                .transferred = 0U,
                .certainty = TransferCertainty::PartialOrUnknown,
                .truncated = false,
                .detail = {},
                .native_code = 0,
            };
        }
        std::lock_guard lock(state_->mutex);
        state_->payloads.push_back(std::move(payload));
        state_->pending_response = "OKAY";
        return {
            .status = TransportStatus::Ok,
            .transferred = static_cast<std::size_t>(source->size()),
            .certainty = TransferCertainty::FullyTransferred,
            .truncated = false,
            .detail = {},
            .native_code = 0,
        };
    }

    void request_cancel() noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->cancellation_requested = true;
        state_->release_block = true;
        state_->condition.notify_all();
    }

    void close() noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        state_->release_block = true;
        state_->condition.notify_all();
    }

private:
    std::shared_ptr<TransportState> state_;
};

[[nodiscard]] UsbDeviceInfo device(const std::size_t index) {
    const auto address = static_cast<std::uint8_t>(index + 1U);
    UsbDeviceInfo result{
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .bus_number = 1U,
        .device_address = address,
        .backend_session_id = address,
        .configuration_value = 1U,
        .port_path = {address},
        .serial_utf8 = "SERIAL-" + std::to_string(index),
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
        .physical_port_path = "usb:1-" + std::to_string(index + 1U),
        .root_controller_id = "linux-sysfs:/controller0",
        .hub_port_chain = result.port_path,
        .vendor_id = result.vendor_id,
        .product_id = result.product_id,
        .bus_number = result.bus_number,
        .device_address = result.device_address,
        .serial_utf8 = result.serial_utf8,
        .product_utf8 = std::nullopt,
        .sysfs_device_path = "/sys/bus/usb/devices/1-" +
            std::to_string(index + 1U),
    };
    return result;
}

[[nodiscard]] DevicePreflightUsbIdentity identity(
    const UsbDeviceInfo& value) {
    return {
        .physical_port_path = value.linux_topology->physical_port_path,
        .root_controller_id = value.linux_topology->root_controller_id,
        .hub_port_chain = value.port_path,
        .bus_number = value.bus_number,
        .device_address = value.device_address,
        .backend_session_id = value.backend_session_id,
        .serial = value.serial_utf8,
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
        .platform_attestation = *value.linux_topology,
    };
}

class AutomaticOpener final : public IDevicePreflightSessionOpener {
public:
    explicit AutomaticOpener(
        std::vector<std::shared_ptr<TransportState>> states)
        : states_(std::move(states)) {}

    [[nodiscard]] std::expected<OpenedDevicePreflightSession,
                                kairosboot::fleet::DevicePreflightOpenError>
    open(const UsbDeviceInfo& value,
         DevicePreflightTimePoint,
         std::stop_token) override {
        CHECK(next_ < states_.size());
        return OpenedDevicePreflightSession{
            .verified_usb_identity = identity(value),
            .session = std::make_unique<FastbootSession>(
                std::make_unique<AutomaticTransport>(states_[next_++])),
        };
    }

private:
    std::vector<std::shared_ptr<TransportState>> states_;
    std::size_t next_{};
};

class AutomaticProbe final : public IDevicePreflightProbe {
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

struct PreparedInputs final {
    std::vector<UsbDeviceInfo> snapshot;
    std::vector<std::shared_ptr<TransportState>> states;
    std::optional<PreparedDeviceBatchConsumption> devices;
};

[[nodiscard]] PreparedInputs prepare_devices(const JobPlan& plan,
                                             const std::size_t count) {
    PreparedInputs result;
    result.snapshot.reserve(count);
    result.states.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        result.snapshot.push_back(device(index));
        result.states.push_back(std::make_shared<TransportState>());
    }
    AutomaticOpener opener(result.states);
    AutomaticProbe probe;
    auto prepared = preflight_fleet_devices(
        plan, result.snapshot, opener, probe,
        std::chrono::steady_clock::now() + 5s);
    CHECK(prepared.has_value());
    auto consumed = std::move(*prepared).consume(plan.sha256());
    CHECK(consumed.has_value());
    result.devices.emplace(std::move(*consumed));
    return result;
}

[[nodiscard]] std::vector<std::string> serials(const std::size_t count) {
    std::vector<std::string> result;
    result.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        result.push_back("SERIAL-" + std::to_string(index));
    }
    return result;
}

[[nodiscard]] std::vector<std::string> commands(
    const std::shared_ptr<TransportState>& state) {
    std::lock_guard lock(state->mutex);
    return state->commands;
}

[[nodiscard]] bool closed(
    const std::shared_ptr<TransportState>& state) {
    std::lock_guard lock(state->mutex);
    return state->closed;
}

void digest_mismatch_does_not_unwrap_devices() {
    TemporaryDirectory temporary;
    const std::string payload = "digest-bound-image";
    write_bytes(temporary.path() / "images/image.img", payload);
    auto first = make_plan(
        serials(1U), {erase_step("userdata")}, payload, "first");
    auto second = make_plan(
        serials(1U), {erase_step("userdata")}, payload, "second");
    auto artifacts = preflight_fleet_artifacts(first, temporary.path());
    CHECK(artifacts.has_value());
    auto inputs = prepare_devices(first, 1U);

    auto rejected = prepare_fleet_device_actors(
        second, std::move(*artifacts), std::move(*inputs.devices));
    CHECK(!rejected);
    CHECK(rejected.error().kind ==
          FleetActorPrepareErrorKind::PlanDigestMismatch);
    CHECK(inputs.devices->devices().size() == 1U);
    CHECK(artifacts->size() == 1U);
    CHECK(!closed(inputs.states.front()));
    inputs.devices.reset();
    CHECK(closed(inputs.states.front()));
}

void sealed_source_and_five_operation_trace() {
    TemporaryDirectory temporary;
    const std::string original = "sealed-original-image";
    write_bytes(temporary.path() / "images/image.img", original);
    auto plan = make_plan(
        serials(1U),
        {
            flash_step("boot", "image", ManifestFlashSlot::All),
            erase_step("userdata"),
            active_step(ManifestActiveSlot::Other),
            oem_step("unlock-go"),
            reboot_step(ManifestRebootTarget::Bootloader),
        },
        original);
    auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
    CHECK(artifacts.has_value());
    write_bytes(temporary.path() / "images/image.img",
                std::string(original.size(), 'x'));
    auto inputs = prepare_devices(plan, 1U);

    auto batch = prepare_fleet_device_actors(
        plan, std::move(*artifacts), std::move(*inputs.devices));
    if (!batch) {
        throw CheckFailure("five-operation preparation failed: " +
                           batch.error().message +
                           (batch.error().device_error
                                ? " / " + batch.error().device_error->message
                                : std::string{}));
    }
    CHECK(batch->report_specs().size() == 1U);
    const auto& spec = batch->report_specs().front();
    CHECK(spec.identifier == "usb:1-1");
    CHECK(spec.serial == "SERIAL-0");
    CHECK(spec.usb_path == "usb:1-1");
    CHECK(spec.observed_product == "product-a");
    CHECK(spec.steps.size() == 5U);
    CHECK(spec.steps[0].bytes_total == original.size() * 2U);
    CHECK(!spec.steps[1].bytes_total.has_value());
    CHECK(!spec.steps[2].bytes_total.has_value());
    CHECK(!spec.steps[3].bytes_total.has_value());
    CHECK(!spec.steps[4].bytes_total.has_value());
    auto report_specs = std::vector<kairosboot::fleet::ReportDeviceSpec>(
        batch->report_specs().begin(), batch->report_specs().end());
    CHECK(JobReportBuilder::create(
              "job-actor", plan.sha256(), "2026-08-27T00:00:00.000Z",
              std::move(report_specs))
              .has_value());

    CHECK(std::ranges::all_of(
        commands(inputs.states.front()), [](const std::string& command) {
            return command.starts_with("getvar:");
        }));

    auto executed = batch->execute_device(0U);
    CHECK(executed.has_value());
    CHECK(executed->completed_steps == 5U);
    CHECK(executed->completed_data_bytes == original.size() * 2U);
    CHECK(closed(inputs.states.front()));
    {
        std::lock_guard lock(inputs.states.front()->mutex);
        CHECK(inputs.states.front()->payloads ==
              std::vector<std::string>({original, original}));
    }
    const auto trace = commands(inputs.states.front());
    const std::vector<std::string> expected{
        "getvar:has-slot:boot",
        "getvar:slot-count",
        "getvar:max-download-size",
        "getvar:slot-count",
        "getvar:current-slot",
        "download:00000015",
        "flash:boot_a",
        "download:00000015",
        "flash:boot_b",
        "erase:userdata",
        "set_active:b",
        "oem unlock-go",
        "reboot-bootloader",
    };
    CHECK(trace == expected);

    auto repeated = batch->execute_device(0U);
    CHECK(!repeated);
    CHECK(repeated.error().kind ==
          FleetActorExecutionErrorKind::AlreadyExecuted);
}

void sparse_and_resparse_report_exact_data_bytes() {
    {
        TemporaryDirectory temporary;
        const auto sparse = one_block_sparse_image();
        write_bytes(temporary.path() / "images/image.img", sparse);
        auto plan = make_plan(
            serials(1U), {flash_step("boot", "image")}, sparse,
            "sparse");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        CHECK(artifacts->at(0U).source()->flash_artifact().sparse_image() !=
              nullptr);
        auto inputs = prepare_devices(plan, 1U);
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        CHECK(batch->report_specs()[0U].steps[0U].bytes_total ==
              sparse.size());
        CHECK(batch->execute_device(0U).has_value());
        std::lock_guard lock(inputs.states.front()->mutex);
        CHECK(inputs.states.front()->payloads ==
              std::vector<std::string>{sparse});
    }

    {
        TemporaryDirectory temporary;
        const auto raw = three_block_raw_image();
        write_bytes(temporary.path() / "images/image.img", raw);
        auto plan = make_plan(
            serials(1U), {flash_step("boot", "image")}, raw,
            "resparse");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto oracle = kairosboot::image::SparseFlashPlan::create(
            artifacts->at(0U).source()->flash_artifact(), 4200U);
        CHECK(oracle.has_value());
        CHECK(oracle->parts().size() == 3U);
        const auto expected_bytes = oracle->transfer_size();

        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->maximum_download_size = 4200U;
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        CHECK(batch->report_specs()[0U].steps[0U].bytes_total ==
              expected_bytes);
        auto executed = batch->execute_device(0U);
        CHECK(executed.has_value());
        CHECK(executed->completed_data_bytes == expected_bytes);
        std::lock_guard lock(inputs.states.front()->mutex);
        CHECK(inputs.states.front()->payloads.size() == 3U);
        std::uint64_t transferred = 0U;
        for (const auto& payload : inputs.states.front()->payloads) {
            transferred += payload.size();
        }
        CHECK(transferred == expected_bytes);
    }
}

void every_flash_slot_form_is_frozen_during_prepare() {
    TemporaryDirectory temporary;
    const std::string payload = "slot-image";
    write_bytes(temporary.path() / "images/image.img", payload);
    auto plan = make_plan(
        serials(1U),
        {
            flash_step("boot", "image", ManifestFlashSlot::Current),
            flash_step("boot", "image", ManifestFlashSlot::Other),
            flash_step("boot", "image", ManifestFlashSlot::All),
            flash_step("boot", "image", ManifestFlashSlot::A),
            flash_step("boot", "image", ManifestFlashSlot::B),
        },
        payload,
        "slots");
    auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
    CHECK(artifacts.has_value());
    auto inputs = prepare_devices(plan, 1U);
    auto batch = prepare_fleet_device_actors(
        plan, std::move(*artifacts), std::move(*inputs.devices));
    CHECK(batch.has_value());
    std::uint64_t prepared_bytes = 0U;
    for (const auto& step : batch->report_specs()[0U].steps) {
        prepared_bytes += step.bytes_total.value_or(0U);
    }
    CHECK(prepared_bytes == payload.size() * 6U);

    const auto preparation_trace = commands(inputs.states.front());
    CHECK(std::ranges::all_of(
        preparation_trace, [](const std::string& command) {
            return command.starts_with("getvar:");
        }));
    auto executed = batch->execute_device(0U);
    CHECK(executed.has_value());
    const auto trace = commands(inputs.states.front());
    CHECK(std::ranges::count(trace, "flash:boot_a") == 3);
    CHECK(std::ranges::count(trace, "flash:boot_b") == 3);
}

void late_preparation_failure_has_no_destructive_wire_effect() {
    for (const std::size_t count : {2U, 32U}) {
        TemporaryDirectory temporary;
        const std::string payload = "barrier-image";
        write_bytes(temporary.path() / "images/image.img", payload);
        auto plan = make_plan(
            serials(count),
            {erase_step("metadata"), flash_step("boot", "image")},
            payload);
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, count);
        inputs.states.back()->fail_getvar = "max-download-size";

        auto rejected = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(!rejected);
        if (rejected.error().device_index != count - 1U) {
            throw CheckFailure("late barrier failed at unexpected device: " +
                               rejected.error().message +
                               (rejected.error().device_error
                                    ? " / " +
                                          rejected.error().device_error->message
                                    : std::string{}));
        }
        CHECK(rejected.error().step_index == 1U);
        for (const auto& state : inputs.states) {
            const auto trace = commands(state);
            CHECK(std::ranges::all_of(trace, [](const std::string& command) {
                return command.starts_with("getvar:");
            }));
            CHECK(closed(state));
        }
    }
}

void reboot_must_be_last_and_oem_validation_is_prepare_only() {
    TemporaryDirectory temporary;
    const std::string payload = "validation-image";
    write_bytes(temporary.path() / "images/image.img", payload);
    auto plan = make_plan(
        serials(1U),
        {reboot_step(), erase_step("userdata")}, payload);
    auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
    CHECK(artifacts.has_value());
    auto inputs = prepare_devices(plan, 1U);
    auto rejected = prepare_fleet_device_actors(
        plan, std::move(*artifacts), std::move(*inputs.devices));
    CHECK(!rejected);
    CHECK(rejected.error().kind == FleetActorPrepareErrorKind::InvalidPlan);
    CHECK(commands(inputs.states.front()).empty());
    CHECK(closed(inputs.states.front()));

    auto invalid = make_plan(
        serials(1U), {oem_step("bad\ncommand")}, payload, "invalid-oem");
    auto invalid_artifacts =
        preflight_fleet_artifacts(invalid, temporary.path());
    CHECK(invalid_artifacts.has_value());
    auto invalid_inputs = prepare_devices(invalid, 1U);
    auto invalid_result = prepare_fleet_device_actors(
        invalid, std::move(*invalid_artifacts),
        std::move(*invalid_inputs.devices));
    CHECK(!invalid_result);
    CHECK(commands(invalid_inputs.states.front()).empty());
    CHECK(closed(invalid_inputs.states.front()));
}

void failure_cancel_and_poison_stop_suffix_and_close() {
    TemporaryDirectory temporary;
    const std::string payload = "suffix-image";
    write_bytes(temporary.path() / "images/image.img", payload);
    const auto steps = std::vector<ManifestStep>{
        erase_step("userdata"), oem_step("must-not-run"), reboot_step()};

    {
        auto plan = make_plan(serials(1U), steps, payload, "device-fail");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->fail_command = "erase:userdata";
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        auto executed = batch->execute_device(0U);
        CHECK(!executed);
        CHECK(executed.error().kind ==
              FleetActorExecutionErrorKind::DeviceTaskFailed);
        CHECK(commands(inputs.states.front()) ==
              std::vector<std::string>{"erase:userdata"});
        CHECK(closed(inputs.states.front()));
    }

    {
        auto plan = make_plan(serials(1U), steps, payload, "poison");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->poison_command = "erase:userdata";
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        auto executed = batch->execute_device(0U);
        CHECK(!executed);
        CHECK(executed.error().device_error.has_value());
        CHECK(executed.error().device_error->outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(commands(inputs.states.front()) ==
              std::vector<std::string>{"erase:userdata"});
        CHECK(closed(inputs.states.front()));
    }

    {
        auto plan = make_plan(serials(1U), steps, payload, "cancel");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        std::stop_source cancellation;
        cancellation.request_stop();
        auto executed = batch->execute_device(
            0U,
            UpdateOperationContext{
                .cancellation = cancellation.get_token(),
                .deadline = std::nullopt});
        CHECK(!executed);
        CHECK(executed.error().kind ==
              FleetActorExecutionErrorKind::Cancelled);
        CHECK(commands(inputs.states.front()).empty());
        CHECK(closed(inputs.states.front()));
    }

    {
        auto plan = make_plan(serials(1U), steps, payload, "timeout");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        auto executed = batch->execute_device(
            0U,
            UpdateOperationContext{
                .cancellation = {},
                .deadline = std::chrono::steady_clock::now()});
        CHECK(!executed);
        CHECK(executed.error().kind ==
              FleetActorExecutionErrorKind::TimedOut);
        CHECK(commands(inputs.states.front()).empty());
        CHECK(closed(inputs.states.front()));
    }

    {
        auto plan = make_plan(serials(1U), steps, payload, "observer");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        auto executed = batch->execute_device(
            0U, {}, [](const kairosboot::fleet::FleetActorExecutionEvent& event) {
                if (event.kind == kairosboot::fleet::
                                      FleetActorExecutionEventKind::StepCompleted) {
                    throw std::runtime_error("stop after first step");
                }
            });
        CHECK(!executed);
        CHECK(executed.error().kind ==
              FleetActorExecutionErrorKind::ObserverFailed);
        CHECK(executed.error().completed_steps == 1U);
        CHECK(commands(inputs.states.front()) ==
              std::vector<std::string>{"erase:userdata"});
        CHECK(closed(inputs.states.front()));
    }
}

void concurrent_execute_is_rejected_and_destructor_releases_session() {
    TemporaryDirectory temporary;
    const std::string payload = "concurrency-image";
    write_bytes(temporary.path() / "images/image.img", payload);
    auto plan = make_plan(
        serials(1U), {erase_step("userdata")}, payload);
    auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
    CHECK(artifacts.has_value());
    auto inputs = prepare_devices(plan, 1U);
    inputs.states.front()->block_command = "erase:userdata";
    auto batch = prepare_fleet_device_actors(
        plan, std::move(*artifacts), std::move(*inputs.devices));
    CHECK(batch.has_value());

    std::optional<kairosboot::fleet::FleetActorDeviceExecution> first_value;
    std::optional<kairosboot::fleet::FleetActorExecutionError> first_error;
    std::thread first([&] {
        auto result = batch->execute_device(0U);
        if (result) {
            first_value = std::move(*result);
        } else {
            first_error = std::move(result.error());
        }
    });
    {
        std::unique_lock lock(inputs.states.front()->mutex);
        inputs.states.front()->condition.wait(
            lock, [&] { return inputs.states.front()->blocked; });
    }
    auto concurrent = batch->execute_device(0U);
    CHECK(!concurrent);
    CHECK(concurrent.error().kind == FleetActorExecutionErrorKind::Busy);
    {
        std::lock_guard lock(inputs.states.front()->mutex);
        inputs.states.front()->release_block = true;
        inputs.states.front()->condition.notify_all();
    }
    first.join();
    CHECK(first_value.has_value());
    CHECK(!first_error.has_value());
    CHECK(closed(inputs.states.front()));

    auto destructor_plan = make_plan(
        serials(1U), {erase_step("metadata")}, payload, "destructor");
    auto destructor_artifacts =
        preflight_fleet_artifacts(destructor_plan, temporary.path());
    CHECK(destructor_artifacts.has_value());
    auto destructor_inputs = prepare_devices(destructor_plan, 1U);
    {
        auto unexecuted = prepare_fleet_device_actors(
            destructor_plan, std::move(*destructor_artifacts),
            std::move(*destructor_inputs.devices));
        CHECK(unexecuted.has_value());
        CHECK(!closed(destructor_inputs.states.front()));
    }
    CHECK(closed(destructor_inputs.states.front()));
}

struct RaceOutcome final {
    std::optional<kairosboot::fleet::FleetActorDeviceExecution> value;
    std::optional<kairosboot::fleet::FleetActorExecutionError> error;
};

// Runs execute_device on a worker thread while the scripted transport is
// blocked inside the write of one command, then applies the caller's race
// action (request_stop, deadline expiry, manual release) and joins.
[[nodiscard]] RaceOutcome execute_racing_blocked_command(
    PreparedFleetActorBatch& batch,
    const std::shared_ptr<TransportState>& state,
    const UpdateOperationContext context,
    const std::function<void()>& after_blocked) {
    RaceOutcome outcome;
    std::thread worker([&] {
        auto result = batch.execute_device(0U, context);
        if (result) {
            outcome.value = std::move(*result);
        } else {
            outcome.error = std::move(result.error());
        }
    });
    {
        std::unique_lock lock(state->mutex);
        state->condition.wait(lock, [&] { return state->blocked; });
    }
    after_blocked();
    worker.join();
    return outcome;
}

[[nodiscard]] bool cancellation_requested(
    const std::shared_ptr<TransportState>& state) {
    std::lock_guard lock(state->mutex);
    return state->cancellation_requested;
}

void slot_all_second_child_failures_report_completed_prefix() {
    TemporaryDirectory temporary;
    const std::string payload = "prefix-image";
    write_bytes(temporary.path() / "images/image.img", payload);
    const auto steps = std::vector<ManifestStep>{
        flash_step("boot", "image", ManifestFlashSlot::All),
        erase_step("never"),
    };

    {
        auto plan = make_plan(serials(1U), steps, payload, "second-fail");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->fail_command = "flash:boot_b";
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        auto executed = batch->execute_device(0U);
        CHECK(!executed);
        CHECK(executed.error().kind ==
              FleetActorExecutionErrorKind::DeviceTaskFailed);
        CHECK(executed.error().step_index == 0U);
        CHECK(executed.error().completed_steps == 0U);
        CHECK(executed.error().completed_data_bytes == payload.size());
        CHECK(executed.error().completed_child_tasks_in_step == 1U);
        CHECK(executed.error().total_child_tasks_in_step == 2U);
        CHECK(executed.error().device_error.has_value());
        CHECK(!executed.error().device_error->session_poisoned);
        CHECK(!cancellation_requested(inputs.states.front()));
        const auto trace = commands(inputs.states.front());
        CHECK(std::ranges::count(trace, "flash:boot_a") == 1);
        CHECK(std::ranges::count(trace, "flash:boot_b") == 1);
        CHECK(std::ranges::count(trace, "erase:never") == 0);
        CHECK(closed(inputs.states.front()));
    }

    {
        auto plan = make_plan(serials(1U), steps, payload, "second-cancel");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->block_command = "flash:boot_b";
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        std::stop_source cancellation;
        auto outcome = execute_racing_blocked_command(
            *batch, inputs.states.front(),
            UpdateOperationContext{
                .cancellation = cancellation.get_token(),
                .deadline = std::nullopt},
            [&cancellation] { cancellation.request_stop(); });
        CHECK(!outcome.value.has_value());
        CHECK(outcome.error.has_value());
        CHECK(outcome.error->kind == FleetActorExecutionErrorKind::Cancelled);
        CHECK(outcome.error->step_index == 0U);
        CHECK(outcome.error->completed_steps == 0U);
        CHECK(outcome.error->completed_data_bytes == payload.size());
        CHECK(outcome.error->completed_child_tasks_in_step == 1U);
        CHECK(outcome.error->total_child_tasks_in_step == 2U);
        CHECK(outcome.error->device_error.has_value());
        CHECK(outcome.error->device_error->session_poisoned);
        CHECK(cancellation_requested(inputs.states.front()));
        const auto trace = commands(inputs.states.front());
        CHECK(std::ranges::count(trace, "flash:boot_a") == 1);
        CHECK(std::ranges::count(trace, "flash:boot_b") == 1);
        CHECK(std::ranges::count(trace, "erase:never") == 0);
        CHECK(closed(inputs.states.front()));
    }

    {
        auto plan = make_plan(serials(1U), steps, payload, "second-deadline");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->block_command = "flash:boot_b";
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        const auto state = inputs.states.front();
        auto outcome = execute_racing_blocked_command(
            *batch, state,
            UpdateOperationContext{
                .cancellation = {},
                .deadline = std::chrono::steady_clock::now() + 50ms},
            [state] {
                std::this_thread::sleep_for(120ms);
                std::lock_guard lock(state->mutex);
                state->release_block = true;
                state->condition.notify_all();
            });
        CHECK(!outcome.value.has_value());
        CHECK(outcome.error.has_value());
        CHECK(outcome.error->kind == FleetActorExecutionErrorKind::TimedOut);
        CHECK(outcome.error->step_index == 0U);
        CHECK(outcome.error->completed_steps == 0U);
        CHECK(outcome.error->completed_data_bytes == payload.size());
        CHECK(outcome.error->completed_child_tasks_in_step == 1U);
        CHECK(outcome.error->total_child_tasks_in_step == 2U);
        CHECK(outcome.error->device_error.has_value());
        CHECK(!outcome.error->device_error->session_poisoned);
        CHECK(!cancellation_requested(state));
        const auto trace = commands(state);
        CHECK(std::ranges::count(trace, "flash:boot_a") == 1);
        CHECK(std::ranges::count(trace, "flash:boot_b") == 1);
        CHECK(std::ranges::count(trace, "erase:never") == 0);
        CHECK(closed(state));
    }
}

void primitive_commands_report_single_action_on_failure() {
    TemporaryDirectory temporary;
    const std::string payload = "single-action-image";
    write_bytes(temporary.path() / "images/image.img", payload);

    const std::vector<
        std::tuple<const char*, std::vector<ManifestStep>, std::string>>
        cases{
            {"set-active",
             {active_step(ManifestActiveSlot::Other), erase_step("never")},
             "set_active:b"},
            {"oem",
             {oem_step("unlock-go"), erase_step("never")},
             "oem unlock-go"},
            {"reboot",
             {reboot_step(ManifestRebootTarget::Bootloader)},
             "reboot-bootloader"},
        };
    for (const auto& [label, steps, command] : cases) {
        auto plan = make_plan(serials(1U), steps, payload, label);
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->fail_command = command;
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        auto executed = batch->execute_device(0U);
        CHECK(!executed);
        CHECK(executed.error().kind ==
              FleetActorExecutionErrorKind::DeviceTaskFailed);
        CHECK(executed.error().step_index == 0U);
        CHECK(executed.error().completed_steps == 0U);
        CHECK(executed.error().completed_data_bytes == 0U);
        CHECK(executed.error().completed_child_tasks_in_step == 0U);
        CHECK(executed.error().total_child_tasks_in_step == 1U);
        CHECK(executed.error().device_error.has_value());
        const auto& device_error = *executed.error().device_error;
        CHECK(device_error.kind ==
              kairosboot::fastboot::UpdateDeviceErrorKind::Failed);
        CHECK(device_error.device_message == "injected command failure");
        CHECK(device_error.completed_actions == 0U);
        CHECK(device_error.total_actions == 1U);
        CHECK(device_error.task_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(!device_error.session_poisoned);
        CHECK(!device_error.session_closed);
        CHECK(!cancellation_requested(inputs.states.front()));
        const auto trace = commands(inputs.states.front());
        CHECK(std::ranges::count(trace, command) == 1);
        CHECK(std::ranges::count(trace, "erase:never") == 0);
        CHECK(closed(inputs.states.front()));
    }
}

void primitive_command_cancellation_races_preserve_sticky_poisoning() {
    TemporaryDirectory temporary;
    const std::string payload = "sticky-cancel-image";
    write_bytes(temporary.path() / "images/image.img", payload);

    const std::vector<
        std::tuple<const char*, std::vector<ManifestStep>, std::string>>
        cases{
            {"set-active",
             {active_step(ManifestActiveSlot::Other), erase_step("never")},
             "set_active:b"},
            {"oem",
             {oem_step("unlock-go"), erase_step("never")},
             "oem unlock-go"},
            {"reboot",
             {reboot_step(ManifestRebootTarget::Bootloader)},
             "reboot-bootloader"},
        };

    // The command completes with OKAY while a cancellation wins the race
    // into the primitive call: the result is Cancelled, the session is
    // poisoned, and the single action still counts as completed.
    for (const auto& [label, steps, command] : cases) {
        auto plan = make_plan(serials(1U), steps, payload, label);
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->block_command = command;
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        std::stop_source cancellation;
        auto outcome = execute_racing_blocked_command(
            *batch, inputs.states.front(),
            UpdateOperationContext{
                .cancellation = cancellation.get_token(),
                .deadline = std::nullopt},
            [&cancellation] { cancellation.request_stop(); });
        CHECK(!outcome.value.has_value());
        CHECK(outcome.error.has_value());
        CHECK(outcome.error->kind == FleetActorExecutionErrorKind::Cancelled);
        CHECK(outcome.error->step_index == 0U);
        CHECK(outcome.error->completed_steps == 0U);
        CHECK(outcome.error->completed_data_bytes == 0U);
        CHECK(outcome.error->device_error.has_value());
        const auto& device_error = *outcome.error->device_error;
        CHECK(device_error.kind ==
              kairosboot::fastboot::UpdateDeviceErrorKind::Cancelled);
        CHECK(device_error.completed_actions == 1U);
        CHECK(device_error.total_actions == 1U);
        CHECK(device_error.task_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(device_error.session_poisoned);
        CHECK(device_error.session_closed ==
              (std::string_view(label) == "reboot"));
        CHECK(cancellation_requested(inputs.states.front()));
        const auto trace = commands(inputs.states.front());
        CHECK(std::ranges::count(trace, command) == 1);
        CHECK(std::ranges::count(trace, "erase:never") == 0);
        CHECK(closed(inputs.states.front()));
    }

    // The device FAILs while the same cancellation is forwarded: the coarse
    // FAIL is refined to Cancelled, the FAIL text survives, and the sticky
    // session poisoning is retained even though no action completed.
    for (const auto& [label, steps, command] : cases) {
        auto plan = make_plan(serials(1U), steps, payload, label);
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->block_command = command;
        inputs.states.front()->fail_command = command;
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        std::stop_source cancellation;
        auto outcome = execute_racing_blocked_command(
            *batch, inputs.states.front(),
            UpdateOperationContext{
                .cancellation = cancellation.get_token(),
                .deadline = std::nullopt},
            [&cancellation] { cancellation.request_stop(); });
        CHECK(!outcome.value.has_value());
        CHECK(outcome.error.has_value());
        CHECK(outcome.error->kind == FleetActorExecutionErrorKind::Cancelled);
        CHECK(outcome.error->device_error.has_value());
        const auto& device_error = *outcome.error->device_error;
        CHECK(device_error.kind ==
              kairosboot::fastboot::UpdateDeviceErrorKind::Cancelled);
        CHECK(device_error.device_message == "injected command failure");
        CHECK(device_error.completed_actions == 0U);
        CHECK(device_error.total_actions == 1U);
        CHECK(device_error.task_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(device_error.session_poisoned);
        CHECK(cancellation_requested(inputs.states.front()));
        const auto trace = commands(inputs.states.front());
        CHECK(std::ranges::count(trace, command) == 1);
        CHECK(std::ranges::count(trace, "erase:never") == 0);
        CHECK(closed(inputs.states.front()));
    }

    // A cancellation arriving after the task's stop callback was destroyed
    // is never forwarded: the actor's next step-boundary check reports
    // Cancelled without fabricating device evidence or poisoning the
    // session.
    {
        auto plan = make_plan(
            serials(1U),
            {active_step(ManifestActiveSlot::Other), erase_step("never")},
            payload, "late-cancel");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        std::stop_source cancellation;
        auto executed = batch->execute_device(
            0U,
            UpdateOperationContext{
                .cancellation = cancellation.get_token(),
                .deadline = std::nullopt},
            [&cancellation](const kairosboot::fleet::
                                FleetActorExecutionEvent& event) {
                if (event.kind == kairosboot::fleet::
                                     FleetActorExecutionEventKind::
                                         StepCompleted) {
                    cancellation.request_stop();
                }
            });
        CHECK(!executed);
        CHECK(executed.error().kind ==
              FleetActorExecutionErrorKind::Cancelled);
        CHECK(executed.error().step_index == 1U);
        CHECK(executed.error().completed_steps == 1U);
        CHECK(executed.error().completed_data_bytes == 0U);
        CHECK(!executed.error().device_error.has_value());
        CHECK(!cancellation_requested(inputs.states.front()));
        const auto trace = commands(inputs.states.front());
        CHECK(std::ranges::count(trace, "set_active:b") == 1);
        CHECK(std::ranges::count(trace, "erase:never") == 0);
        CHECK(closed(inputs.states.front()));
    }

    // A deadline expiring after the command's OKAY response reports TimedOut
    // with the action completed and never poisons the session.
    {
        auto plan = make_plan(
            serials(1U),
            {active_step(ManifestActiveSlot::Other), erase_step("never")},
            payload, "late-deadline");
        auto artifacts = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(artifacts.has_value());
        auto inputs = prepare_devices(plan, 1U);
        inputs.states.front()->block_command = "set_active:b";
        auto batch = prepare_fleet_device_actors(
            plan, std::move(*artifacts), std::move(*inputs.devices));
        CHECK(batch.has_value());
        const auto state = inputs.states.front();
        auto outcome = execute_racing_blocked_command(
            *batch, state,
            UpdateOperationContext{
                .cancellation = {},
                .deadline = std::chrono::steady_clock::now() + 50ms},
            [state] {
                std::this_thread::sleep_for(120ms);
                std::lock_guard lock(state->mutex);
                state->release_block = true;
                state->condition.notify_all();
            });
        CHECK(!outcome.value.has_value());
        CHECK(outcome.error.has_value());
        CHECK(outcome.error->kind == FleetActorExecutionErrorKind::TimedOut);
        CHECK(outcome.error->device_error.has_value());
        const auto& device_error = *outcome.error->device_error;
        CHECK(device_error.kind ==
              kairosboot::fastboot::UpdateDeviceErrorKind::TimedOut);
        CHECK(device_error.completed_actions == 1U);
        CHECK(device_error.total_actions == 1U);
        CHECK(device_error.task_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(!device_error.session_poisoned);
        CHECK(!cancellation_requested(state));
        const auto trace = commands(state);
        CHECK(std::ranges::count(trace, "set_active:b") == 1);
        CHECK(std::ranges::count(trace, "erase:never") == 0);
        CHECK(closed(state));
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"digest mismatch", digest_mismatch_does_not_unwrap_devices},
        {"sealed source and five operations",
         sealed_source_and_five_operation_trace},
        {"sparse and resparse bytes",
         sparse_and_resparse_report_exact_data_bytes},
        {"all flash slot forms",
         every_flash_slot_form_is_frozen_during_prepare},
        {"global preparation barrier",
         late_preparation_failure_has_no_destructive_wire_effect},
        {"terminal reboot and OEM validation",
         reboot_must_be_last_and_oem_validation_is_prepare_only},
        {"failure cancellation poison suffix",
         failure_cancel_and_poison_stop_suffix_and_close},
        {"concurrent execute and lifetime",
         concurrent_execute_is_rejected_and_destructor_releases_session},
        {"slot all second child failure prefix",
         slot_all_second_child_failures_report_completed_prefix},
        {"primitive command single-action failure",
         primitive_commands_report_single_action_on_failure},
        {"primitive command cancellation races",
         primitive_command_cancellation_races_preserve_sticky_poisoning},
    };

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
