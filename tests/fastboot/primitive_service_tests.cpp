// SPDX-License-Identifier: MIT
#include "src/fastboot/primitive_service.hpp"
#include "src/fastboot/slot_planner.hpp"
#include "src/image/flash_artifact.hpp"
#include "src/image/sparse_flash_plan.hpp"
#include "src/transport/image_transfer_source.hpp"
#include "tests/protocol/scripted_transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::PrimitiveErrorCode;
using kairosboot::fastboot::PrimitiveOperation;
using kairosboot::fastboot::PrimitiveService;
using kairosboot::fastboot::RebootTarget;
using kairosboot::fastboot::SlotErrorCode;
using kairosboot::fastboot::SlotPlanner;
using kairosboot::fastboot::SlotSelectionKind;
using kairosboot::fastboot::SlotTopologySource;
using kairosboot::fastboot::parse_slot_selection;
using kairosboot::fastboot::validate_download_size;
using kairosboot::image::FlashArtifact;
using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::SparseFlashPlan;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::ITransferSource;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::ProtocolPhase;
using kairosboot::protocol::ResponseKind;
using kairosboot::protocol::SessionOptions;
using kairosboot::protocol::SessionState;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferProgressAction;
using kairosboot::protocol::TransferResult;
using kairosboot::protocol::TransportStatus;
using kairosboot::protocol::test::ScriptedTransport;
using kairosboot::protocol::test::to_bytes;
using kairosboot::transport::ImageTransferSource;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            throw CheckFailure(std::string("check failed: ") + #condition + " at line " + \
                               std::to_string(__LINE__));                                 \
        }                                                                                 \
    } while (false)

class RecordingSource final : public ITransferSource {
public:
    struct Read final {
        std::uint64_t offset{0};
        std::size_t size{0};

        friend bool operator==(const Read&, const Read&) = default;
    };

    RecordingSource(
        std::vector<std::byte> bytes,
        const std::size_t maximum_read,
        const std::optional<std::size_t> failing_call = std::nullopt)
        : bytes_(std::move(bytes)),
          maximum_read_(maximum_read),
          failing_call_(failing_call) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] bool read_exact(
        const std::uint64_t offset,
        const std::span<std::byte> destination) noexcept override {
        if (read_count_ >= reads_.size()) {
            return false;
        }
        reads_[read_count_] = Read{offset, destination.size()};
        const auto call = read_count_++;
        if (failing_call_ == call || destination.size() > maximum_read_ ||
            offset > bytes_.size()) {
            return false;
        }
        const auto start = static_cast<std::size_t>(offset);
        if (destination.size() > bytes_.size() - start) {
            return false;
        }
        std::ranges::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(start),
            destination.size(),
            destination.begin());
        return true;
    }

    [[nodiscard]] std::span<const Read> reads() const noexcept {
        return std::span(reads_).first(read_count_);
    }

private:
    std::vector<std::byte> bytes_;
    std::size_t maximum_read_{0};
    std::optional<std::size_t> failing_call_;
    std::array<Read, 16> reads_{};
    std::size_t read_count_{0};
};

class SizedSource final : public ITransferSource {
public:
    explicit SizedSource(const std::uint64_t size) : size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] bool read_exact(
        std::uint64_t /*offset*/,
        std::span<std::byte> /*destination*/) noexcept override {
        return false;
    }

private:
    std::uint64_t size_{0};
};

class MemoryImageSource final : public IImageSource {
public:
    explicit MemoryImageSource(std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset >= bytes_.size() || destination.empty()) {
            return 0;
        }
        const auto available = bytes_.size() - static_cast<std::size_t>(offset);
        const auto amount = std::min(available, destination.size());
        std::ranges::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            amount,
            destination.begin());
        return amount;
    }

private:
    std::vector<std::byte> bytes_;
};

[[nodiscard]] std::vector<std::byte> read_image(
    const std::shared_ptr<const IImageSource>& source) {
    CHECK(source->size() <= std::numeric_limits<std::size_t>::max());
    std::vector<std::byte> result(static_cast<std::size_t>(source->size()));
    std::size_t completed = 0;
    while (completed < result.size()) {
        auto read = source->read_at(
            completed, std::span(result).subspan(completed));
        CHECK(read.has_value());
        CHECK(*read != 0);
        completed += *read;
    }
    return result;
}

class NonStreamingTransport final : public ITransportSession {
public:
    [[nodiscard]] TransferResult write(
        std::span<const std::byte> /*bytes*/,
        std::chrono::milliseconds /*timeout*/) override {
        ++wire_calls;
        return {
            .status = TransportStatus::IoError,
            .certainty = TransferCertainty::NotTransferred,
            .detail = "unexpected non-streaming write",
        };
    }

    [[nodiscard]] TransferResult read(
        std::span<std::byte> /*destination*/,
        std::chrono::milliseconds /*timeout*/) override {
        ++wire_calls;
        return {
            .status = TransportStatus::IoError,
            .certainty = TransferCertainty::NotTransferred,
            .detail = "unexpected non-streaming read",
        };
    }

    void request_cancel() noexcept override {}
    void close() noexcept override { closed = true; }

    std::size_t wire_calls{0};
    bool closed{false};
};

[[nodiscard]] std::string accepted_text(const ScriptedTransport& script) {
    std::string result;
    result.reserve(script.accepted_bytes().size());
    for (const auto byte : script.accepted_bytes()) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return result;
}

