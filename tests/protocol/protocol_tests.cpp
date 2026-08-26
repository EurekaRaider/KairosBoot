// SPDX-License-Identifier: MIT
#include "fastboot_protocol.hpp"
#include "scripted_transport.hpp"

#include <array>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::protocol::CommandResult;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::ProtocolErrorCode;
using kairosboot::protocol::ResponseKind;
using kairosboot::protocol::ResponseParseErrorCode;
using kairosboot::protocol::SessionOptions;
using kairosboot::protocol::SessionState;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransportStatus;
using kairosboot::protocol::parse_response;
using kairosboot::protocol::test::ScriptedTransport;
using kairosboot::protocol::test::to_bytes;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            throw CheckFailure(std::string("check failed: ") + #condition + " at line " + \
                               std::to_string(__LINE__));                                     \
        }                                                                                    \
    } while (false)

[[nodiscard]] auto parse(
    const std::string_view packet,
    const std::size_t limit = kairosboot::protocol::kDefaultMaxResponseBytes) {
    const auto bytes = to_bytes(packet);
    return parse_response(bytes, limit);
}

void parser_accepts_all_response_kinds() {
    const std::array cases{
        std::pair{"INFOprogress", ResponseKind::Info},
        std::pair{"TEXTdetails", ResponseKind::Text},
        std::pair{"OKAYdone", ResponseKind::Okay},
        std::pair{"FAILdenied", ResponseKind::Fail},
    };
    for (const auto& [packet, kind] : cases) {
        const auto result = parse(packet);
        CHECK(result.has_value());
        CHECK(result->kind == kind);
        CHECK(!result->data_size.has_value());
    }

    const auto data = parse("DATA000000aF");
    CHECK(data.has_value());
    CHECK(data->kind == ResponseKind::Data);
    CHECK(data->data_size == 0xAFU);
}

void parser_rejects_malformed_packets() {
    const auto zlp = parse("");
    CHECK(!zlp);
    CHECK(zlp.error().code == ResponseParseErrorCode::ZeroLength);

    const auto short_packet = parse("OK");
    CHECK(!short_packet);
    CHECK(short_packet.error().code == ResponseParseErrorCode::TooShort);

    const auto oversized = parse("OKAYx", 4);
    CHECK(!oversized);
    CHECK(oversized.error().code == ResponseParseErrorCode::TooLong);

    const auto unknown = parse("NOPEvalue");
    CHECK(!unknown);
    CHECK(unknown.error().code == ResponseParseErrorCode::UnknownPrefix);

    const auto wrong_data_length = parse("DATA1234");
    CHECK(!wrong_data_length);
    CHECK(wrong_data_length.error().code == ResponseParseErrorCode::InvalidDataLength);

    const auto illegal_hex = parse("DATA0000000g");
    CHECK(!illegal_hex);
    CHECK(illegal_hex.error().code == ResponseParseErrorCode::InvalidDataHex);
}

void official_packet_boundaries_are_enforced() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();

    const std::string max_command(4096, 'x');
    std::string max_response = "OKAY";
    max_response.append(252, 'r');
    script->expect_write(max_command);
    script->respond(max_response);

    script->expect_write("getvar:x");
    std::string oversized_response = "OKAY";
    oversized_response.append(253, 'r');
    script->respond(oversized_response);

    FastbootSession session(std::move(transport));
    const auto at_limit = session.command(max_command);
    CHECK(at_limit.has_value());
    CHECK(at_limit->succeeded());
    CHECK(at_limit->terminal.payload.size() == 252);
    CHECK(session.state() == SessionState::Ready);

    const std::string oversized_command(4097, 'x');
    const auto command_too_long = session.command(oversized_command);
    CHECK(!command_too_long);
    CHECK(command_too_long.error().code == ProtocolErrorCode::InvalidArgument);
    CHECK(session.state() == SessionState::Ready);

    const auto response_too_long = session.command("getvar:x");
    CHECK(!response_too_long);
    CHECK(response_too_long.error().code == ProtocolErrorCode::MalformedResponse);
    CHECK(session.state() == SessionState::Poisoned);
    CHECK(script->complete());
}

void command_collects_info_and_text() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:product");
    script->respond("INFOchecking");
    script->respond("TEXTproduct follows");
    script->respond("OKAYkairos");

    FastbootSession session(std::move(transport));
    const auto result = session.command("getvar:product");
    CHECK(result.has_value());
    CHECK(result->succeeded());
    CHECK(result->terminal.payload == "kairos");
    CHECK(result->informational.size() == 2);
    CHECK(result->informational[0].kind == ResponseKind::Info);
    CHECK(result->informational[1].kind == ResponseKind::Text);
    CHECK(session.state() == SessionState::Ready);
    CHECK(script->complete());
}

