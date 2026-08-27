// SPDX-License-Identifier: MIT
#include "src/fastboot/primitive_update_device.hpp"
#include "tests/protocol/scripted_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using kairosboot::fastboot::execute_prepared_update;
using kairosboot::fastboot::bind_initial_reconnect_session;
using kairosboot::fastboot::FastbootUsbMode;
using kairosboot::fastboot::IReconnectDiscovery;
using kairosboot::fastboot::IReconnectSessionOpener;
using kairosboot::fastboot::IReconnectWaiter;
using kairosboot::fastboot::IPreparedDeviceTask;
using kairosboot::fastboot::map_primitive_update_error;
using kairosboot::fastboot::PlannedRebootTarget;
using kairosboot::fastboot::PlannedSlot;
using kairosboot::fastboot::PlannedUpdateTask;
using kairosboot::fastboot::PreparedSuperArtifact;
using kairosboot::fastboot::PreparedUpdateArtifact;
using kairosboot::fastboot::PreparedUpdatePackage;
using kairosboot::fastboot::PrimitiveError;
using kairosboot::fastboot::PrimitiveErrorCode;
using kairosboot::fastboot::PrimitiveOperation;
using kairosboot::fastboot::PrimitiveService;
using kairosboot::fastboot::PrimitiveUpdateDevice;
using kairosboot::fastboot::PrimitiveUpdateDeviceOptions;
using kairosboot::fastboot::PrimitiveUpdateProgress;
using kairosboot::fastboot::PrimitiveUpdateProgressAction;
using kairosboot::fastboot::OpenedReconnectSession;
using kairosboot::fastboot::ReconnectCandidate;
using kairosboot::fastboot::ReconnectCoordinator;
using kairosboot::fastboot::ReconnectDeviceIdentity;
using kairosboot::fastboot::ReconnectDiscoveryError;
using kairosboot::fastboot::ReconnectOpenError;
using kairosboot::fastboot::ReconnectOptions;
using kairosboot::fastboot::ReconnectTarget;
using kairosboot::fastboot::ReconnectTimePoint;
using kairosboot::fastboot::ReconnectUsbFingerprint;
using kairosboot::fastboot::ReconnectWaitResult;
using kairosboot::fastboot::ReconnectWaitStatus;
using kairosboot::fastboot::UsbPhysicalPortPath;
using kairosboot::fastboot::UpdateDeviceErrorKind;
using kairosboot::fastboot::UpdateDeviceTaskInput;
using kairosboot::fastboot::UpdateExecutionErrorKind;
using kairosboot::fastboot::UpdateExecutorOptions;
using kairosboot::fastboot::UpdateOperationContext;
using kairosboot::fastboot::UpdateSuperPreparationState;
using kairosboot::fastboot::UpdateTaskKind;
using kairosboot::image::ArtifactSourceOrigin;
using kairosboot::image::FlashArtifact;
using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::ResolvedArtifact;
using kairosboot::image::SparseFlashPlan;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::ProtocolPhase;
using kairosboot::protocol::Response;
using kairosboot::protocol::ResponseKind;
using kairosboot::protocol::SessionState;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransportStatus;
using kairosboot::protocol::test::ScriptedTransport;
using kairosboot::protocol::test::to_bytes;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                               \
    do {                                                                               \
        if (!(condition)) {                                                            \
            throw CheckFailure(std::string("check failed at line ") +                  \
                               std::to_string(__LINE__) + ": " #condition);            \
        }                                                                              \
    } while (false)

class MemorySource : public IImageSource {
public:
    explicit MemorySource(std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        ++read_count_;
        if (destination.empty() || offset >= bytes_.size()) {
            return std::size_t{0};
        }
        const auto amount = std::min(
            destination.size(), bytes_.size() - static_cast<std::size_t>(offset));
        std::ranges::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset), amount,
            destination.begin());
        return amount;
    }

    [[nodiscard]] std::size_t read_count() const noexcept { return read_count_; }

private:
    std::vector<std::byte> bytes_;
    mutable std::size_t read_count_{};
};

class DeclaredSizeSource final : public IImageSource {
public:
    explicit DeclaredSizeSource(const std::uint64_t size) : size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset >= size_ || destination.empty()) {
            return std::size_t{0};
        }
        const auto available = size_ - offset;
        const auto amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(available, destination.size()));
        std::fill_n(destination.begin(), amount, std::byte{0});
        return amount;
    }

private:
    std::uint64_t size_{};
};

class StopAfterReadTransport final
    : public kairosboot::protocol::ITransportSession,
      public kairosboot::protocol::IStreamingTransportSession {
public:
    StopAfterReadTransport(std::stop_source& cancellation,
                           const std::size_t trigger_read) noexcept
        : cancellation_(cancellation), trigger_read_(trigger_read) {}

    [[nodiscard]] ScriptedTransport& script() noexcept { return script_; }

    [[nodiscard]] kairosboot::protocol::TransferResult write(
        const std::span<const std::byte> bytes,
        const std::chrono::milliseconds timeout) override {
        return script_.write(bytes, timeout);
    }

    [[nodiscard]] kairosboot::protocol::TransferResult read(
        const std::span<std::byte> destination,
        const std::chrono::milliseconds timeout) override {
        auto result = script_.read(destination, timeout);
        ++read_count_;
        if (read_count_ == trigger_read_) {
            cancellation_.request_stop();
        }
        return result;
    }

    [[nodiscard]] kairosboot::protocol::TransferResult read_data(
        const std::span<std::byte> destination,
        const std::chrono::milliseconds timeout) override {
        return script_.read_data(destination, timeout);
    }

    [[nodiscard]] kairosboot::protocol::TransferResult write_source(
        std::shared_ptr<kairosboot::protocol::ITransferSource> source,
        const std::chrono::milliseconds timeout,
        const kairosboot::protocol::TransferProgressObserver& observer = {})
        override {
        return script_.write_source(std::move(source), timeout, observer);
    }

    void request_cancel() noexcept override { script_.request_cancel(); }
    void close() noexcept override { script_.close(); }

private:
    ScriptedTransport script_;
    std::stop_source& cancellation_;
    std::size_t trigger_read_{};
    std::size_t read_count_{};
};

[[nodiscard]] UsbPhysicalPortPath actor_port(
    std::vector<std::uint8_t> ports = {2U, 3U}) {
    return UsbPhysicalPortPath{
        .bus_number = 1U,
        .ports = std::move(ports),
    };
}

[[nodiscard]] ReconnectUsbFingerprint actor_fingerprint() {
    return ReconnectUsbFingerprint{
        .vendor_id = 0x18D1U,
        .product_id = 0x4EE0U,
        .interface_number = 0U,
        .interface_class = 0xFFU,
        .interface_subclass = 0x42U,
        .interface_protocol = 0x03U,
    };
}

[[nodiscard]] ReconnectTarget actor_target() {
    return ReconnectTarget{
        .physical_port = actor_port(),
        .serial = std::string{"SERIAL-A"},
        .usb_fingerprint = actor_fingerprint(),
        .product = "product_a",
        .previous_mode = FastbootUsbMode::Bootloader,
        .required_mode = FastbootUsbMode::Fastbootd,
        .preceding_operation_certainty = TransferCertainty::FullyTransferred,
    };
}

[[nodiscard]] ReconnectCandidate actor_candidate(
    UsbPhysicalPortPath physical_port,
    std::optional<std::string> serial = std::string{"SERIAL-A"}) {
    return ReconnectCandidate{
        .physical_port = std::move(physical_port),
        .serial = std::move(serial),
        .usb_fingerprint = actor_fingerprint(),
    };
}

[[nodiscard]] ReconnectDeviceIdentity actor_identity(
    UsbPhysicalPortPath physical_port = actor_port(),
    std::optional<std::string> serial = std::string{"SERIAL-A"},
    std::string product = "product_a",
    const FastbootUsbMode mode = FastbootUsbMode::Fastbootd) {
    return ReconnectDeviceIdentity{
        .physical_port = std::move(physical_port),
        .serial = std::move(serial),
        .usb_fingerprint = actor_fingerprint(),
        .product = std::move(product),
        .mode = mode,
    };
}

using ActorDiscoveryResult = std::expected<
    std::vector<ReconnectCandidate>, ReconnectDiscoveryError>;

class ActorDiscovery final : public IReconnectDiscovery {
public:
    std::vector<ActorDiscoveryResult> steps;
    std::vector<ReconnectTimePoint> deadlines;
    std::size_t calls{};

    [[nodiscard]] ActorDiscoveryResult discover(
        const ReconnectTimePoint deadline,
        const std::stop_token) override {
        ++calls;
        deadlines.push_back(deadline);
        if (calls <= steps.size()) {
            return steps[calls - 1U];
        }
        return std::vector<ReconnectCandidate>{};
    }
};

class ActorOpener final : public IReconnectSessionOpener {
public:
    std::vector<std::optional<ReconnectOpenError>> errors;
    std::vector<ReconnectDeviceIdentity> identities;
    std::vector<std::unique_ptr<kairosboot::protocol::ITransportSession>>
        transports;
    std::vector<ReconnectCandidate> candidates;
    std::vector<ReconnectTimePoint> deadlines;