void exact_non_data_commands_are_emitted() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:partition-type:system");
    script->respond("OKAYext4");
    script->expect_write("erase:userdata_a");
    script->respond("INFOerasing");
    script->respond("OKAYdone");
    script->expect_write("oem unlock-go  token");
    script->respond("TEXTconfirm");
    script->respond("OKAYaccepted");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);

    const auto variable = service.getvar("partition-type:system");
    CHECK(variable.has_value());
    CHECK(variable->terminal.payload == "ext4");

    const auto erased = service.erase("userdata_a");
    CHECK(erased.has_value());
    CHECK(erased->informational.size() == 1);

    const auto oem = service.oem("unlock-go  token");
    CHECK(oem.has_value());
    CHECK(oem->informational.size() == 1);
    CHECK(accepted_text(*script) ==
          "getvar:partition-type:systemerase:userdata_aoem unlock-go  token");
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void set_active_and_raw_commands_preserve_response_order() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("set_active:b");
    script->respond("INFOselecting slot");
    script->respond("TEXTupdating metadata");
    script->respond("OKAYb");
    script->expect_write("flashing get_unlock_ability");
    script->respond("INFOchecking policy");
    script->respond("TEXTdevice response follows");
    script->respond("OKAY1");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);

    const auto activated = service.set_active("b");
    CHECK(activated.has_value());
    CHECK(activated->terminal.kind == ResponseKind::Okay);
    CHECK(activated->terminal.payload == "b");
    CHECK(activated->informational.size() == 2);
    CHECK(activated->informational[0].kind == ResponseKind::Info);
    CHECK(activated->informational[0].payload == "selecting slot");
    CHECK(activated->informational[1].kind == ResponseKind::Text);
    CHECK(activated->informational[1].payload == "updating metadata");
    CHECK(session.state() == SessionState::Ready);

    const auto raw = service.raw_command("flashing get_unlock_ability");
    CHECK(raw.has_value());
    CHECK(raw->terminal.kind == ResponseKind::Okay);
    CHECK(raw->terminal.payload == "1");
    CHECK(raw->informational.size() == 2);
    CHECK(raw->informational[0].kind == ResponseKind::Info);
    CHECK(raw->informational[0].payload == "checking policy");
    CHECK(raw->informational[1].kind == ResponseKind::Text);
    CHECK(raw->informational[1].payload == "device response follows");
    CHECK(raw->outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(session.state() == SessionState::Ready);
    CHECK(accepted_text(*script) ==
          "set_active:bflashing get_unlock_ability");
    CHECK(script->complete());
}

void raw_fail_preserves_response_order_and_session() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("oem vendor-check");
    script->respond("INFOchecking");
    script->respond("TEXTpolicy denied");
    script->respond("FAILlocked");
    script->expect_write("getvar:product");
    script->respond("OKAYkairos");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto raw = service.raw_command("oem vendor-check");
    CHECK(!raw);
    CHECK(raw.error().code == PrimitiveErrorCode::DeviceFail);
    CHECK(raw.error().operation == PrimitiveOperation::RawCommand);
    CHECK(raw.error().device_message == "locked");
    CHECK(raw.error().informational.size() == 2);
    CHECK(raw.error().informational[0].kind == ResponseKind::Info);
    CHECK(raw.error().informational[0].payload == "checking");
    CHECK(raw.error().informational[1].kind == ResponseKind::Text);
    CHECK(raw.error().informational[1].payload == "policy denied");
    CHECK(raw.error().outbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(!raw.error().session_poisoned);
    CHECK(session.state() == SessionState::Ready);
    CHECK(service.getvar("product").has_value());
    CHECK(script->complete());
}

void raw_data_response_is_unsupported_and_poisons_session() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("download:00000001");
    script->respond("INFOpreparing");
    script->respond("TEXTready for data");
    script->respond("DATA00000001");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto raw = service.raw_command("download:00000001");
    CHECK(!raw);
    CHECK(raw.error().code == PrimitiveErrorCode::Unsupported);
    CHECK(raw.error().operation == PrimitiveOperation::RawCommand);
    CHECK(raw.error().phase == ProtocolPhase::FinalResponse);
    CHECK(raw.error().message.find("DATA") != std::string::npos);
    CHECK(raw.error().informational.size() == 2);
    CHECK(raw.error().informational[0].kind == ResponseKind::Info);
    CHECK(raw.error().informational[0].payload == "preparing");
    CHECK(raw.error().informational[1].kind == ResponseKind::Text);
    CHECK(raw.error().informational[1].payload == "ready for data");
    CHECK(raw.error().outbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(raw.error().session_poisoned);
    CHECK(session.state() == SessionState::Poisoned);

    const auto retry = service.raw_command("getvar:product");
    CHECK(!retry);
    CHECK(retry.error().code == PrimitiveErrorCode::Poisoned);
    CHECK(script->complete());
}

void protocol_errors_preserve_bounded_informational_history() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:product");
        script->respond("INFOwaiting");
        script->respond(
            "", TransportStatus::Timeout, TransferCertainty::NotTransferred);
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.getvar("product");
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::Timeout);
        CHECK(result.error().informational.size() == 1);
        CHECK(result.error().informational[0].kind == ResponseKind::Info);
        CHECK(result.error().informational[0].payload == "waiting");
        CHECK(result.error().session_poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:product");
        script->respond("INFOparsing");
        script->respond("NOPEbad");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.getvar("product");
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::ProtocolViolation);
        CHECK(result.error().informational.size() == 1);
        CHECK(result.error().informational[0].kind == ResponseKind::Info);
        CHECK(result.error().informational[0].payload == "parsing");
        CHECK(result.error().session_poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:product");
        script->respond("INFOone");
        script->respond("TEXTtwo");
        SessionOptions options;
        options.max_informational_responses = 1;
        FastbootSession session(std::move(transport), options);
        PrimitiveService service(session);
        const auto result = service.getvar("product");
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::ProtocolViolation);
        CHECK(result.error().informational.size() == 1);
        CHECK(result.error().informational[0].kind == ResponseKind::Info);
        CHECK(result.error().informational[0].payload == "one");
        CHECK(result.error().session_poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:product");
        script->respond("NOPEbad");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.getvar("product");
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::ProtocolViolation);
        CHECK(result.error().informational.empty());
        CHECK(result.error().session_poisoned);
        CHECK(script->complete());
    }
}

void reboot_variants_and_continue_are_terminal() {
    constexpr std::array cases{
        std::pair{RebootTarget::System, std::string_view{"reboot"}},
        std::pair{RebootTarget::Bootloader, std::string_view{"reboot-bootloader"}},
        std::pair{RebootTarget::Recovery, std::string_view{"reboot-recovery"}},
        std::pair{RebootTarget::Fastboot, std::string_view{"reboot-fastboot"}},
    };
    for (const auto& [target, command] : cases) {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write(command);
        script->respond("OKAY");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.reboot(target);
        CHECK(result.has_value());
        CHECK(session.state() == SessionState::Closed);
        CHECK(script->closed());
        CHECK(script->complete());
    }

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("continue");
    script->respond("OKAY");
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.continue_boot();
    CHECK(result.has_value());
    CHECK(session.state() == SessionState::Closed);
    CHECK(script->closed());
    CHECK(script->complete());

    auto failed_transport = std::make_unique<ScriptedTransport>();
    auto* failed_script = failed_transport.get();
    failed_script->expect_write("reboot-fastboot");
    failed_script->respond("FAILnot supported");
    failed_script->expect_write("getvar:product");
    failed_script->respond("OKAYkairos");
    FastbootSession failed_session(std::move(failed_transport));
    PrimitiveService failed_service(failed_session);
    const auto failed = failed_service.reboot(RebootTarget::Fastboot);
    CHECK(!failed);
    CHECK(failed.error().operation == PrimitiveOperation::Reboot);
    CHECK(failed.error().code == PrimitiveErrorCode::DeviceFail);
    CHECK(!failed.error().session_poisoned);
    CHECK(failed_session.state() == SessionState::Ready);
    CHECK(failed_service.getvar("product").has_value());
    CHECK(failed_script->complete());
}