void valid_fail_keeps_session_reusable() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("erase:locked");
    script->respond("FAILnot allowed");
    script->expect_write("getvar:product");
    script->respond("OKAYkairos");

    FastbootSession session(std::move(transport));
    const auto failed = session.command("erase:locked");
    CHECK(failed.has_value());
    CHECK(!failed->succeeded());
    CHECK(failed->terminal.payload == "not allowed");
    CHECK(session.state() == SessionState::Ready);

    const auto next = session.command("getvar:product");
    CHECK(next.has_value());
    CHECK(next->succeeded());
    CHECK(script->complete());
}

void successful_short_writes_are_completed_exactly() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("getvar:version", 4);
    script->expect_write("ar:version");
    script->respond("OKAY0.4");

    FastbootSession session(std::move(transport));
    const auto result = session.command("getvar:version");
    CHECK(result.has_value());
    CHECK(result->succeeded());
    CHECK(script->accepted_bytes() == to_bytes("getvar:version"));
    CHECK(script->complete());
}

void download_requires_and_writes_exact_length() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("INFOpreparing");
    script->respond("DATA00000006");
    script->expect_write(payload, 2);
    script->expect_write(std::span<const std::byte>(payload).subspan(2));
    script->respond("TEXTwriting");
    script->respond("OKAYcomplete");

    FastbootSession session(std::move(transport));
    const auto result = session.download(payload);
    CHECK(result.has_value());
    CHECK(result->succeeded());
    CHECK(result->terminal.payload == "complete");
    CHECK(result->informational.size() == 2);
    CHECK(script->accepted_bytes() == to_bytes("download:00000006abcdef"));
    CHECK(script->complete());
}

void zero_length_download_is_well_defined() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("download:00000000");
    script->respond("DATA00000000");
    script->respond("OKAY");

    FastbootSession session(std::move(transport));
    const std::span<const std::byte> empty;
    const auto result = session.download(empty);
    CHECK(result.has_value());
    CHECK(result->succeeded());
    CHECK(script->complete());
}

void download_length_mismatch_poisons_before_payload() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("DATA00000005");

    FastbootSession session(std::move(transport));
    const auto result = session.download(payload);
    CHECK(!result);
    CHECK(result.error().code == ProtocolErrorCode::DataLengthMismatch);
    CHECK(session.state() == SessionState::Poisoned);
    CHECK(script->accepted_bytes() == to_bytes("download:00000006"));
    CHECK(script->complete());
}

void download_write_failure_preserves_initial_information() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    const auto payload = to_bytes("abcdef");
    script->expect_write("download:00000006");
    script->respond("INFOpreparing payload");
    script->respond("DATA00000006");
    script->expect_write(
        payload,
        2,
        TransportStatus::IoError,
        TransferCertainty::PartialOrUnknown,
        5,
        "payload write failed");

    FastbootSession session(std::move(transport));
    const auto result = session.download(payload);
    CHECK(!result);
    CHECK(result.error().code == ProtocolErrorCode::TransportIo);
    CHECK(result.error().informational.size() == 1);
    CHECK(result.error().informational[0].kind == ResponseKind::Info);
    CHECK(result.error().informational[0].payload == "preparing payload");
    const auto poison = session.poison_error();
    CHECK(poison.has_value());
    CHECK(poison->informational.size() == 1);
    CHECK(poison->informational[0].kind == ResponseKind::Info);
    CHECK(poison->informational[0].payload == "preparing payload");
    CHECK(session.state() == SessionState::Poisoned);
    CHECK(script->accepted_bytes() == to_bytes("download:00000006ab"));
    CHECK(script->complete());
}

void timeout_and_partial_unknown_poison_session() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write(
        "getvar:product",
        3,
        TransportStatus::Timeout,
        TransferCertainty::PartialOrUnknown);

    FastbootSession session(std::move(transport));
    const auto result = session.command("getvar:product");
    CHECK(!result);
    CHECK(result.error().code == ProtocolErrorCode::TransportTimeout);
    CHECK(result.error().transfer_certainty == TransferCertainty::PartialOrUnknown);
    CHECK(session.state() == SessionState::Poisoned);

    const auto retry = session.command("getvar:product");
    CHECK(!retry);
    CHECK(retry.error().code == ProtocolErrorCode::Poisoned);
    CHECK(script->complete());
}

void read_timeout_and_ambiguous_success_poison_session() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:product");
        script->respond("", TransportStatus::Timeout, TransferCertainty::NotTransferred);

        FastbootSession session(std::move(transport));
        const auto result = session.command("getvar:product");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::TransportTimeout);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write(
            "getvar:product",
            std::nullopt,
            TransportStatus::Ok,
            TransferCertainty::PartialOrUnknown);

        FastbootSession session(std::move(transport));
        const auto result = session.command("getvar:product");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::TransportContractViolation);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
}

void malformed_wire_responses_poison_session() {
    const std::array packets{"", "OK", "DATA0000000g", "NOPEvalue"};
    for (const auto packet : packets) {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:product");
        script->respond(packet);

        FastbootSession session(std::move(transport));
        const auto result = session.command("getvar:product");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::MalformedResponse);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
}