    [[nodiscard]] std::expected<OpenedReconnectSession, ReconnectOpenError>
    open(const ReconnectCandidate& passive,
         const ReconnectTimePoint deadline,
         const std::stop_token) override {
        candidates.push_back(passive);
        deadlines.push_back(deadline);
        const auto index = candidates.size() - 1U;
        if (index < errors.size() && errors[index]) {
            return std::unexpected(*errors[index]);
        }
        if (index >= identities.size() || index >= transports.size() ||
            transports[index] == nullptr) {
            return std::unexpected(ReconnectOpenError{
                .message = "scripted opener has no replacement session",
                .retryable = false,
                .outbound_certainty = TransferCertainty::NotTransferred,
            });
        }
        return OpenedReconnectSession{
            .verified_identity = identities[index],
            .session = std::make_unique<FastbootSession>(
                std::move(transports[index])),
            .outbound_certainty = TransferCertainty::FullyTransferred,
        };
    }
};

class ActorWaiter final : public IReconnectWaiter {
public:
    TimePoint current{std::chrono::steady_clock::now()};
    std::vector<std::chrono::milliseconds> waits;
    std::stop_source* cancel_source{};

    [[nodiscard]] TimePoint now() const noexcept override {
        return current;
    }

    [[nodiscard]] ReconnectWaitResult wait_for(
        const std::chrono::milliseconds duration,
        const std::stop_token) override {
        waits.push_back(duration);
        current += duration;
        if (cancel_source != nullptr) {
            cancel_source->request_stop();
            return ReconnectWaitResult{
                .status = ReconnectWaitStatus::Cancelled,
                .message = "scripted reconnect cancellation",
            };
        }
        return ReconnectWaitResult{
            .status = ReconnectWaitStatus::Elapsed,
        };
    }
};

[[nodiscard]] ReconnectOptions actor_reconnect_options() {
    return ReconnectOptions{
        .initial_backoff = 5ms,
        .maximum_backoff = 10ms,
        .maximum_discovered_devices = 32U,
        .maximum_discovery_attempts = 8U,
        .maximum_open_attempts = 8U,
    };
}

struct ReconnectDeviceFixture final {
    std::unique_ptr<PrimitiveUpdateDevice> device;
    FastbootSession* initial_session{};
};

[[nodiscard]] ReconnectDeviceFixture make_reconnect_device(
    std::unique_ptr<ITransportSession> transport,
    ReconnectCoordinator& coordinator,
    ReconnectTarget target = actor_target(),
    ReconnectOptions options = actor_reconnect_options()) {
    auto session = std::make_unique<FastbootSession>(std::move(transport));
    auto* session_pointer = session.get();
    auto identity = ReconnectDeviceIdentity{
        .physical_port = target.physical_port,
        .serial = target.serial,
        .usb_fingerprint = target.usb_fingerprint,
        .product = target.product,
        .mode = target.previous_mode,
    };
    auto binding = bind_initial_reconnect_session(
        OpenedReconnectSession{
            .verified_identity = std::move(identity),
            .session = std::move(session),
            .outbound_certainty = TransferCertainty::FullyTransferred,
        },
        target);
    CHECK(binding);
    auto device = PrimitiveUpdateDevice::create_with_reconnect(
        std::move(*binding), coordinator, options);
    CHECK(device);
    return {
        .device = std::move(*device),
        .initial_session = session_pointer,
    };
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
}