void boot_downloaded_emits_exact_command_and_retires() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("boot");
    script->respond("INFOvalidating boot image");
    script->respond("TEXTstarting kernel");
    script->respond("OKAYbooting");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.boot_downloaded();
    CHECK(result.has_value());
    CHECK(result->terminal.kind == ResponseKind::Okay);
    CHECK(result->terminal.payload == "booting");
    CHECK(result->informational.size() == 2);
    CHECK(result->informational[0].kind == ResponseKind::Info);
    CHECK(result->informational[0].payload == "validating boot image");
    CHECK(result->informational[1].kind == ResponseKind::Text);
    CHECK(result->informational[1].payload == "starting kernel");
    CHECK(result->outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(accepted_text(*script) == "boot");
    CHECK(session.state() == SessionState::Closed);
    CHECK(script->closed());
    CHECK(script->complete());
}

void boot_fail_preserves_history_and_session() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("boot");
    script->respond("INFOvalidating boot image");
    script->respond("TEXTverification failed");
    script->respond("FAILboot image is not signed");
    script->expect_write("getvar:product");
    script->respond("OKAYkairos");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.boot_downloaded();
    CHECK(!result);
    CHECK(result.error().code == PrimitiveErrorCode::DeviceFail);
    CHECK(result.error().operation == PrimitiveOperation::Boot);
    CHECK(result.error().phase == ProtocolPhase::FinalResponse);
    CHECK(result.error().device_message == "boot image is not signed");
    CHECK(result.error().informational.size() == 2);
    CHECK(result.error().informational[0].kind == ResponseKind::Info);
    CHECK(result.error().informational[0].payload == "validating boot image");
    CHECK(result.error().informational[1].kind == ResponseKind::Text);
    CHECK(result.error().informational[1].payload == "verification failed");
    CHECK(result.error().outbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(!result.error().session_poisoned);
    CHECK(session.state() == SessionState::Ready);
    CHECK(!script->closed());

    CHECK(service.getvar("product").has_value());
    CHECK(script->complete());
}

void canonical_download_and_boot_trace() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("INFOpreparing");
    script->respond("DATA00000006");
    script->expect_write(payload);
    script->respond("TEXTdownloaded");
    script->respond("OKAYstaged");
    script->expect_write("boot");
    script->respond("INFOstarting");
    script->respond("OKAYbooting");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_boot(payload);
    CHECK(result.has_value());
    CHECK(result->download.terminal.payload == "staged");
    CHECK(result->download.informational.size() == 2);
    CHECK(result->download.outbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(result->boot.terminal.payload == "booting");
    CHECK(result->boot.informational.size() == 1);
    CHECK(result->boot.outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(accepted_text(*script) == "download:00000006abcdefboot");
    CHECK(session.state() == SessionState::Closed);
    CHECK(script->closed());
    CHECK(script->complete());
}

void boot_download_failures_do_not_issue_boot() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        const auto payload = to_bytes("abcdef");
        script->expect_write("download:00000006");
        script->respond("INFOchecking");
        script->respond("FAILtoo large");
        script->expect_write("getvar:product");
        script->respond("OKAYkairos");

        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.download_and_boot(payload);
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::DeviceFail);
        CHECK(result.error().operation == PrimitiveOperation::Download);
        CHECK(result.error().phase == ProtocolPhase::InitialResponse);
        CHECK(result.error().device_message == "too large");
        CHECK(result.error().informational.size() == 1);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(!result.error().session_poisoned);
        CHECK(accepted_text(*script) == "download:00000006");
        CHECK(session.state() == SessionState::Ready);
        CHECK(!script->closed());

        CHECK(service.getvar("product").has_value());
        CHECK(script->complete());
    }

    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        const auto payload = to_bytes("abcdef");
        script->expect_write("download:00000006");
        script->respond("DATA00000006");
        script->expect_write(
            payload,
            2,
            TransportStatus::Cancelled,
            TransferCertainty::PartialOrUnknown,
            125,
            "cancelled while downloading boot image");

        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.download_and_boot(payload);
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::Cancelled);
        CHECK(result.error().operation == PrimitiveOperation::Download);
        CHECK(result.error().phase == ProtocolPhase::DataWrite);
        CHECK(result.error().transport_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().native_code == 125);
        CHECK(result.error().session_poisoned);
        CHECK(accepted_text(*script) == "download:00000006ab");
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(!script->closed());
        CHECK(script->complete());
    }
}

void boot_command_partial_failure_is_poisoned() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("DATA00000006");
    script->expect_write(payload);
    script->respond("OKAYstaged");
    script->expect_write(
        "boot",
        2,
        TransportStatus::Cancelled,
        TransferCertainty::PartialOrUnknown,
        125,
        "cancelled while sending boot command");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_boot(payload);
    CHECK(!result);
    CHECK(result.error().code == PrimitiveErrorCode::Cancelled);
    CHECK(result.error().operation == PrimitiveOperation::Boot);
    CHECK(result.error().phase == ProtocolPhase::CommandWrite);
    CHECK(result.error().transport_status == TransportStatus::Cancelled);
    CHECK(result.error().transport_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().outbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(result.error().native_code == 125);
    CHECK(result.error().session_poisoned);
    CHECK(accepted_text(*script) == "download:00000006abcdefbo");
    CHECK(session.state() == SessionState::Poisoned);
    CHECK(!script->closed());
    CHECK(script->complete());
}

