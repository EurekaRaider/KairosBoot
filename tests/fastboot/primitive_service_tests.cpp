// SPDX-License-Identifier: MIT
#include "src/fastboot/primitive_service.hpp"
#include "tests/protocol/scripted_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
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
using kairosboot::protocol::ProtocolPhase;
using kairosboot::protocol::SessionState;
using kairosboot::protocol::TransferCertainty;
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