[[nodiscard]] std::vector<std::byte> one_block_sparse_image() {
    constexpr std::uint32_t block_size = 4096U;
    std::vector<std::byte> bytes;
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
        bytes.push_back(static_cast<std::byte>((index * 13U + 7U) % 251U));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> three_block_raw_image() {
    std::vector<std::byte> bytes(3U * 4096U);
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = std::byte{
            static_cast<unsigned char>((index * 17U + 3U) % 251U)};
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> read_image_source(
    const IImageSource& source) {
    CHECK(source.size() <= std::numeric_limits<std::size_t>::max());
    std::vector<std::byte> bytes(static_cast<std::size_t>(source.size()));
    std::size_t completed = 0U;
    while (completed < bytes.size()) {
        auto read = source.read_at(completed, std::span(bytes).subspan(completed));
        CHECK(read);
        CHECK(*read != 0U);
        CHECK(*read <= bytes.size() - completed);
        completed += *read;
    }
    return bytes;
}

struct BoundArtifact final {
    std::shared_ptr<const ResolvedArtifact> resolved;
    std::shared_ptr<const FlashArtifact> artifact;
};

[[nodiscard]] BoundArtifact bind_artifact(
    std::string name,
    std::shared_ptr<const IImageSource> source) {
    auto inspected = FlashArtifact::inspect(source);
    CHECK(inspected);
    auto resolved = std::make_shared<const ResolvedArtifact>(ResolvedArtifact{
        .source = source,
        .origin = ArtifactSourceOrigin::DirectoryEntry,
        .logical_name = std::move(name),
    });
    auto artifact =
        std::make_shared<const FlashArtifact>(std::move(*inspected));
    return {
        .resolved = std::move(resolved),
        .artifact = std::move(artifact),
    };
}

[[nodiscard]] UpdateDeviceTaskInput flash_input(
    const BoundArtifact& artifact,
    std::string partition = "system",
    std::string name = "system.img",
    const PlannedSlot slot = PlannedSlot::Default) {
    return {
        .task =
            {
                .kind = UpdateTaskKind::Flash,
                .partition = std::move(partition),
                .artifact = std::move(name),
                .slot = slot,
            },
        .flash_artifact =
            kairosboot::fastboot::UpdateFlashArtifactInput{
                .resolved = artifact.resolved,
                .artifact = artifact.artifact,
            },
    };
}

[[nodiscard]] UpdateDeviceTaskInput update_super_input(
    const BoundArtifact& artifact,
    const bool wants_wipe = false) {
    return {
        .task = {.kind = UpdateTaskKind::UpdateSuper},
        .super_artifact = std::make_shared<const PreparedSuperArtifact>(
            artifact.resolved, artifact.artifact, wants_wipe),
    };
}

[[nodiscard]] std::string download_command(const std::uint64_t size) {
    std::ostringstream stream;
    stream << "download:" << std::hex << std::setw(8) << std::setfill('0')
           << size;
    return stream.str();
}

[[nodiscard]] bool accepted_contains(
    const ScriptedTransport& script,
    const std::string_view bytes) {
    const auto wanted = to_bytes(bytes);
    const auto& accepted = script.accepted_bytes();
    return std::search(accepted.begin(), accepted.end(), wanted.begin(),
                       wanted.end()) != accepted.end();
}

void expect_flash(
    ScriptedTransport& script,
    const std::string_view partition,
    const std::span<const std::byte> payload,
    std::vector<ScriptedTransport::SourceRead> reads = {}) {
    if (reads.empty()) {
        reads.push_back({
            .size = payload.size(),
            .progress_watermark = payload.size(),
        });
    }
    const auto command = download_command(payload.size());
    script.expect_write(command);
    script.respond(std::string{"DATA"} + command.substr(9U));
    script.expect_source_write(payload, std::move(reads));
    script.respond("OKAYdownloaded");
    script.expect_write(std::string{"flash:"} + std::string(partition));
    script.respond("OKAYflashed");
}

void expect_update_super_preparation(
    ScriptedTransport& script,
    const std::string_view super_name_response = "OKAYsuper") {
    script.expect_write("getvar:is-userspace");
    script.respond("OKAYyes");
    script.expect_write("getvar:super-partition-name");
    script.respond(super_name_response);
    script.expect_write("getvar:max-download-size");
    script.respond("OKAY0x100000");
}

void expect_update_super_download(
    ScriptedTransport& script,
    const std::span<const std::byte> payload) {
    script.expect_write("getvar:is-userspace");
    script.respond("OKAYyes");
    const auto command = download_command(payload.size());
    script.expect_write(command);
    script.respond(std::string{"DATA"} + command.substr(9U));
    script.expect_source_write(
        payload,
        {{.size = payload.size(), .progress_watermark = payload.size()}});
    script.respond("OKAYdownloaded");
}

void ordinary_flash_erase_and_reboot_are_complete() {
    const std::vector<std::byte> payload{
        std::byte{0x00}, std::byte{0x7f}, std::byte{0x80}, std::byte{0xff},
        std::byte{0x42}, std::byte{0x00}, std::byte{0x19},
    };
    auto bound = bind_artifact(
        "system.img", std::make_shared<MemorySource>(payload));

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:max-download-size");
    script->respond("OKAY4096");
    expect_flash(*script, "system", payload,
                 {{.offset = 3U, .size = 4U, .progress_watermark = 4U},
                  {.offset = 0U, .size = 3U, .progress_watermark = 7U}});
    script->expect_write("erase:userdata");
    script->respond("OKAYerased");
    script->expect_write("reboot-bootloader");
    script->respond("OKAYrebooting");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);

    auto flash = device.prepare_task(flash_input(bound), {});
    CHECK(flash);
    auto erase = device.prepare_task(
        UpdateDeviceTaskInput{
            .task =
                {
                    .kind = UpdateTaskKind::Erase,
                    .partition = "userdata",
                },
        },
        {});
    CHECK(erase);
    auto reboot = device.prepare_task(
        UpdateDeviceTaskInput{
            .task =
                {
                    .kind = UpdateTaskKind::Reboot,
                    .reboot_target = PlannedRebootTarget::Bootloader,
                },
        },
        {});
    CHECK(reboot);

    CHECK((*flash)->execute({}));
    CHECK((*erase)->execute({}));
    CHECK((*reboot)->execute({}));
    CHECK(script->complete());
    CHECK(script->closed());
    CHECK(session.state() == SessionState::Closed);
}

void slot_other_is_fully_resolved_during_prepare() {
    const auto payload = to_bytes("slot-image");
    auto bound = bind_artifact(
        "boot.img", std::make_shared<MemorySource>(payload));
    const auto userdata_payload = to_bytes("userdata-image");
    auto userdata = bind_artifact(
        "userdata.img", std::make_shared<MemorySource>(userdata_payload));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:has-slot:boot");
    script->respond("OKAYyes");
    script->expect_write("getvar:slot-count");
    script->respond("OKAY2");
    script->expect_write("getvar:current-slot");
    script->respond("OKAYa");
    script->expect_write("getvar:max-download-size");
    script->respond("OKAY4096");
    script->expect_write("getvar:has-slot:userdata");
    script->respond("OKAYno");
    expect_flash(*script, "boot_b", payload);

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);
    auto token = device.prepare_task(
        flash_input(bound, "boot", "boot.img", PlannedSlot::Other), {});
    CHECK(token);
    auto non_slotted = device.prepare_task(
        flash_input(userdata, "userdata", "userdata.img", PlannedSlot::Other),
        {});
    CHECK(!non_slotted);
    CHECK(non_slotted.error().task_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(non_slotted.error().completed_actions == 0U);
    CHECK(non_slotted.error().total_actions == 0U);
    CHECK((*token)->execute({}));
    CHECK(script->complete());
}

void slot_other_legacy_fallback_and_ambiguous_topology_fail_closed() {
    const auto payload = to_bytes("slot-image");
    auto bound = bind_artifact(
        "boot.img", std::make_shared<MemorySource>(payload));
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:has-slot:boot");
        script->respond("OKAYyes");
        script->expect_write("getvar:slot-count");
        script->respond("FAILunknown variable");
        script->expect_write("getvar:slot-suffixes");
        script->respond("OKAY_a,_b");
        script->expect_write("getvar:current-slot");
        script->respond("OKAY_a");
        script->expect_write("getvar:max-download-size");
        script->respond("OKAY4096");
        expect_flash(*script, "boot_b", payload);

        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(
            flash_input(bound, "boot", "boot.img", PlannedSlot::Other), {});
        CHECK(token);
        CHECK((*token)->execute({}));
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:has-slot:boot");
        script->respond("OKAYyes");
        script->expect_write("getvar:slot-count");
        script->respond("OKAY3");

        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(
            flash_input(bound, "boot", "boot.img", PlannedSlot::Other), {});
        CHECK(!token);
        CHECK(token.error().outbound_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(token.error().task_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Ready);
    }
}

[[nodiscard]] PreparedUpdateArtifact package_artifact(
    std::string name,
    std::string_view contents) {
    auto bound = bind_artifact(
        name, std::make_shared<MemorySource>(to_bytes(contents)));
    return {
        .name = std::move(name),
        .resolved = std::move(bound.resolved),
        .artifact = std::move(bound.artifact),
    };
}

void later_prepare_failure_sends_zero_destructive_commands() {
    auto system = package_artifact("system.img", "system-image");
    auto super = package_artifact("super_empty.img", "super-image");
    PreparedUpdatePackage package{
        .plan =
            {
                .tasks =
                    {
                        PlannedUpdateTask{
                            .kind = UpdateTaskKind::Flash,
                            .partition = "system",
                            .artifact = "system.img",
                        },
                        PlannedUpdateTask{
                            .kind = UpdateTaskKind::UpdateSuper,
                        },
                    },
            },
        .artifacts = {system},
        .update_super_state = UpdateSuperPreparationState::Prepared,
        .prepared_super_artifact =
            std::make_shared<const PreparedSuperArtifact>(super.resolved,
                                                          super.artifact),
        .requires_device_validation = false,
    };

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:max-download-size");
    script->respond("OKAY4096");
    script->expect_write("getvar:is-userspace");
    script->respond("OKAYno");
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);

    auto result = execute_prepared_update(package, device);
    CHECK(!result);
    CHECK(result.error().kind == UpdateExecutionErrorKind::DeviceTaskFailed);
    CHECK(result.error().task_index == 1U);
    CHECK(result.error().completed_tasks == 0U);
    CHECK(result.error().device_error);
    CHECK(result.error().device_error->kind ==
          UpdateDeviceErrorKind::Unsupported);
    CHECK(result.error().device_error->outbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void invalid_reconnect_options_block_every_task_before_flash() {
    const auto run = [](const PlannedUpdateTask second_task) {
        auto system = package_artifact("system.img", "system-image");
        PreparedUpdatePackage package{
            .plan = {
                .tasks = {
                    PlannedUpdateTask{
                        .kind = UpdateTaskKind::Flash,
                        .partition = "system",
                        .artifact = "system.img",
                    },
                    second_task,
                },
            },
            .artifacts = {system},
            .requires_device_validation = false,
        };
        if (second_task.kind == UpdateTaskKind::UpdateSuper) {
            auto super = package_artifact(
                "super_empty.img", "immutable-super-image");
            package.update_super_state =
                UpdateSuperPreparationState::Prepared;
            package.prepared_super_artifact =
                std::make_shared<const PreparedSuperArtifact>(
                    super.resolved, super.artifact);
        }

        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:max-download-size");
        script->respond("OKAY4096");
        ActorDiscovery discovery;
        ActorOpener opener;
        ActorWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto invalid_options = actor_reconnect_options();
        invalid_options.initial_backoff = 0ms;
        auto fixture = make_reconnect_device(
            std::move(transport), coordinator, actor_target(),
            invalid_options);

        auto result = execute_prepared_update(package, *fixture.device);
        CHECK(!result);
        CHECK(result.error().kind ==
              UpdateExecutionErrorKind::DeviceTaskFailed);
        CHECK(result.error().task_index == 1U);
        CHECK(result.error().completed_tasks == 0U);
        CHECK(result.error().device_error);
        CHECK(result.error().device_error->outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(result.error().device_error->message.find(
                  "invalid reconnect target or options") !=
              std::string::npos);
        CHECK(script->accepted_bytes() ==
              to_bytes("getvar:max-download-size"));
        CHECK(script->complete());
        CHECK(discovery.calls == 0U);
        CHECK(opener.candidates.empty());
        CHECK(fixture.initial_session->state() == SessionState::Ready);
    };

    run(PlannedUpdateTask{.kind = UpdateTaskKind::UpdateSuper});
    run(PlannedUpdateTask{
        .kind = UpdateTaskKind::Reboot,
        .reboot_target = PlannedRebootTarget::Fastboot,
    });
}

void primitive_error_mapping_preserves_every_diagnostic() {
    PrimitiveError primitive{
        .code = PrimitiveErrorCode::Poisoned,
        .operation = PrimitiveOperation::Fetch,
        .phase = ProtocolPhase::DataRead,
        .message = "transport failed",
        .device_message = "device detail",
        .informational =
            {
                Response{.kind = ResponseKind::Info, .payload = "one"},
                Response{.kind = ResponseKind::Text, .payload = "two"},
            },
        .transport_status = TransportStatus::IoError,
        .transport_certainty = TransferCertainty::PartialOrUnknown,
        .outbound_certainty = TransferCertainty::FullyTransferred,
        .inbound_expected = 8192U,
        .inbound_transferred = 1024U,
        .inbound_certainty = TransferCertainty::PartialOrUnknown,
        .native_code = 73,
        .session_poisoned = true,
    };
    auto mapped = map_primitive_update_error(std::move(primitive));
    CHECK(mapped.kind == UpdateDeviceErrorKind::Failed);
    CHECK(mapped.phase == ProtocolPhase::DataRead);
    CHECK(mapped.message == "transport failed");
    CHECK(mapped.device_message == "device detail");
    CHECK(mapped.informational.size() == 2U);
    CHECK(mapped.informational[0].payload == "one");
    CHECK(mapped.informational[1].kind == ResponseKind::Text);
    CHECK(mapped.transport_status == TransportStatus::IoError);
    CHECK(mapped.transport_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(mapped.outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(mapped.inbound_expected == 8192U);
    CHECK(mapped.inbound_transferred == 1024U);
    CHECK(mapped.inbound_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(mapped.session_poisoned);
    CHECK(!mapped.session_closed);
    CHECK(mapped.native_code == 73);

    auto closed = map_primitive_update_error(PrimitiveError{
        .code = PrimitiveErrorCode::Closed,
        .operation = PrimitiveOperation::Reboot,
        .message = "closed",
    });
    CHECK(closed.session_closed);
}

void actual_device_fail_preserves_info_and_device_message() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:product");
    script->respond("INFOchecking");
    script->respond("FAILdenied");
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);

    auto result = device.getvar("product", {});
    CHECK(!result);
    CHECK(result.error().kind == UpdateDeviceErrorKind::Failed);
    CHECK(result.error().phase == ProtocolPhase::FinalResponse);
    CHECK(result.error().device_message == "denied");
    CHECK(result.error().informational.size() == 1U);
    CHECK(result.error().informational[0].payload == "checking");
    CHECK(result.error().transport_status == TransportStatus::Ok);
    CHECK(result.error().transport_certainty == TransferCertainty::FullyTransferred);
    CHECK(result.error().outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(!result.error().session_poisoned);
    CHECK(result.error().task_certainty == TransferCertainty::NotTransferred);
    CHECK(result.error().completed_actions == 0U);
    CHECK(result.error().total_actions == 0U);
    CHECK(script->complete());
}

void single_primitive_task_certainty_tracks_exact_outbound() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("erase:userdata");
    script->respond("FAILerase denied");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);
    auto token = device.prepare_task(
        UpdateDeviceTaskInput{
            .task = {
                .kind = UpdateTaskKind::Erase,
                .partition = "userdata",
            },
        },
        {});
    CHECK(token);
    auto result = (*token)->execute({});
    CHECK(!result);
    CHECK(result.error().device_message == "erase denied");
    CHECK(result.error().outbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(result.error().task_certainty ==
          result.error().outbound_certainty);
    CHECK(result.error().completed_actions == 0U);
    CHECK(result.error().total_actions == 1U);
    CHECK(script->complete());
}

void multipart_flash_failure_preserves_primitive_and_task_evidence() {
    const auto raw = three_block_raw_image();
    auto bound = bind_artifact(
        "system.img", std::make_shared<MemorySource>(raw));
    auto oracle = SparseFlashPlan::create(*bound.artifact, 4200U);
    CHECK(oracle);
    CHECK(oracle->parts().size() == 3U);
    std::vector<std::vector<std::byte>> encoded_parts;
    encoded_parts.reserve(oracle->parts().size());
    for (const auto& part : oracle->parts()) {
        encoded_parts.push_back(read_image_source(*part.source));
    }

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:max-download-size");
    script->respond("OKAY4200");
    expect_flash(*script, "system", encoded_parts[0]);

    const auto second_download = download_command(encoded_parts[1].size());
    script->expect_write(second_download);
    script->respond(std::string{"DATA"} + second_download.substr(9U));
    script->expect_source_write(
        encoded_parts[1],
        {{.size = encoded_parts[1].size(),
          .progress_watermark = encoded_parts[1].size()}});
    script->respond("OKAYstaged");
    script->expect_write("flash:system");
    script->respond("INFOchecking partition");
    script->respond("FAILpartition denied");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);
    auto token = device.prepare_task(flash_input(bound), {});
    CHECK(token);
    auto result = (*token)->execute({});
    CHECK(!result);
    const auto& error = result.error();
    CHECK(error.kind == UpdateDeviceErrorKind::Failed);
    CHECK(error.phase == ProtocolPhase::FinalResponse);
    CHECK(error.device_message == "partition denied");
    CHECK(error.informational.size() == 1U);
    CHECK(error.informational[0].payload == "checking partition");
    CHECK(error.transport_status == TransportStatus::Ok);
    CHECK(error.transport_certainty == TransferCertainty::FullyTransferred);
    CHECK(error.outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(error.task_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(error.completed_actions == 1U);
    CHECK(error.total_actions == 3U);
    CHECK(!error.session_poisoned);
    CHECK(!error.session_closed);
    CHECK(error.native_code == 0);
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void cancellation_and_absolute_deadline_are_fail_closed() {
    const auto payload = to_bytes("cancel-payload");
    auto bound = bind_artifact(
        "system.img", std::make_shared<MemorySource>(payload));
    {
        std::stop_source stop;
        auto transport =
            std::make_unique<StopAfterReadTransport>(stop, 1U);
        auto* scripted = &transport->script();
        scripted->expect_write("erase:userdata");
        scripted->respond("OKAYerased");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(
            UpdateDeviceTaskInput{
                .task = {
                    .kind = UpdateTaskKind::Erase,
                    .partition = "userdata",
                },
            },
            {});
        CHECK(token);
        auto result = (*token)->execute(
            UpdateOperationContext{.cancellation = stop.get_token()});
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::Cancelled);
        CHECK(result.error().session_poisoned);
        CHECK(result.error().task_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(result.error().completed_actions == 1U);
        CHECK(result.error().total_actions == 1U);
        CHECK(scripted->cancellation_requested());
        CHECK(scripted->complete());
        // The cancellation raced after the reply but is sticky in the session.
        // The quarantine bit prevents callers from treating Ready as reusable.
        CHECK(session.state() == SessionState::Ready);
        auto unavailable = service.getvar("product");
        CHECK(!unavailable);
        CHECK(unavailable.error().code == PrimitiveErrorCode::Cancelled);
        CHECK(session.state() == SessionState::Poisoned);
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        std::stop_source stopped;
        stopped.request_stop();
        auto result = device.prepare_task(
            flash_input(bound),
            UpdateOperationContext{.cancellation = stopped.get_token()});
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::Cancelled);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto result = device.prepare_task(
            flash_input(bound),
            UpdateOperationContext{
                .deadline = std::chrono::steady_clock::now(),
            });
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::TimedOut);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:max-download-size");
        script->respond("OKAY4096");
        const auto command = download_command(payload.size());
        script->expect_write(command);
        script->respond(std::string{"DATA"} + command.substr(9U));
        script->expect_source_write(
            payload,
            {{.size = 2U, .progress_watermark = 2U},
             {.offset = 2U,
              .size = payload.size() - 2U,
              .progress_watermark = payload.size()}});

        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        std::stop_source stop;
        PrimitiveUpdateDevice device(
            service,
            PrimitiveUpdateDeviceOptions{
                .progress = [&stop](const PrimitiveUpdateProgress& value) {
                    if (value.part_completed_bytes != 0U) {
                        stop.request_stop();
                    }
                    return PrimitiveUpdateProgressAction::Continue;
                },
            });
        auto token = device.prepare_task(flash_input(bound), {});
        CHECK(token);
        auto result = (*token)->execute(
            UpdateOperationContext{.cancellation = stop.get_token()});
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::Cancelled);
        CHECK(result.error().transport_status == TransportStatus::Cancelled);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().session_poisoned);
        CHECK(result.error().task_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().completed_actions == 0U);
        CHECK(result.error().total_actions == 1U);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:max-download-size");
        script->respond("OKAY4096");
        const auto command = download_command(payload.size());
        script->expect_write(command);
        script->respond(std::string{"DATA"} + command.substr(9U));
        script->expect_source_write(
            payload,
            {{.size = 2U, .progress_watermark = 2U},
             {.offset = 2U,
              .size = payload.size() - 2U,
              .progress_watermark = payload.size()}});

        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(
            service,
            PrimitiveUpdateDeviceOptions{
                .progress = [](const PrimitiveUpdateProgress& value) {
                    if (value.part_completed_bytes != 0U) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(750));
                    }
                    return PrimitiveUpdateProgressAction::Continue;
                },
            });
        auto token = device.prepare_task(flash_input(bound), {});
        CHECK(token);
        auto result = (*token)->execute(UpdateOperationContext{
            .deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(500),
        });
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::TimedOut);
        CHECK(result.error().transport_status == TransportStatus::Cancelled);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().session_poisoned);
        CHECK(result.error().task_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().completed_actions == 0U);
        CHECK(result.error().total_actions == 1U);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
}