void canonical_download_and_flash_trace() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("INFOpreparing");
    script->respond("DATA00000006");
    script->expect_write(payload);
    script->respond("TEXTdownloaded");
    script->respond("OKAYstaged");
    script->expect_write("flash:boot");
    script->respond("INFOwriting");
    script->respond("OKAYflashed");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_flash("boot", payload);
    CHECK(result.has_value());
    CHECK(result->download.terminal.payload == "staged");
    CHECK(result->download.informational.size() == 2);
    CHECK(result->download.outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(result->flash.terminal.payload == "flashed");
    CHECK(result->flash.informational.size() == 1);
    CHECK(accepted_text(*script) == "download:00000006abcdefflash:boot");
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void pre_data_fail_is_not_sent_and_reusable() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("INFOchecking");
    script->respond("FAILtoo large");
    script->expect_write("getvar:product");
    script->respond("OKAYkairos");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_flash("boot", payload);
    CHECK(!result);
    CHECK(result.error().code == PrimitiveErrorCode::DeviceFail);
    CHECK(result.error().operation == PrimitiveOperation::Download);
    CHECK(result.error().phase == ProtocolPhase::InitialResponse);
    CHECK(result.error().device_message == "too large");
    CHECK(result.error().informational.size() == 1);
    CHECK(result.error().outbound_certainty == TransferCertainty::NotTransferred);
    CHECK(!result.error().session_poisoned);
    CHECK(accepted_text(*script) == "download:00000006");
    CHECK(session.state() == SessionState::Ready);

    CHECK(service.getvar("product").has_value());
    CHECK(script->complete());
}

void post_payload_fail_is_fully_transferred_and_reusable() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("DATA00000006");
    script->expect_write(payload);
    script->respond("INFOverifying");
    script->respond("FAILbad payload");
    script->expect_write("getvar:product");
    script->respond("OKAYkairos");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_flash("boot", payload);
    CHECK(!result);
    CHECK(result.error().code == PrimitiveErrorCode::DeviceFail);
    CHECK(result.error().operation == PrimitiveOperation::Download);
    CHECK(result.error().phase == ProtocolPhase::FinalResponse);
    CHECK(result.error().device_message == "bad payload");
    CHECK(result.error().outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(!result.error().session_poisoned);
    CHECK(accepted_text(*script) == "download:00000006abcdef");
    CHECK(session.state() == SessionState::Ready);

    CHECK(service.getvar("product").has_value());
    CHECK(script->complete());
}

void flash_fail_does_not_poison_or_retry() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("DATA00000006");
    script->expect_write(payload);
    script->respond("OKAY");
    script->expect_write("flash:boot");
    script->respond("INFOwriting");
    script->respond("FAILlocked");
    script->expect_write("getvar:product");
    script->respond("OKAYkairos");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_flash("boot", payload);
    CHECK(!result);
    CHECK(result.error().code == PrimitiveErrorCode::DeviceFail);
    CHECK(result.error().operation == PrimitiveOperation::Flash);
    CHECK(result.error().phase == ProtocolPhase::FinalResponse);
    CHECK(result.error().device_message == "locked");
    CHECK(result.error().outbound_certainty == TransferCertainty::FullyTransferred);
    CHECK(!result.error().session_poisoned);
    CHECK(session.state() == SessionState::Ready);

    CHECK(service.getvar("product").has_value());
    CHECK(script->complete());
}

void invalid_inputs_never_touch_the_wire() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);

    CHECK(!service.getvar(""));
    CHECK(!service.erase(""));
    CHECK(!service.oem(""));
    CHECK(!service.set_active(""));
    CHECK(!service.raw_command(""));
    CHECK(!service.getvar(std::string_view{"bad\tkey", 7}));
    CHECK(!service.set_active(std::string_view{"bad\nslot", 8}));
    CHECK(!service.raw_command(std::string_view{"getvar:\n", 8}));
    const std::array non_ascii{static_cast<char>(0xC3), static_cast<char>(0xA9)};
    CHECK(!service.erase(std::string_view(non_ascii.data(), non_ascii.size())));
    const std::string oversized(4090, 'x');
    CHECK(!service.getvar(oversized));
    CHECK(!service.set_active(std::string(
        4096U - std::string_view{"set_active:"}.size() + 1U, 'a')));
    CHECK(!service.raw_command(std::string(4097, 'x')));

    const std::span<const std::byte> empty;
    const auto zero = service.download(empty);
    CHECK(!zero);
    CHECK(zero.error().code == PrimitiveErrorCode::InvalidArgument);
    CHECK(zero.error().outbound_certainty == TransferCertainty::NotTransferred);
    const auto zero_boot = service.download_and_boot(empty);
    CHECK(!zero_boot);
    CHECK(zero_boot.error().code == PrimitiveErrorCode::InvalidArgument);
    CHECK(zero_boot.error().operation == PrimitiveOperation::Download);
    CHECK(zero_boot.error().outbound_certainty ==
          TransferCertainty::NotTransferred);

    const auto too_large = validate_download_size(
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U);
    CHECK(!too_large);
    CHECK(too_large.error().code == PrimitiveErrorCode::InvalidArgument);
    CHECK(validate_download_size(std::numeric_limits<std::uint32_t>::max()));

    const auto payload = to_bytes("x");
    CHECK(!service.download_and_flash("", payload));
    CHECK(!service.reboot(static_cast<RebootTarget>(0xFF)));
    CHECK(accepted_text(*script).empty());
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void command_size_limit_is_inclusive() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const std::string key(4096U - std::string_view{"getvar:"}.size(), 'x');
    const std::string command_text = "getvar:" + key;
    script->expect_write(command_text);
    script->respond("OKAYvalue");
    const std::string slot(
        4096U - std::string_view{"set_active:"}.size(), 'a');
    script->expect_write("set_active:" + slot);
    script->respond("OKAY");
    const std::string raw(4096, 'r');
    script->expect_write(raw);
    script->respond("OKAY");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.getvar(key);
    CHECK(result.has_value());
    CHECK(result->terminal.payload == "value");
    CHECK(service.set_active(slot).has_value());
    CHECK(service.raw_command(raw).has_value());
    CHECK(script->complete());
}

