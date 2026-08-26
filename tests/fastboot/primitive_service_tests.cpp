// SPDX-License-Identifier: MIT
#include "src/fastboot/primitive_service.hpp"
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
using kairosboot::fastboot::validate_download_size;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::ITransferSource;
using kairosboot::protocol::ITransportSession;
using kairosboot::protocol::ProtocolPhase;
using kairosboot::protocol::SessionState;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferProgressAction;
using kairosboot::protocol::TransferResult;
using kairosboot::protocol::TransportStatus;
using kairosboot::protocol::test::ScriptedTransport;
using kairosboot::protocol::test::to_bytes;

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
    CHECK(!service.getvar(std::string_view{"bad\tkey", 7}));
    const std::array non_ascii{static_cast<char>(0xC3), static_cast<char>(0xA9)};
    CHECK(!service.erase(std::string_view(non_ascii.data(), non_ascii.size())));
    const std::string oversized(4090, 'x');
    CHECK(!service.getvar(oversized));

    const std::span<const std::byte> empty;
    const auto zero = service.download(empty);
    CHECK(!zero);
    CHECK(zero.error().code == PrimitiveErrorCode::InvalidArgument);
    CHECK(zero.error().outbound_certainty == TransferCertainty::NotTransferred);

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

    FastbootSession session(std::move(transport));
    PrimitiveService service(session);
    const auto result = service.getvar(key);
    CHECK(result.has_value());
    CHECK(result->terminal.payload == "value");
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

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"exact primitive commands", exact_non_data_commands_are_emitted},
        {"terminal reboot and continue", reboot_variants_and_continue_are_terminal},
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
        {"source FAIL phases remain reusable", source_device_fail_phases_are_reusable},
        {"source DATA mismatch", source_data_mismatch_never_reads_payload},
        {"source cancellation and failure", source_cancel_and_failure_poison_with_native_codes},
        {"source preflight and capability", source_preflight_and_capability_fail_without_wire_io},
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