void sparse_token_retains_identity_and_does_not_reparse() {
    const auto sparse_bytes = one_block_sparse_image();
    auto source = std::make_shared<MemorySource>(sparse_bytes);
    auto bound = bind_artifact("system.img", source);
    CHECK(bound.artifact->sparse_image() != nullptr);
    std::weak_ptr<const IImageSource> weak_source = source;
    std::weak_ptr<const ResolvedArtifact> weak_resolved = bound.resolved;
    std::weak_ptr<const FlashArtifact> weak_artifact = bound.artifact;

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:max-download-size");
    script->respond("OKAY0x100000");
    expect_flash(*script, "system", sparse_bytes);
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);
    auto token = device.prepare_task(flash_input(bound), {});
    CHECK(token);
    const auto reads_after_prepare = source->read_count();

    bound.resolved.reset();
    bound.artifact.reset();
    source.reset();
    CHECK(!weak_source.expired());
    CHECK(!weak_resolved.expired());
    CHECK(!weak_artifact.expired());

    CHECK((*token)->execute({}));
    auto retained_source = std::dynamic_pointer_cast<const MemorySource>(
        weak_source.lock());
    CHECK(retained_source);
    // ScriptedTransport requested exactly one full transfer read. Any sparse
    // reparse in execute would add metadata reads before that transfer.
    CHECK(retained_source->read_count() == reads_after_prepare + 1U);
    retained_source.reset();
    token->reset();
    CHECK(weak_source.expired());
    CHECK(weak_resolved.expired());
    CHECK(weak_artifact.expired());
    CHECK(script->complete());
}

void update_super_success_binds_wipe_and_immutable_sparse_source() {
    const auto payload = one_block_sparse_image();
    auto source = std::make_shared<MemorySource>(payload);
    auto bound = bind_artifact("super_empty.img", source);
    std::weak_ptr<const IImageSource> weak_source = source;
    std::weak_ptr<const ResolvedArtifact> weak_resolved = bound.resolved;
    std::weak_ptr<const FlashArtifact> weak_artifact = bound.artifact;

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    expect_update_super_preparation(*script, "OKAYsuper_main");
    expect_update_super_download(*script, payload);
    script->expect_write("update-super:super_main:wipe");
    script->respond("INFOrewriting metadata");
    script->respond("OKAYupdated");

    std::vector<PrimitiveUpdateProgress> progress;
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(
        service,
        PrimitiveUpdateDeviceOptions{
            .progress = [&progress](const PrimitiveUpdateProgress& value) {
                progress.push_back(value);
                return PrimitiveUpdateProgressAction::Continue;
            },
        });
    auto token = device.prepare_task(update_super_input(bound, true), {});
    CHECK(token);
    const auto reads_after_prepare = source->read_count();

    bound.resolved.reset();
    bound.artifact.reset();
    source.reset();
    CHECK(!weak_source.expired());
    CHECK(!weak_resolved.expired());
    CHECK(!weak_artifact.expired());

    CHECK((*token)->execute({}));
    auto retained_source = std::dynamic_pointer_cast<const MemorySource>(
        weak_source.lock());
    CHECK(retained_source);
    CHECK(retained_source->read_count() == reads_after_prepare + 1U);
    CHECK(!progress.empty());
    CHECK(progress.back().completed_bytes == payload.size());
    CHECK(progress.back().total_bytes == payload.size());
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);

    retained_source.reset();
    token->reset();
    CHECK(weak_source.expired());
    CHECK(weak_resolved.expired());
    CHECK(weak_artifact.expired());
}