void cancellation_and_native_codes_are_preserved() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write(
            "getvar:product",
            0,
            TransportStatus::Cancelled,
            TransferCertainty::NotTransferred,
            125,
            "cancelled by caller");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.getvar("product");
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::Cancelled);
        CHECK(result.error().phase == ProtocolPhase::CommandWrite);
        CHECK(result.error().native_code == 125);
        CHECK(result.error().outbound_certainty == TransferCertainty::NotTransferred);
        CHECK(result.error().session_poisoned);
        CHECK(session.state() == SessionState::Poisoned);
        const auto retry = service.getvar("product");
        CHECK(!retry);
        CHECK(retry.error().code == PrimitiveErrorCode::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write(
            "set_active:b",
            4,
            TransportStatus::Cancelled,
            TransferCertainty::PartialOrUnknown,
            125,
            "cancelled while selecting slot");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.set_active("b");
        CHECK(!result);
        CHECK(result.error().operation == PrimitiveOperation::SetActive);
        CHECK(result.error().code == PrimitiveErrorCode::Cancelled);
        CHECK(result.error().phase == ProtocolPhase::CommandWrite);
        CHECK(result.error().transport_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().native_code == 125);
        CHECK(result.error().session_poisoned);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(accepted_text(*script) == "set_");
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        service.request_cancel();
        const auto result = service.raw_command("getvar:product");
        CHECK(!result);
        CHECK(result.error().operation == PrimitiveOperation::RawCommand);
        CHECK(result.error().code == PrimitiveErrorCode::Cancelled);
        CHECK(result.error().phase == ProtocolPhase::Validation);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(result.error().session_poisoned);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->cancellation_requested());
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        service.request_cancel();
        CHECK(script->cancellation_requested());
        const auto result = service.getvar("product");
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::Cancelled);
        CHECK(result.error().phase == ProtocolPhase::Validation);
        CHECK(result.error().outbound_certainty == TransferCertainty::NotTransferred);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:product");
        script->respond(
            "",
            TransportStatus::Timeout,
            TransferCertainty::NotTransferred,
            false,
            std::nullopt,
            60,
            "response deadline");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.getvar("product");
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::Timeout);
        CHECK(result.error().phase == ProtocolPhase::FinalResponse);
        CHECK(result.error().transport_certainty == TransferCertainty::NotTransferred);
        CHECK(result.error().outbound_certainty == TransferCertainty::FullyTransferred);
        CHECK(result.error().native_code == 60);
        CHECK(result.error().session_poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write(
            "getvar:product",
            std::nullopt,
            TransportStatus::IoError,
            TransferCertainty::FullyTransferred,
            32,
            "peer rejected framed write");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.getvar("product");
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::TransportIo);
        CHECK(result.error().phase == ProtocolPhase::CommandWrite);
        CHECK(result.error().transport_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(result.error().native_code == 32);
        CHECK(result.error().session_poisoned);
        CHECK(script->complete());
    }
}

void cancelled_payload_is_partial_and_poisoned() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("DATA00000006");
    script->expect_write(
        payload,
        2,
        TransportStatus::Cancelled,
        TransferCertainty::PartialOrUnknown,
        125,
        "cancelled while downloading");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_flash("boot", payload);
    CHECK(!result);
    CHECK(result.error().code == PrimitiveErrorCode::Cancelled);
    CHECK(result.error().operation == PrimitiveOperation::Download);
    CHECK(result.error().phase == ProtocolPhase::DataWrite);
    CHECK(result.error().outbound_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(result.error().native_code == 125);
    CHECK(result.error().session_poisoned);
    CHECK(accepted_text(*script) == "download:00000006ab");
    CHECK(script->complete());
}

void malformed_download_handshake_poisons_without_payload() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("DATA00000005");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_flash("boot", payload);
    CHECK(!result);
    CHECK(result.error().code == PrimitiveErrorCode::ProtocolViolation);
    CHECK(result.error().phase == ProtocolPhase::InitialResponse);
    CHECK(result.error().outbound_certainty == TransferCertainty::NotTransferred);
    CHECK(result.error().session_poisoned);
    CHECK(accepted_text(*script) == "download:00000006");
    CHECK(session.state() == SessionState::Poisoned);
    CHECK(script->complete());
}

void terminal_device_fail_does_not_retire_session() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("continue");
    script->respond("FAILnot ready");
    script->expect_write("getvar:product");
    script->respond("OKAYkairos");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto continued = service.continue_boot();
    CHECK(!continued);
    CHECK(continued.error().code == PrimitiveErrorCode::DeviceFail);
    CHECK(!continued.error().session_poisoned);
    CHECK(session.state() == SessionState::Ready);
    CHECK(service.getvar("product").has_value());
    CHECK(script->complete());
}

void source_download_uses_bounded_random_reads() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdefghij");
    script->expect_write("download:0000000a");
    script->respond("DATA0000000a");
    script->expect_source_write(
        payload,
        {
            {.offset = 6, .size = 4, .progress_watermark = 0},
            {.offset = 0, .size = 3, .progress_watermark = 3},
            {.offset = 3, .size = 3, .progress_watermark = 10},
        });
    script->respond("OKAYstaged");
    script->expect_write("flash:boot");
    script->respond("OKAYflashed");

    auto source = std::make_shared<RecordingSource>(payload, 4);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_flash_source(
        "boot",
        source,
        [&progress](const std::uint64_t watermark, const std::uint64_t total) {
            progress.emplace_back(watermark, total);
            return TransferProgressAction::continue_transfer;
        });

    CHECK(result.has_value());
    CHECK(result->download.terminal.payload == "staged");
    CHECK(result->flash.terminal.payload == "flashed");
    constexpr std::array expected_reads{
        RecordingSource::Read{6, 4},
        RecordingSource::Read{0, 3},
        RecordingSource::Read{3, 3},
    };
    CHECK(std::ranges::equal(source->reads(), expected_reads));
    const std::vector expected_progress{
        std::pair<std::uint64_t, std::uint64_t>{3, 10},
        std::pair<std::uint64_t, std::uint64_t>{10, 10},
    };
    CHECK(progress == expected_progress);
    CHECK(accepted_text(*script) ==
          "download:0000000aabcdefghijflash:boot");
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void source_download_and_boot_is_terminal() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdefghij");
    script->expect_write("download:0000000a");
    script->respond("DATA0000000a");
    script->expect_source_write(
        payload,
        {
            {.offset = 6, .size = 4, .progress_watermark = 0},
            {.offset = 0, .size = 3, .progress_watermark = 3},
            {.offset = 3, .size = 3, .progress_watermark = 10},
        });
    script->respond("OKAYstaged");
    script->expect_write("boot");
    script->respond("OKAYbooting");

    auto source = std::make_shared<RecordingSource>(payload, 4);
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_and_boot_source(source);

    CHECK(result.has_value());
    CHECK(result->download.terminal.payload == "staged");
    CHECK(result->boot.terminal.payload == "booting");
    constexpr std::array expected_reads{
        RecordingSource::Read{6, 4},
        RecordingSource::Read{0, 3},
        RecordingSource::Read{3, 3},
    };
    CHECK(std::ranges::equal(source->reads(), expected_reads));
    CHECK(accepted_text(*script) == "download:0000000aabcdefghijboot");
    CHECK(session.state() == SessionState::Closed);
    CHECK(script->closed());
    CHECK(script->complete());
}