void oversized_and_truncated_responses_poison_session() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:x");
        script->respond("OKAY12345");
        SessionOptions options;
        options.max_response_bytes = 8;
        FastbootSession session(std::move(transport), options);
        const auto result = session.command("getvar:x");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::MalformedResponse);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:x");
        script->respond("OKAY", TransportStatus::Ok, TransferCertainty::FullyTransferred, true);
        FastbootSession session(std::move(transport));
        const auto result = session.command("getvar:x");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::MalformedResponse);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
}

void unexpected_data_and_info_flood_poison_session() {
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:x");
        script->respond("INFOchecking");
        script->respond("TEXTstill checking");
        script->respond("DATA00000001");
        FastbootSession session(std::move(transport));
        const auto result = session.command("getvar:x");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::UnexpectedResponse);
        CHECK(result.error().informational.size() == 2);
        CHECK(result.error().informational[0].kind == ResponseKind::Info);
        CHECK(result.error().informational[0].payload == "checking");
        CHECK(result.error().informational[1].kind == ResponseKind::Text);
        CHECK(result.error().informational[1].payload == "still checking");
        const auto poison = session.poison_error();
        CHECK(poison.has_value());
        CHECK(poison->informational.size() == 2);
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:x");
        script->respond("INFOone");
        script->respond("TEXTtwo");
        SessionOptions options;
        options.max_informational_responses = 1;
        FastbootSession session(std::move(transport), options);
        const auto result = session.command("getvar:x");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::TooManyInformationalResponses);
        CHECK(result.error().informational.size() == 1);
        CHECK(result.error().informational[0].kind == ResponseKind::Info);
        CHECK(result.error().informational[0].payload == "one");
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:x");
        script->respond("INFOwaiting");
        script->respond(
            "", TransportStatus::Timeout, TransferCertainty::NotTransferred);
        FastbootSession session(std::move(transport));
        const auto result = session.command("getvar:x");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::TransportTimeout);
        CHECK(result.error().informational.size() == 1);
        CHECK(result.error().informational[0].kind == ResponseKind::Info);
        CHECK(result.error().informational[0].payload == "waiting");
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:x");
        script->respond("INFOparsing");
        script->respond("NOPEbad");
        FastbootSession session(std::move(transport));
        const auto result = session.command("getvar:x");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::MalformedResponse);
        CHECK(result.error().informational.size() == 1);
        CHECK(result.error().informational[0].kind == ResponseKind::Info);
        CHECK(result.error().informational[0].payload == "parsing");
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
    {
        auto transport = std::make_unique<ScriptedTransport>();
        auto* script = transport.get();
        script->expect_write("getvar:x");
        script->respond("NOPEbad");
        FastbootSession session(std::move(transport));
        const auto result = session.command("getvar:x");
        CHECK(!result);
        CHECK(result.error().code == ProtocolErrorCode::MalformedResponse);
        CHECK(result.error().informational.empty());
        CHECK(session.state() == SessionState::Poisoned);
        CHECK(script->complete());
    }
}

void invalid_arguments_do_not_touch_or_poison_transport() {
    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    FastbootSession session(std::move(transport));

    const auto empty = session.command("");
    CHECK(!empty);
    CHECK(empty.error().code == ProtocolErrorCode::InvalidArgument);
    CHECK(session.state() == SessionState::Ready);

    const std::string with_newline = "getvar:x\n";
    const auto invalid = session.command(with_newline);
    CHECK(!invalid);
    CHECK(invalid.error().code == ProtocolErrorCode::InvalidArgument);
    CHECK(session.state() == SessionState::Ready);
    CHECK(script->complete());
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"parser accepts all response kinds", parser_accepts_all_response_kinds},
        {"parser rejects malformed packets", parser_rejects_malformed_packets},
        {"official packet boundaries", official_packet_boundaries_are_enforced},
        {"command collects INFO and TEXT", command_collects_info_and_text},
        {"valid FAIL keeps session reusable", valid_fail_keeps_session_reusable},
        {"successful short writes are completed exactly", successful_short_writes_are_completed_exactly},
        {"download writes exact length", download_requires_and_writes_exact_length},
        {"zero length download", zero_length_download_is_well_defined},
        {"download length mismatch poisons", download_length_mismatch_poisons_before_payload},
        {"download write errors preserve INFO",
         download_write_failure_preserves_initial_information},
        {"timeout and partial unknown poison", timeout_and_partial_unknown_poison_session},
        {"read timeout and ambiguous success poison", read_timeout_and_ambiguous_success_poison_session},
        {"malformed responses poison", malformed_wire_responses_poison_session},
        {"oversized and truncated responses poison", oversized_and_truncated_responses_poison_session},
        {"unexpected DATA and INFO flood poison", unexpected_data_and_info_flood_poison_session},
        {"invalid arguments are local", invalid_arguments_do_not_touch_or_poison_transport},
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
        std::cerr << failures << " protocol test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " protocol tests passed\n";
    return 0;
}