void update_super_device_fail_falls_back_to_super_only() {
    const auto payload = one_block_sparse_image();
    auto bound = bind_artifact(
        "super_empty.img", std::make_shared<MemorySource>(payload));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    expect_update_super_preparation(*script, "FAILunknown variable");
    expect_update_super_download(*script, payload);
    script->expect_write("update-super:super");
    script->respond("OKAYupdated");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);
    auto token = device.prepare_task(update_super_input(bound), {});
    CHECK(token);
    CHECK((*token)->execute({}));
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void update_super_bootloader_mode_is_explicitly_unsupported() {
    auto bound = bind_artifact(
        "super_empty.img",
        std::make_shared<MemorySource>(one_block_sparse_image()));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:is-userspace");
    script->respond("OKAYno");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);
    auto token = device.prepare_task(update_super_input(bound), {});
    CHECK(!token);
    CHECK(token.error().kind == UpdateDeviceErrorKind::Unsupported);
    CHECK(token.error().message.find("reboot-fastboot") != std::string::npos);
    CHECK(token.error().message.find("physical-port reconnect") !=
          std::string::npos);
    CHECK(token.error().task_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(token.error().completed_actions == 0U);
    CHECK(token.error().total_actions == 0U);
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void fastbootd_reconnect_is_shared_by_every_later_prepared_token() {
    const auto super_payload = one_block_sparse_image();
    const auto flash_payload = to_bytes("post-reconnect-system-image");
    auto super = bind_artifact(
        "super_empty.img", std::make_shared<MemorySource>(super_payload));
    auto system = bind_artifact(
        "system.img", std::make_shared<MemorySource>(flash_payload));

    auto initial_transport = std::make_unique<ScriptedTransport>();
    auto* initial = initial_transport.get();
    // All tasks prepare before any task executes. Only the ordinary flash
    // needs the bootloader session's limit while binding its sparse plan.
    initial->expect_write("getvar:max-download-size");
    initial->respond("OKAY0x100000");
    initial->expect_write("getvar:is-userspace");
    initial->respond("OKAYno");
    initial->expect_write("getvar:product");
    initial->respond("OKAYproduct_a");
    initial->expect_write("getvar:serialno");
    initial->respond("OKAYSERIAL-A");
    initial->expect_write("reboot-fastboot");
    initial->respond("OKAYrebooting");

    auto replacement_transport = std::make_unique<ScriptedTransport>();
    auto* replacement = replacement_transport.get();
    // The explicit reboot token performs the transition. update-super proves
    // the actor now exposes the replacement service before any DATA.
    replacement->expect_write("getvar:is-userspace");
    replacement->respond("OKAYyes");
    replacement->expect_write("getvar:super-partition-name");
    replacement->respond("OKAYsuper_main");
    replacement->expect_write("getvar:max-download-size");
    replacement->respond("OKAY0x100000");
    const auto super_download = download_command(super_payload.size());
    replacement->expect_write(super_download);
    replacement->respond(std::string{"DATA"} + super_download.substr(9U));
    replacement->expect_source_write(
        super_payload,
        {{.size = super_payload.size(),
          .progress_watermark = super_payload.size()}});
    replacement->respond("OKAYdownloaded");
    replacement->expect_write("update-super:super_main:wipe");
    replacement->respond("OKAYupdated");
    // The flash token was prepared against generation zero. It must validate
    // the replacement session's limit and then send both operations there.
    replacement->expect_write("getvar:max-download-size");
    replacement->respond("OKAY0x100000");
    expect_flash(*replacement, "system", flash_payload);

    auto wanted = actor_target();
    ActorDiscovery discovery;
    discovery.steps = {
        // Same serial on the wrong port must not be followed while the expected
        // physical port is disconnected.
        std::vector<ReconnectCandidate>{
            actor_candidate(actor_port({2U, 4U}), wanted.serial),
        },
        std::vector<ReconnectCandidate>{
            actor_candidate(actor_port({2U, 4U}), wanted.serial),
            actor_candidate(wanted.physical_port, wanted.serial),
        },
    };
    ActorOpener opener;
    opener.identities.push_back(actor_identity());
    opener.transports.push_back(std::move(replacement_transport));
    ActorWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);
    auto fixture = make_reconnect_device(
        std::move(initial_transport), coordinator, wanted,
        actor_reconnect_options());
    auto& device = *fixture.device;

    PreparedUpdatePackage package{
        .plan = {
            .tasks = {
                PlannedUpdateTask{
                    .kind = UpdateTaskKind::Reboot,
                    .reboot_target = PlannedRebootTarget::Fastboot,
                },
                PlannedUpdateTask{
                    .kind = UpdateTaskKind::UpdateSuper,
                },
                PlannedUpdateTask{
                    .kind = UpdateTaskKind::Flash,
                    .partition = "system",
                    .artifact = "system.img",
                },
            },
        },
        .artifacts = {
            PreparedUpdateArtifact{
                .name = "system.img",
                .resolved = system.resolved,
                .artifact = system.artifact,
            },
        },
        .update_super_state = UpdateSuperPreparationState::Prepared,
        .prepared_super_artifact =
            std::make_shared<const PreparedSuperArtifact>(
                super.resolved, super.artifact, true),
        .requires_device_validation = false,
    };

    const auto deadline = waiter.current + 30s;
    auto executed = execute_prepared_update(
        package, device,
        UpdateExecutorOptions{
            .deadline = deadline,
        });
    CHECK(executed);
    CHECK(executed->completed_tasks == 3U);

    CHECK(initial->complete());
    CHECK(initial->closed());
    CHECK(fixture.initial_session->state() == SessionState::Closed);
    CHECK(discovery.calls == 2U);
    CHECK(opener.candidates.size() == 1U);
    CHECK(opener.candidates.front().physical_port == wanted.physical_port);
    CHECK(std::ranges::all_of(discovery.deadlines,
                              [deadline](const auto value) {
                                  return value == deadline;
                              }));
    CHECK(std::ranges::all_of(opener.deadlines,
                              [deadline](const auto value) {
                                  return value == deadline;
                              }));
    CHECK(replacement->complete());
}

void preparing_after_reconnect_uses_the_new_session_generation() {
    auto first = bind_artifact(
        "system.img",
        std::make_shared<MemorySource>(to_bytes("first-image")));
    auto second = bind_artifact(
        "vendor.img",
        std::make_shared<MemorySource>(to_bytes("second-image")));

    auto initial_transport = std::make_unique<ScriptedTransport>();
    auto* initial = initial_transport.get();
    initial->expect_write("getvar:max-download-size");
    initial->respond("OKAY4096");
    initial->expect_write("getvar:is-userspace");
    initial->respond("OKAYno");
    initial->expect_write("getvar:product");
    initial->respond("OKAYproduct_a");
    initial->expect_write("getvar:serialno");
    initial->respond("OKAYSERIAL-A");
    initial->expect_write("reboot-fastboot");
    initial->respond("OKAYrebooting");

    auto replacement_transport = std::make_unique<ScriptedTransport>();
    auto* replacement = replacement_transport.get();
    replacement->expect_write("getvar:max-download-size");
    replacement->respond("OKAY8192");

    const auto wanted = actor_target();
    ActorDiscovery discovery;
    discovery.steps = {
        std::vector<ReconnectCandidate>{
            actor_candidate(wanted.physical_port, wanted.serial),
        },
    };
    ActorOpener opener;
    opener.identities.push_back(actor_identity());
    opener.transports.push_back(std::move(replacement_transport));
    ActorWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);
    auto fixture = make_reconnect_device(
        std::move(initial_transport), coordinator, wanted,
        actor_reconnect_options());

    auto prepared_before = fixture.device->prepare_task(
        flash_input(first, "system", "system.img"), {});
    CHECK(prepared_before);
    auto reboot = fixture.device->prepare_task(
        UpdateDeviceTaskInput{
            .task = {
                .kind = UpdateTaskKind::Reboot,
                .reboot_target = PlannedRebootTarget::Fastboot,
            },
        },
        {});
    CHECK(reboot);
    CHECK((*reboot)->execute(UpdateOperationContext{
        .deadline = waiter.current + 30s,
    }));

    auto prepared_after = fixture.device->prepare_task(
        flash_input(second, "vendor", "vendor.img"), {});
    CHECK(prepared_after);
    CHECK(initial->complete());
    CHECK(initial->closed());
    CHECK(replacement->complete());
}