void source_device_fail_phases_are_reusable() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        const auto payload = to_bytes("abcdef");
        script->expect_write("download:00000006");
        script->respond("INFOchecking");
        script->respond("FAILtoo large");
        script->expect_write("getvar:product");
        script->respond("OKAYkairos");

        auto source = std::make_shared<RecordingSource>(payload, payload.size());
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.download_source(source);
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::DeviceFail);
        CHECK(result.error().phase == ProtocolPhase::InitialResponse);
        CHECK(result.error().device_message == "too large");
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(!result.error().session_poisoned);
        CHECK(source->reads().empty());
        CHECK(session.state() == SessionState::Ready);
        CHECK(service.getvar("product").has_value());
        CHECK(script->complete());
    }

    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        const auto payload = to_bytes("abcdef");
        script->expect_write("download:00000006");
        script->respond("DATA00000006");
        script->expect_source_write(
            payload,
            {{.offset = 0, .size = 6, .progress_watermark = 6}});
        script->respond("INFOverifying");
        script->respond("FAILbad payload");
        script->expect_write("getvar:product");
        script->respond("OKAYkairos");

        auto source = std::make_shared<RecordingSource>(payload, payload.size());
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.download_source(source);
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::DeviceFail);
        CHECK(result.error().phase == ProtocolPhase::FinalResponse);
        CHECK(result.error().device_message == "bad payload");
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::FullyTransferred);
        CHECK(!result.error().session_poisoned);
        CHECK(source->reads().size() == 1);
        CHECK(session.state() == SessionState::Ready);
        CHECK(service.getvar("product").has_value());
        CHECK(script->complete());
    }
}

void source_data_mismatch_never_reads_payload() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("DATA00000005");

    auto source = std::make_shared<RecordingSource>(payload, payload.size());
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.download_source(source);
    CHECK(!result);
    CHECK(result.error().code == PrimitiveErrorCode::ProtocolViolation);
    CHECK(result.error().phase == ProtocolPhase::InitialResponse);
    CHECK(result.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(result.error().session_poisoned);
    CHECK(source->reads().empty());
    CHECK(accepted_text(*script) == "download:00000006");
    CHECK(session.state() == SessionState::Poisoned);
    CHECK(script->complete());
}

void source_cancel_and_failure_poison_with_native_codes() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        const auto payload = to_bytes("abcdef");
        script->expect_write("download:00000006");
        script->respond("DATA00000006");
        script->expect_source_write(
            payload,
            {
                {.offset = 0, .size = 3, .progress_watermark = 3},
                {.offset = 3, .size = 3, .progress_watermark = 6},
            },
            std::nullopt,
            TransportStatus::Ok,
            TransferCertainty::FullyTransferred,
            125);

        auto source = std::make_shared<RecordingSource>(payload, 3);
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.download_source(
            source,
            [](std::uint64_t, std::uint64_t) {
                return TransferProgressAction::cancel;
            });
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::Cancelled);
        CHECK(result.error().phase == ProtocolPhase::DataWrite);
        CHECK(result.error().transport_status == TransportStatus::Cancelled);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::PartialOrUnknown);
        CHECK(result.error().native_code == 125);
        CHECK(result.error().session_poisoned);
        CHECK(source->reads().size() == 1);
        CHECK(accepted_text(*script) == "download:00000006abc");
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }

    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        const auto payload = to_bytes("abcdef");
        script->expect_write("download:00000006");
        script->respond("DATA00000006");
        script->expect_source_write(
            payload,
            {{.offset = 0, .size = 3, .progress_watermark = 3}},
            0,
            TransportStatus::IoError,
            TransferCertainty::NotTransferred,
            5,
            "source read failed");

        auto source = std::make_shared<RecordingSource>(payload, 3, 0);
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.download_source(source);
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::TransportIo);
        CHECK(result.error().phase == ProtocolPhase::DataWrite);
        CHECK(result.error().transport_status == TransportStatus::IoError);
        CHECK(result.error().transport_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(result.error().native_code == 5);
        CHECK(result.error().session_poisoned);
        CHECK(source->reads().size() == 1);
        CHECK(accepted_text(*script) == "download:00000006");
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
}

void source_preflight_and_capability_fail_without_wire_io() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);

        const auto null_source = service.download_source(nullptr);
        CHECK(!null_source);
        CHECK(null_source.error().code == PrimitiveErrorCode::InvalidArgument);
        CHECK(null_source.error().phase == ProtocolPhase::Validation);

        const auto zero = service.download_source(
            std::make_shared<SizedSource>(0));
        CHECK(!zero);
        CHECK(zero.error().code == PrimitiveErrorCode::InvalidArgument);

        const auto null_boot = service.download_and_boot_source(nullptr);
        CHECK(!null_boot);
        CHECK(null_boot.error().code == PrimitiveErrorCode::InvalidArgument);
        CHECK(null_boot.error().operation == PrimitiveOperation::Download);

        const auto zero_boot = service.download_and_boot_source(
            std::make_shared<SizedSource>(0));
        CHECK(!zero_boot);
        CHECK(zero_boot.error().code == PrimitiveErrorCode::InvalidArgument);
        CHECK(zero_boot.error().operation == PrimitiveOperation::Download);

        const auto too_large = service.download_source(
            std::make_shared<SizedSource>(
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max()) + 1U));
        CHECK(!too_large);
        CHECK(too_large.error().code == PrimitiveErrorCode::InvalidArgument);

        const auto invalid_partition = service.download_and_flash_source(
            "bad\npartition",
            std::make_shared<SizedSource>(1));
        CHECK(!invalid_partition);
        CHECK(invalid_partition.error().code ==
              PrimitiveErrorCode::InvalidArgument);

        CHECK(accepted_text(*script).empty());
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Ready);
    }

    {
        auto transport = std::make_unique<NonStreamingTransport>();
        auto* raw_transport = transport.get();
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        const auto result = service.download_source(
            std::make_shared<RecordingSource>(to_bytes("x"), 1));
        CHECK(!result);
        CHECK(result.error().code == PrimitiveErrorCode::Unsupported);
        CHECK(result.error().phase == ProtocolPhase::Validation);
        CHECK(result.error().outbound_certainty ==
              TransferCertainty::NotTransferred);
        CHECK(!result.error().session_poisoned);
        CHECK(raw_transport->wire_calls == 0);
        CHECK(session.state() == SessionState::Ready);
    }
}