void update_super_can_perform_the_fastbootd_transition_itself() {
    const auto payload = one_block_sparse_image();
    auto super = bind_artifact(
        "super_empty.img", std::make_shared<MemorySource>(payload));

    auto initial_transport = std::make_unique<ScriptedTransport>();
    auto* initial = initial_transport.get();
    initial->expect_write("getvar:is-userspace");
    initial->respond("OKAYno");
    initial->expect_write("getvar:product");
    initial->respond("OKAYproduct_a");
    initial->expect_write("getvar:serialno");
    initial->respond("OKAYSERIAL-A");
    initial->expect_write("reboot-fastboot");
    initial->respond("OKAYrebooting");

    auto replacement_transport = std::make_unique<ScriptedTransport>();
    auto* replacement = replacement_transport.get();
    replacement->expect_write("getvar:super-partition-name");
    replacement->respond("OKAYsuper");
    replacement->expect_write("getvar:max-download-size");
    replacement->respond("OKAY0x100000");
    const auto command = download_command(payload.size());
    replacement->expect_write(command);
    replacement->respond(std::string{"DATA"} + command.substr(9U));
    replacement->expect_source_write(
        payload,
        {{.size = payload.size(), .progress_watermark = payload.size()}});
    replacement->respond("OKAYdownloaded");
    replacement->expect_write("update-super:super");
    replacement->respond("OKAYupdated");

    const auto wanted = actor_target();
    ActorDiscovery discovery;
    discovery.steps = {
        std::vector<ReconnectCandidate>{
            actor_candidate(wanted.physical_port, wanted.serial),
        },
    };
    ActorOpener opener;
    opener.identities.push_back(actor_identity());
    opener.transports.push_back(std::move(replacement_transport));
    ActorWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);
    auto fixture = make_reconnect_device(
        std::move(initial_transport), coordinator, wanted,
        actor_reconnect_options());
    auto& device = *fixture.device;
    auto token = device.prepare_task(update_super_input(super), {});
    CHECK(token);

    CHECK((*token)->execute(UpdateOperationContext{
        .deadline = waiter.current + 30s,
    }));
    CHECK(initial->complete());
    CHECK(initial->closed());
    CHECK(discovery.calls == 1U);
    CHECK(opener.candidates.size() == 1U);
    CHECK(replacement->complete());
}

void already_fastbootd_never_reboots_or_discovers() {
    const auto payload = one_block_sparse_image();
    auto super = bind_artifact(
        "super_empty.img", std::make_shared<MemorySource>(payload));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    expect_update_super_preparation(*script);
    expect_update_super_download(*script, payload);
    script->expect_write("update-super:super");
    script->respond("OKAYupdated");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);
    auto token = device.prepare_task(update_super_input(super), {});
    CHECK(token);
    CHECK((*token)->execute({}));
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void raw_reboot_fastboot_is_fastbootd_only() {
    const auto input = [] {
        return UpdateDeviceTaskInput{
            .task = {
                .kind = UpdateTaskKind::Reboot,
                .reboot_target = PlannedRebootTarget::Fastboot,
            },
        };
    };
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYyes");
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYyes");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(input(), {});
        CHECK(token);
        CHECK((*token)->execute({}));
        CHECK(!accepted_contains(*script, "reboot-fastboot"));
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Ready);
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYno");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(input(), {});
        CHECK(!token);
        CHECK(token.error().kind == UpdateDeviceErrorKind::Unsupported);
        CHECK(!accepted_contains(*script, "reboot-fastboot"));
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Ready);
    }
}

void partial_reboot_fastboot_never_starts_discovery() {
    auto super = bind_artifact(
        "super_empty.img",
        std::make_shared<MemorySource>(one_block_sparse_image()));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:is-userspace");
    script->respond("OKAYno");
    script->expect_write("getvar:product");
    script->respond("OKAYproduct_a");
    script->expect_write("getvar:serialno");
    script->respond("OKAYSERIAL-A");
    script->expect_write(
        "reboot-fastboot", 3U, TransportStatus::IoError,
        TransferCertainty::PartialOrUnknown, 71,
        "scripted partial reboot command");

    ActorDiscovery discovery;
    ActorOpener opener;
    ActorWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);
    auto fixture = make_reconnect_device(
        std::move(transport), coordinator, actor_target(),
        actor_reconnect_options());
    auto& device = *fixture.device;
    auto token = device.prepare_task(update_super_input(super), {});
    CHECK(token);
    auto result = (*token)->execute(UpdateOperationContext{
        .deadline = waiter.current + 30s,
    });
    CHECK(!result);
    CHECK(result.error().outbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().task_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().session_poisoned);
    CHECK(result.error().native_code == 71);
    CHECK(discovery.calls == 0U);
    CHECK(opener.candidates.empty());
    CHECK(script->complete());
    CHECK(fixture.initial_session->state() == SessionState::Poisoned);
}

void invalid_reconnect_identity_fails_before_protocol_bytes() {
    const auto rejected = [](ReconnectDeviceIdentity identity,
                             ReconnectTarget target) {
        auto binding = bind_initial_reconnect_session(
            OpenedReconnectSession{
                .verified_identity = std::move(identity),
                .session = std::make_unique<FastbootSession>(
                    std::make_unique<ScriptedTransport>()),
                .outbound_certainty = TransferCertainty::FullyTransferred,
            },
            std::move(target));
        CHECK(!binding);
        CHECK(binding.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
    };

    auto invalid_target = actor_target();
    invalid_target.physical_port.ports.clear();
    rejected(actor_identity(actor_port(), std::string{"SERIAL-A"},
                            "product_a", FastbootUsbMode::Bootloader),
             invalid_target);
    auto invalid_direction = actor_target();
    invalid_direction.previous_mode = FastbootUsbMode::Fastbootd;
    invalid_direction.required_mode = FastbootUsbMode::Bootloader;
    rejected(actor_identity(actor_port(), std::string{"SERIAL-A"},
                            "product_a", FastbootUsbMode::Fastbootd),
             invalid_direction);

    const auto target = actor_target();
    rejected(actor_identity(actor_port({9U}), target.serial, target.product,
                            FastbootUsbMode::Bootloader),
             target);
    rejected(actor_identity(target.physical_port, std::string{"SERIAL-B"},
                            target.product, FastbootUsbMode::Bootloader),
             target);
    rejected(actor_identity(target.physical_port, target.serial, "product_b",
                            FastbootUsbMode::Bootloader),
             target);
    rejected(actor_identity(target.physical_port, target.serial,
                            target.product, FastbootUsbMode::Fastbootd),
             target);
    auto wrong_fingerprint = actor_identity(
        target.physical_port, target.serial, target.product,
        FastbootUsbMode::Bootloader);
    ++wrong_fingerprint.usb_fingerprint.product_id;
    rejected(std::move(wrong_fingerprint), target);
}

void initial_protocol_identity_is_rechecked_before_reboot() {
    auto super = bind_artifact(
        "super_empty.img",
        std::make_shared<MemorySource>(one_block_sparse_image()));
    const auto run = [&super](const std::string_view product,
                             const std::optional<std::string_view> serial,
                             const std::string_view failed_variable) {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYno");
        script->expect_write("getvar:product");
        script->respond(std::string{"OKAY"} + std::string(product));
        if (serial) {
            script->expect_write("getvar:serialno");
            script->respond(std::string{"OKAY"} + std::string(*serial));
        }

        ActorDiscovery discovery;
        ActorOpener opener;
        ActorWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto fixture = make_reconnect_device(
            std::move(transport), coordinator, actor_target(),
            actor_reconnect_options());
        auto token = fixture.device->prepare_task(
            update_super_input(super), {});
        CHECK(token);
        auto result = (*token)->execute(UpdateOperationContext{
            .deadline = waiter.current + 30s,
        });
        CHECK(!result);
        CHECK(result.error().message.find(failed_variable) !=
              std::string::npos);
        CHECK(!result.error().session_closed);
        CHECK(!result.error().session_poisoned);
        CHECK(!accepted_contains(*script, "reboot-fastboot"));
        CHECK(discovery.calls == 0U);
        CHECK(opener.candidates.empty());
        CHECK(script->complete());
        CHECK(fixture.initial_session->state() == SessionState::Ready);
    };

    run("product_b", std::nullopt, "product");
    run("product_a", std::string_view{"SERIAL-B"}, "serialno");
}

void explicit_reboot_fastboot_fail_keeps_the_actor_ready() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:is-userspace");
    script->respond("OKAYno");
    script->expect_write("getvar:product");
    script->respond("OKAYproduct_a");
    script->expect_write("getvar:serialno");
    script->respond("OKAYSERIAL-A");
    script->expect_write("reboot-fastboot");
    script->respond("FAILtransition rejected");
    script->expect_write("getvar:product");
    script->respond("OKAYproduct_a");

    ActorDiscovery discovery;
    ActorOpener opener;
    ActorWaiter waiter;
    ReconnectCoordinator coordinator(discovery, opener, waiter);
    auto fixture = make_reconnect_device(
        std::move(transport), coordinator, actor_target(),
        actor_reconnect_options());
    auto token = fixture.device->prepare_task(
        UpdateDeviceTaskInput{
            .task = {
                .kind = UpdateTaskKind::Reboot,
                .reboot_target = PlannedRebootTarget::Fastboot,
            },
        },
        {});
    CHECK(token);
    auto rebooted = (*token)->execute(UpdateOperationContext{
        .deadline = waiter.current + 30s,
    });
    CHECK(!rebooted);
    CHECK(rebooted.error().device_message == "transition rejected");
    CHECK(!rebooted.error().session_closed);
    CHECK(!rebooted.error().session_poisoned);
    CHECK(fixture.initial_session->state() == SessionState::Ready);
    CHECK(discovery.calls == 0U);

    auto product = fixture.device->getvar("product", {});
    CHECK(product);
    CHECK(*product == "product_a");
    CHECK(script->complete());
}

void reconnect_timeout_and_cancel_share_the_operation_boundary() {
    auto super = bind_artifact(
        "super_empty.img",
        std::make_shared<MemorySource>(one_block_sparse_image()));
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYno");
        script->expect_write("getvar:product");
        script->respond("OKAYproduct_a");
        script->expect_write("getvar:serialno");
        script->respond("OKAYSERIAL-A");
        script->expect_write("reboot-fastboot");
        script->respond("OKAYrebooting");
        ActorDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{}};
        ActorOpener opener;
        ActorWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto fixture = make_reconnect_device(
            std::move(transport), coordinator, actor_target(),
            actor_reconnect_options());
        auto& device = *fixture.device;
        auto token = device.prepare_task(update_super_input(super), {});
        CHECK(token);
        const auto deadline = waiter.current + 3ms;
        auto result = (*token)->execute(UpdateOperationContext{
            .deadline = deadline,
        });
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::TimedOut);
        CHECK(result.error().session_closed);
        CHECK(result.error().completed_actions == 1U);
        CHECK(result.error().total_actions == 2U);
        CHECK(result.error().task_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(discovery.calls == 1U);
        CHECK(discovery.deadlines ==
              std::vector<ReconnectTimePoint>{deadline});
        CHECK(opener.candidates.empty());
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYno");
        script->expect_write("getvar:product");
        script->respond("OKAYproduct_a");
        script->expect_write("getvar:serialno");
        script->respond("OKAYSERIAL-A");
        script->expect_write("reboot-fastboot");
        script->respond("OKAYrebooting");
        ActorDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{}};
        ActorOpener opener;
        ActorWaiter waiter;
        std::stop_source stop;
        waiter.cancel_source = &stop;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto fixture = make_reconnect_device(
            std::move(transport), coordinator, actor_target(),
            actor_reconnect_options());
        auto& device = *fixture.device;
        auto token = device.prepare_task(update_super_input(super), {});
        CHECK(token);
        const auto deadline = waiter.current + 30s;
        auto result = (*token)->execute(
            UpdateOperationContext{
                .cancellation = stop.get_token(),
                .deadline = deadline,
            });
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::Cancelled);
        CHECK(result.error().session_closed);
        CHECK(result.error().completed_actions == 1U);
        CHECK(result.error().total_actions == 2U);
        CHECK(discovery.calls == 1U);
        CHECK(discovery.deadlines ==
              std::vector<ReconnectTimePoint>{deadline});
        CHECK(opener.candidates.empty());
        CHECK(script->complete());
    }
}

void reconnect_rejects_wrong_port_product_and_mode_before_data() {
    auto super = bind_artifact(
        "super_empty.img",
        std::make_shared<MemorySource>(one_block_sparse_image()));
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYno");
        script->expect_write("getvar:product");
        script->respond("OKAYproduct_a");
        script->expect_write("getvar:serialno");
        script->respond("OKAYSERIAL-A");
        script->expect_write("reboot-fastboot");
        script->respond("OKAYrebooting");
        const auto wanted = actor_target();
        ActorDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{
            actor_candidate(actor_port({9U}), wanted.serial),
        }};
        ActorOpener opener;
        ActorWaiter waiter;
        auto options = actor_reconnect_options();
        options.maximum_discovery_attempts = 1U;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto fixture = make_reconnect_device(
            std::move(transport), coordinator, wanted, options);
        auto& device = *fixture.device;
        auto token = device.prepare_task(update_super_input(super), {});
        CHECK(token);
        auto result = (*token)->execute(UpdateOperationContext{
            .deadline = waiter.current + 30s,
        });
        CHECK(!result);
        CHECK(result.error().message.find("attempt limit") !=
              std::string::npos);
        CHECK(opener.candidates.empty());
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYno");
        script->expect_write("getvar:product");
        script->respond("OKAYproduct_a");
        script->expect_write("getvar:serialno");
        script->respond("OKAYSERIAL-A");
        script->expect_write("reboot-fastboot");
        script->respond("OKAYrebooting");
        const auto wanted = actor_target();
        ActorDiscovery discovery;
        discovery.steps = {std::vector<ReconnectCandidate>{
            actor_candidate(wanted.physical_port, wanted.serial),
        }};
        ActorOpener opener;
        opener.identities.push_back(
            actor_identity(actor_port(), wanted.serial, "product_b"));
        opener.transports.push_back(std::make_unique<ScriptedTransport>());
        ActorWaiter waiter;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto fixture = make_reconnect_device(
            std::move(transport), coordinator, wanted,
            actor_reconnect_options());
        auto& device = *fixture.device;
        auto token = device.prepare_task(update_super_input(super), {});
        CHECK(token);
        auto result = (*token)->execute(UpdateOperationContext{
            .deadline = waiter.current + 30s,
        });
        CHECK(!result);
        CHECK(result.error().message.find("product") != std::string::npos);
        CHECK(opener.candidates.size() == 1U);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYno");
        script->expect_write("getvar:product");
        script->respond("OKAYproduct_a");
        script->expect_write("getvar:serialno");
        script->respond("OKAYSERIAL-A");
        script->expect_write("reboot-fastboot");
        script->respond("OKAYrebooting");
        const auto wanted = actor_target();
        const auto passive =
            actor_candidate(wanted.physical_port, wanted.serial);
        ActorDiscovery discovery;
        discovery.steps = {
            std::vector<ReconnectCandidate>{passive},
            std::vector<ReconnectCandidate>{passive},
        };
        ActorOpener opener;
        opener.identities.push_back(actor_identity(
            actor_port(), wanted.serial, wanted.product,
            FastbootUsbMode::Bootloader));
        opener.transports.push_back(std::make_unique<ScriptedTransport>());
        ActorWaiter waiter;
        auto options = actor_reconnect_options();
        options.maximum_open_attempts = 1U;
        ReconnectCoordinator coordinator(discovery, opener, waiter);
        auto fixture = make_reconnect_device(
            std::move(transport), coordinator, wanted, options);
        auto& device = *fixture.device;
        auto token = device.prepare_task(update_super_input(super), {});
        CHECK(token);
        auto result = (*token)->execute(UpdateOperationContext{
            .deadline = waiter.current + 30s,
        });
        CHECK(!result);
        CHECK(result.error().message.find("attempt limit") !=
              std::string::npos);
        CHECK(opener.candidates.size() == 1U);
        CHECK(script->complete());
    }
}

void update_super_device_fail_quarantines_after_data() {
    const auto payload = one_block_sparse_image();
    auto bound = bind_artifact(
        "super_empty.img", std::make_shared<MemorySource>(payload));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    expect_update_super_preparation(*script);
    expect_update_super_download(*script, payload);
    script->expect_write("update-super:super");
    script->respond("INFOchecking metadata");
    script->respond("FAILmetadata rejected");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);
    auto token = device.prepare_task(update_super_input(bound), {});
    CHECK(token);
    auto result = (*token)->execute({});
    CHECK(!result);
    const auto& error = result.error();
    CHECK(error.kind == UpdateDeviceErrorKind::Failed);
    CHECK(error.device_message == "metadata rejected");
    CHECK(error.informational.size() == 1U);
    CHECK(error.informational.front().payload == "checking metadata");
    CHECK(error.outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(error.task_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(error.completed_actions == 0U);
    CHECK(error.total_actions == 1U);
    CHECK(error.session_poisoned);
    CHECK(script->cancellation_requested());
    CHECK(script->complete());

    auto unavailable = service.getvar("product");
    CHECK(!unavailable);
    CHECK(unavailable.error().code == PrimitiveErrorCode::Cancelled);
    CHECK(session.state() == SessionState::Poisoned);
}

void update_super_timeout_and_partial_transfer_are_quarantined() {
    const auto payload = one_block_sparse_image();
    auto bound = bind_artifact(
        "super_empty.img", std::make_shared<MemorySource>(payload));
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        expect_update_super_preparation(*script);
        expect_update_super_download(*script, payload);
        script->expect_write("update-super:super");
        script->respond(
            "", TransportStatus::Timeout,
            TransferCertainty::NotTransferred, false, 0U, 91,
            "scripted response timeout");

        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(update_super_input(bound), {});
        CHECK(token);
        auto result = (*token)->execute({});
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::TimedOut);
        CHECK(result.error().transport_status == TransportStatus::Timeout);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(result.error().task_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().session_poisoned);
        CHECK(result.error().native_code == 91);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        expect_update_super_preparation(*script);
        script->expect_write("getvar:is-userspace");
        script->respond("OKAYyes");
        const auto command = download_command(payload.size());
        script->expect_write(command);
        script->respond(std::string{"DATA"} + command.substr(9U));
        script->expect_source_write(
            payload,
            {{.size = payload.size(),
              .progress_watermark = payload.size()}},
            7U, TransportStatus::IoError,
            TransferCertainty::PartialOrUnknown, 73,
            "scripted partial DATA");

        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(update_super_input(bound), {});
        CHECK(token);
        auto result = (*token)->execute({});
        CHECK(!result);
        CHECK(result.error().kind == UpdateDeviceErrorKind::Failed);
        CHECK(result.error().phase == ProtocolPhase::DataWrite);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().task_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().session_poisoned);
        CHECK(result.error().native_code == 73);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
}