void slot_selection_validation_has_no_wire_effects() {
    const auto current = parse_slot_selection("");
    CHECK(current.has_value());
    CHECK(current->kind == SlotSelectionKind::Current);

    const auto legacy_a = parse_slot_selection("_a");
    CHECK(legacy_a.has_value());
    CHECK(legacy_a->kind == SlotSelectionKind::Explicit);
    CHECK(legacy_a->name == "a");

    CHECK(parse_slot_selection("other")->kind == SlotSelectionKind::Other);
    CHECK(parse_slot_selection("all")->kind == SlotSelectionKind::All);
    CHECK(!parse_slot_selection("current"));
    CHECK(!parse_slot_selection("aa"));
    CHECK(!parse_slot_selection("A"));

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    SlotPlanner planner(service);

    const auto invalid_partition = planner.plan_partition("bad:partition", "a");
    CHECK(!invalid_partition);
    CHECK(invalid_partition.error().code == SlotErrorCode::InvalidArgument);
    const auto invalid_slot = planner.plan_partition("boot", "slot-a");
    CHECK(!invalid_slot);
    CHECK(invalid_slot.error().code == SlotErrorCode::InvalidArgument);
    const auto active_all = planner.resolve_active_slot("all");
    CHECK(!active_all);
    CHECK(active_all.error().code == SlotErrorCode::InvalidArgument);
    CHECK(accepted_text(*script).empty());
    CHECK(script->complete());
}

void modern_slot_queries_generate_deterministic_plans() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:has-slot:boot");
    script->respond("OKAYyes");
    script->expect_write("getvar:slot-count");
    script->respond("OKAY2");
    script->expect_write("getvar:current-slot");
    script->respond("OKAY_b");

    script->expect_write("getvar:has-slot:system");
    script->respond("OKAYyes");
    script->expect_write("getvar:slot-count");
    script->respond("OKAY2");

    script->expect_write("getvar:slot-count");
    script->respond("OKAY2");
    script->expect_write("getvar:current-slot");
    script->respond("OKAYa");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    SlotPlanner planner(service);

    const auto current = planner.plan_partition("boot");
    CHECK(current.has_value());
    CHECK(current->slotted);
    CHECK(current->partition_names == std::vector<std::string>({"boot_b"}));
    CHECK(current->slots == std::vector<std::string>({"b"}));

    const auto all = planner.plan_partition("system", "all");
    CHECK(all.has_value());
    const std::vector<std::string> expected_names{"system_a", "system_b"};
    const std::vector<std::string> expected_slots{"a", "b"};
    CHECK(all->partition_names == expected_names);
    CHECK(all->slots == expected_slots);

    const auto other = planner.resolve_active_slot("other");
    CHECK(other.has_value());
    CHECK(*other == "b");
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void legacy_suffixes_are_normalized_and_sorted() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:has-slot:vendor_boot");
    script->respond("OKAYyes");
    script->expect_write("getvar:slot-count");
    script->respond("FAILunknown variable");
    script->expect_write("getvar:slot-suffixes");
    script->respond("OKAY_b,_a");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    SlotPlanner planner(service);

    const auto result = planner.plan_partition("vendor_boot", "_b");
    CHECK(result.has_value());
    CHECK(result->partition_names ==
          std::vector<std::string>{"vendor_boot_b"});
    CHECK(result->slots == std::vector<std::string>({"b"}));
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);

    auto topology_transport = std::make_unique<ScriptedTransport>();
    auto* topology_script = topology_transport.get();
    topology_script->expect_write("getvar:slot-count");
    topology_script->respond("FAILlegacy");
    topology_script->expect_write("getvar:slot-suffixes");
    topology_script->respond("OKAY_b,_a");
    FastbootSession topology_session(std::move(topology_transport));
    PrimitiveService topology_service(topology_session);
    SlotPlanner topology_planner(topology_service);
    const auto topology = topology_planner.query_topology();
    CHECK(topology.has_value());
    CHECK(topology->source == SlotTopologySource::LegacySlotSuffixes);
    CHECK(topology->slots == std::vector<std::string>({"a", "b"}));
    CHECK(topology_script->complete());
}

void non_slotted_partitions_are_not_silently_forced() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:has-slot:userdata");
    script->respond("OKAYno");
    script->expect_write("getvar:has-slot:userdata");
    script->respond("OKAYno");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    SlotPlanner planner(service);

    const auto implicit = planner.plan_partition("userdata");
    CHECK(implicit.has_value());
    CHECK(!implicit->slotted);
    CHECK(implicit->partition_names ==
          std::vector<std::string>{"userdata"});
    CHECK(implicit->slots.empty());

    const auto forced = planner.plan_partition("userdata", "a");
    CHECK(!forced);
    CHECK(forced.error().code == SlotErrorCode::Unsupported);
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void unsupported_and_failed_queries_are_distinct() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:has-slot:boot");
        script->respond("FAILunknown variable");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        SlotPlanner planner(service);

        const auto result = planner.plan_partition("boot");
        CHECK(!result);
        CHECK(result.error().code == SlotErrorCode::Unsupported);
        CHECK(result.error().query_error.has_value());
        CHECK(result.error().query_error->code == PrimitiveErrorCode::DeviceFail);
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Ready);
    }

    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:has-slot:boot");
        script->respond(
            "",
            TransportStatus::Timeout,
            TransferCertainty::NotTransferred,
            false,
            std::nullopt,
            60,
            "deadline");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        SlotPlanner planner(service);

        const auto result = planner.plan_partition("boot");
        CHECK(!result);
        CHECK(result.error().code == SlotErrorCode::QueryFailed);
        CHECK(result.error().query_error.has_value());
        CHECK(result.error().query_error->code == PrimitiveErrorCode::Timeout);
        CHECK(result.error().query_error->native_code == 60);
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Poisoned);
    }
}

void malformed_slot_metadata_is_never_guessed() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:has-slot:boot");
        script->respond("OKAYyes");
        script->expect_write("getvar:slot-count");
        script->respond("OKAY2x");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        SlotPlanner planner(service);

        const auto result = planner.plan_partition("boot", "a");
        CHECK(!result);
        CHECK(result.error().code == SlotErrorCode::InvalidDeviceResponse);
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Ready);
    }

    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:slot-count");
        script->respond("FAILlegacy");
        script->expect_write("getvar:slot-suffixes");
        script->respond("OKAY_a,a");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        SlotPlanner planner(service);

        const auto result = planner.resolve_active_slot("a");
        CHECK(!result);
        CHECK(result.error().code == SlotErrorCode::Ambiguous);
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Ready);
    }

    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:has-slot:boot");
        script->respond("OKAYmaybe");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        SlotPlanner planner(service);

        const auto result = planner.plan_partition("boot");
        CHECK(!result);
        CHECK(result.error().code == SlotErrorCode::InvalidDeviceResponse);
        CHECK(script->complete());
    }

    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:has-slot:boot");
        script->respond("OKAYyes");
        script->expect_write("getvar:slot-count");
        script->respond("OKAY2");
        FastbootSession session(std::move(transport));
        PrimitiveService service(session);
        SlotPlanner planner(service);

        const auto result = planner.plan_partition("boot", "c");
        CHECK(!result);
        CHECK(result.error().code == SlotErrorCode::InvalidArgument);
        CHECK(script->complete());
        CHECK(session.state() == SessionState::Ready);
    }
}

void other_slot_rejects_ambiguous_topologies() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:slot-count");
    script->respond("OKAY3");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    SlotPlanner planner(service);
    const auto result = planner.resolve_active_slot("other");
    CHECK(!result);
    CHECK(result.error().code == SlotErrorCode::Ambiguous);
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void current_slot_must_belong_to_discovered_topology() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:has-slot:boot");
    script->respond("OKAYyes");
    script->expect_write("getvar:slot-count");
    script->respond("OKAY2");
    script->expect_write("getvar:current-slot");
    script->respond("OKAYc");

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    SlotPlanner planner(service);
    const auto result = planner.plan_partition("boot");
    CHECK(!result);
    CHECK(result.error().code == SlotErrorCode::InvalidDeviceResponse);
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

void sparse_flash_parts_use_independent_canonical_downloads() {
    std::vector<std::byte> raw(3 * 4096);
    for (std::size_t index = 0; index < raw.size(); ++index) {
        raw[index] = std::byte{
            static_cast<unsigned char>((index * 17 + 3) % 251)};
    }
    auto artifact = FlashArtifact::inspect(
        std::make_shared<MemoryImageSource>(std::move(raw)));
    CHECK(artifact.has_value());
    auto plan = SparseFlashPlan::create(*artifact, 4200);
    CHECK(plan.has_value());
    CHECK(plan->parts().size() == 3);

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    std::vector<std::vector<std::byte>> encoded_parts;
    encoded_parts.reserve(plan->parts().size());
    constexpr std::array expected_sizes{
        std::uint64_t{4148}, std::uint64_t{4160}, std::uint64_t{4148}};
    constexpr std::array<std::string_view, 3> expected_downloads{
        "download:00001034", "download:00001040", "download:00001034"};
    for (std::size_t index = 0; index < plan->parts().size(); ++index) {
        const auto& part = plan->parts()[index];
        CHECK(part.source->size() == expected_sizes[index]);
        encoded_parts.push_back(read_image(part.source));
        script->expect_write(expected_downloads[index]);
        script->respond(
            std::string{"DATA"} +
            std::string{expected_downloads[index].substr(9)});
        script->expect_source_write(
            encoded_parts.back(),
            {{.offset = 0,
              .size = encoded_parts.back().size(),
              .progress_watermark = encoded_parts.back().size()}});
        script->respond("OKAYstaged");
        script->expect_write("flash:system");
        script->respond("OKAYflashed");
    }

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    for (const auto& part : plan->parts()) {
        auto source = ImageTransferSource::create(part.source);
        CHECK(source.has_value());
        auto flashed = service.download_and_flash_source("system", *source);
        CHECK(flashed.has_value());
    }
    CHECK(script->complete());
    CHECK(session.state() == SessionState::Ready);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"exact primitive commands", exact_non_data_commands_are_emitted},
        {"set-active and raw response order",
         set_active_and_raw_commands_preserve_response_order},
        {"raw FAIL response order", raw_fail_preserves_response_order_and_session},
        {"raw DATA rejection", raw_data_response_is_unsupported_and_poisons_session},
        {"protocol error informational history",
         protocol_errors_preserve_bounded_informational_history},
        {"terminal reboot and continue", reboot_variants_and_continue_are_terminal},
        {"boot exact command and retirement",
         boot_downloaded_emits_exact_command_and_retires},
        {"boot FAIL history and retry", boot_fail_preserves_history_and_session},
        {"canonical download and boot trace", canonical_download_and_boot_trace},
        {"boot download failures", boot_download_failures_do_not_issue_boot},
        {"boot command partial failure", boot_command_partial_failure_is_poisoned},
        {"canonical download and flash trace", canonical_download_and_flash_trace},
        {"pre-DATA FAIL", pre_data_fail_is_not_sent_and_reusable},
        {"post-payload FAIL", post_payload_fail_is_fully_transferred_and_reusable},
        {"flash FAIL", flash_fail_does_not_poison_or_retry},
        {"invalid inputs have no wire effects", invalid_inputs_never_touch_the_wire},
        {"command size limit is inclusive", command_size_limit_is_inclusive},
        {"cancellation and native codes", cancellation_and_native_codes_are_preserved},
        {"cancelled payload", cancelled_payload_is_partial_and_poisoned},
        {"malformed download handshake", malformed_download_handshake_poisons_without_payload},
        {"terminal FAIL remains reusable", terminal_device_fail_does_not_retire_session},
        {"source bounded random reads", source_download_uses_bounded_random_reads},
        {"source download and boot", source_download_and_boot_is_terminal},
        {"source FAIL phases remain reusable", source_device_fail_phases_are_reusable},
        {"source DATA mismatch", source_data_mismatch_never_reads_payload},
        {"source cancellation and failure", source_cancel_and_failure_poison_with_native_codes},
        {"source preflight and capability", source_preflight_and_capability_fail_without_wire_io},
        {"slot selection validation", slot_selection_validation_has_no_wire_effects},
        {"modern slot planning", modern_slot_queries_generate_deterministic_plans},
        {"legacy slot suffixes", legacy_suffixes_are_normalized_and_sorted},
        {"non-slotted partition planning", non_slotted_partitions_are_not_silently_forced},
        {"slot query error classes", unsupported_and_failed_queries_are_distinct},
        {"malformed slot metadata", malformed_slot_metadata_is_never_guessed},
        {"ambiguous other slot", other_slot_rejects_ambiguous_topologies},
        {"current slot topology membership", current_slot_must_belong_to_discovered_topology},
        {"sparse multi-part flash trace",
         sparse_flash_parts_use_independent_canonical_downloads},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " primitive service test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " primitive service tests passed\n";
    return 0;
}