void update_super_progress_cancel_is_quarantined() {
    const auto payload = one_block_sparse_image();
    auto bound = bind_artifact(
        "super_empty.img", std::make_shared<MemorySource>(payload));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    expect_update_super_preparation(*script);
    script->expect_write("getvar:is-userspace");
    script->respond("OKAYyes");
    const auto command = download_command(payload.size());
    script->expect_write(command);
    script->respond(std::string{"DATA"} + command.substr(9U));
    script->expect_source_write(
        payload,
        {{.size = 2U, .progress_watermark = 2U},
         {.offset = 2U,
          .size = payload.size() - 2U,
          .progress_watermark = payload.size()}});

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(
        service,
        PrimitiveUpdateDeviceOptions{
            .progress = [](const PrimitiveUpdateProgress& value) {
                return value.part_completed_bytes == 0U
                    ? PrimitiveUpdateProgressAction::Continue
                    : PrimitiveUpdateProgressAction::Cancel;
            },
        });
    auto token = device.prepare_task(update_super_input(bound), {});
    CHECK(token);
    auto result = (*token)->execute({});
    CHECK(!result);
    CHECK(result.error().kind == UpdateDeviceErrorKind::Cancelled);
    CHECK(result.error().outbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().task_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().session_poisoned);
    CHECK(session.state() == SessionState::Poisoned);
    CHECK(script->complete());
}

void update_super_artifact_identity_fails_before_device_queries() {
    auto first = bind_artifact(
        "super_empty.img",
        std::make_shared<MemorySource>(one_block_sparse_image()));
    auto second = bind_artifact(
        "super_empty.img",
        std::make_shared<MemorySource>(one_block_sparse_image()));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(service);

    auto input = update_super_input(first);
    input.super_artifact = std::make_shared<const PreparedSuperArtifact>(
        first.resolved, second.artifact, false);
    auto token = device.prepare_task(std::move(input), {});
    CHECK(!token);
    CHECK(token.error().kind == UpdateDeviceErrorKind::Failed);
    CHECK(token.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(token.error().message.find("inconsistent") != std::string::npos);
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void progress_observer_exception_is_contained() {
    const auto payload = to_bytes("observer-payload");
    auto bound = bind_artifact(
        "system.img", std::make_shared<MemorySource>(payload));
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:max-download-size");
    script->respond("OKAY4096");
    const auto command = download_command(payload.size());
    script->expect_write(command);
    script->respond(std::string{"DATA"} + command.substr(9U));
    script->expect_source_write(
        payload,
        {{.size = 3U, .progress_watermark = 3U},
         {.offset = 3U,
          .size = payload.size() - 3U,
          .progress_watermark = payload.size()}});

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    PrimitiveUpdateDevice device(
        service,
        PrimitiveUpdateDeviceOptions{
            .progress = [](const PrimitiveUpdateProgress& value) {
                if (value.part_completed_bytes != 0U) {
                    throw std::runtime_error("observer failure");
                }
                return PrimitiveUpdateProgressAction::Continue;
            },
        });
    auto token = device.prepare_task(flash_input(bound), {});
    CHECK(token);
    auto result = (*token)->execute({});
    CHECK(!result);
    CHECK(result.error().kind == UpdateDeviceErrorKind::Failed);
    CHECK(result.error().message ==
          "flash progress observer threw during image transfer");
    CHECK(result.error().transport_status == TransportStatus::Cancelled);
    CHECK(result.error().outbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().session_poisoned);
    CHECK(session.state() == SessionState::Poisoned);
    CHECK(script->complete());
}

void invalid_commands_artifacts_and_wire_limits_fail_in_prepare() {
    auto regular = bind_artifact(
        "system.img", std::make_shared<MemorySource>(to_bytes("image")));
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);

        auto invalid_partition = flash_input(regular);
        invalid_partition.task.partition = std::string{"bad\nname", 8U};
        CHECK(!device.prepare_task(std::move(invalid_partition), {}));

        auto invalid_slot = flash_input(regular);
        invalid_slot.task.slot = static_cast<PlannedSlot>(0xffU);
        CHECK(!device.prepare_task(std::move(invalid_slot), {}));

        auto mismatched = flash_input(regular);
        mismatched.flash_artifact->resolved =
            bind_artifact("other.img",
                          std::make_shared<MemorySource>(to_bytes("other")))
                .resolved;
        CHECK(!device.prepare_task(std::move(mismatched), {}));

        auto vbmeta = flash_input(regular);
        vbmeta.task.apply_vbmeta = true;
        CHECK(!device.prepare_task(std::move(vbmeta), {}));

        auto malformed_flash = flash_input(regular);
        malformed_flash.task.reboot_target = PlannedRebootTarget::Recovery;
        CHECK(!device.prepare_task(std::move(malformed_flash), {}));

        auto malformed_erase = UpdateDeviceTaskInput{
            .task = {
                .kind = UpdateTaskKind::Erase,
                .partition = "userdata",
                .slot = PlannedSlot::Other,
            },
        };
        CHECK(!device.prepare_task(std::move(malformed_erase), {}));

        auto malformed_reboot = UpdateDeviceTaskInput{
            .task = {
                .kind = UpdateTaskKind::Reboot,
                .apply_vbmeta = true,
            },
        };
        CHECK(!device.prepare_task(std::move(malformed_reboot), {}));

        auto update_super = device.prepare_task(
            UpdateDeviceTaskInput{
                .task = {.kind = UpdateTaskKind::UpdateSuper},
            },
            {});
        CHECK(!update_super);
        CHECK(update_super.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(update_super.error().task_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(update_super.error().completed_actions == 0U);
        CHECK(update_super.error().total_actions == 0U);
        CHECK(script->complete());
    }
    {
        auto empty = bind_artifact(
            "empty.img", std::make_shared<MemorySource>(std::vector<std::byte>{}));
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        CHECK(!device.prepare_task(
            flash_input(empty, "empty", "empty.img"), {}));
        CHECK(script->complete());
    }
    {
        const auto wire_max =
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
        auto exact = bind_artifact(
            "huge.img", std::make_shared<DeclaredSizeSource>(wire_max));
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:max-download-size");
        script->respond("OKAY0x100000000");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(
            flash_input(exact, "huge", "huge.img"), {});
        CHECK(token);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:max-download-size");
        script->respond("OKAY0");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        PrimitiveUpdateDevice device(service);
        auto token = device.prepare_task(flash_input(regular), {});
        CHECK(!token);
        CHECK(token.error().phase == ProtocolPhase::FinalResponse);
        CHECK(token.error().outbound_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(script->complete());
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"ordinary tasks", ordinary_flash_erase_and_reboot_are_complete},
        {"slot other prepare", slot_other_is_fully_resolved_during_prepare},
        {"slot topology compatibility",
         slot_other_legacy_fallback_and_ambiguous_topology_fail_closed},
        {"two phase fail closed",
         later_prepare_failure_sends_zero_destructive_commands},
        {"reconnect prepare fail closed",
         invalid_reconnect_options_block_every_task_before_flash},
        {"error bridge", primitive_error_mapping_preserves_every_diagnostic},
        {"actual device fail",
         actual_device_fail_preserves_info_and_device_message},
        {"single primitive task certainty",
         single_primitive_task_certainty_tracks_exact_outbound},
        {"multipart task certainty",
         multipart_flash_failure_preserves_primitive_and_task_evidence},
        {"cancellation and deadline",
         cancellation_and_absolute_deadline_are_fail_closed},
        {"sparse token identity",
         sparse_token_retains_identity_and_does_not_reparse},
        {"update-super success",
         update_super_success_binds_wipe_and_immutable_sparse_source},
        {"update-super fallback",
         update_super_device_fail_falls_back_to_super_only},
        {"update-super fastbootd boundary",
         update_super_bootloader_mode_is_explicitly_unsupported},
        {"fastbootd replacement session",
         fastbootd_reconnect_is_shared_by_every_later_prepared_token},
        {"reconnect generation cache",
         preparing_after_reconnect_uses_the_new_session_generation},
        {"update-super enters fastbootd",
         update_super_can_perform_the_fastbootd_transition_itself},
        {"already fastbootd", already_fastbootd_never_reboots_or_discovers},
        {"raw reboot fastboot boundary",
         raw_reboot_fastboot_is_fastbootd_only},
        {"partial reboot blocks reconnect",
         partial_reboot_fastboot_never_starts_discovery},
        {"invalid reconnect identity",
         invalid_reconnect_identity_fails_before_protocol_bytes},
        {"initial identity recheck",
         initial_protocol_identity_is_rechecked_before_reboot},
        {"reboot device fail remains ready",
         explicit_reboot_fastboot_fail_keeps_the_actor_ready},
        {"reconnect timeout cancellation",
         reconnect_timeout_and_cancel_share_the_operation_boundary},
        {"reconnect identity rejection",
         reconnect_rejects_wrong_port_product_and_mode_before_data},
        {"update-super device fail quarantine",
         update_super_device_fail_quarantines_after_data},
        {"update-super timeout partial quarantine",
         update_super_timeout_and_partial_transfer_are_quarantined},
        {"update-super cancel quarantine",
         update_super_progress_cancel_is_quarantined},
        {"update-super artifact identity",
         update_super_artifact_identity_fails_before_device_queries},
        {"observer exception", progress_observer_exception_is_contained},
        {"prepare validation",
         invalid_commands_artifacts_and_wire_limits_fail_in_prepare},
    };

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
