// SPDX-License-Identifier: MIT
#include "src/fastboot/file_receive_service.hpp"
#include "tests/protocol/scripted_transport.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

using kairosboot::fastboot::FetchRange;
using kairosboot::fastboot::FileReceiveErrorCode;
using kairosboot::fastboot::FileReceiveService;
using kairosboot::fastboot::kMaximumFileReceiveBytes;
using kairosboot::fastboot::PrimitiveErrorCode;
using kairosboot::fastboot::PrimitiveOperation;
using kairosboot::fastboot::PrimitiveService;
using kairosboot::protocol::FastbootSession;
using kairosboot::protocol::FileTransferSinkErrorKind;
using kairosboot::protocol::ProtocolPhase;
using kairosboot::protocol::ResponseKind;
using kairosboot::protocol::SessionOptions;
using kairosboot::protocol::SessionState;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransferProgressAction;
using kairosboot::protocol::TransportStatus;
using kairosboot::protocol::test::ScriptedTransport;

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

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
            ("kairosboot-file-receive-service-" + std::to_string(stamp) +
             "-" + std::to_string(sequence.fetch_add(1U)));
        CHECK(std::filesystem::create_directory(path_));
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

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text) {
    const auto raw = std::as_bytes(std::span(text));
    return {raw.begin(), raw.end()};
}

[[nodiscard]] std::string binary(
    const std::initializer_list<unsigned int> values) {
    std::string result;
    result.reserve(values.size());
    for (const auto value : values) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void write_file(
    const std::filesystem::path& path,
    const std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    CHECK(output.good());
}

[[nodiscard]] std::vector<std::byte> read_file(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input.good());
    const auto end = input.tellg();
    CHECK(end >= 0);
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(
        reinterpret_cast<char*>(result.data()),
        static_cast<std::streamsize>(result.size()));
    CHECK(input.good() || input.eof());
    return result;
}

[[nodiscard]] std::size_t temporary_count(
    const std::filesystem::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with(".kairosboot-receive-") &&
            name.ends_with(".tmp")) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::string accepted_text(const ScriptedTransport& script) {
    std::string result;
    result.reserve(script.accepted_bytes().size());
    for (const auto byte : script.accepted_bytes()) {
        result.push_back(static_cast<char>(
            std::to_integer<unsigned char>(byte)));
    }
    return result;
}

void binary_upload_is_published_only_after_terminal_okay() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "upload.bin";
    write_file(destination, "old-complete-file");

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("upload");
    script->respond("INFOverifying");
    script->respond("DATA00000005");
    const auto payload = binary({0x00U, 0xffU, 0x41U, 0x42U, 0x7fU});
    script->respond(std::string_view(payload).substr(0, 2));
    script->respond(std::string_view(payload).substr(2, 2));
    script->respond(std::string_view(payload).substr(4, 1));
    script->respond("TEXTdurable");
    script->respond("OKAYuploaded");

    SessionOptions options;
    options.receive_chunk_bytes = 2;
    FastbootSession session(std::move(transport), options);
    PrimitiveService primitives(session);
    FileReceiveService files(primitives);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    auto observer_token = std::make_shared<std::uint8_t>(std::uint8_t{0});
    const std::weak_ptr<std::uint8_t> observer_lifetime = observer_token;
    const auto result = files.upload(
        destination,
        5,
        [&, observer_token](
            const std::uint64_t completed, const std::uint64_t total) {
            CHECK(observer_token != nullptr);
            CHECK(read_file(destination) == bytes("old-complete-file"));
            progress.emplace_back(completed, total);
            return TransferProgressAction::continue_transfer;
        });
    observer_token.reset();

    CHECK(result.has_value());
    CHECK(observer_lifetime.expired());
    CHECK(result->bytes_published == 5);
    CHECK(result->reply.terminal.kind == ResponseKind::Okay);
    CHECK(result->reply.terminal.payload == "uploaded");
    CHECK(result->reply.inbound_expected == 5);
    CHECK(result->reply.inbound_transferred == 5);
    CHECK(result->reply.inbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(result->reply.informational.size() == 2);
    CHECK(progress ==
          (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
              {2, 5}, {4, 5}, {5, 5}}));
    CHECK(read_file(destination) == bytes(payload));
    const auto moved_destination = temporary.path() / "upload-moved.bin";
    std::filesystem::rename(destination, moved_destination);
    CHECK(read_file(moved_destination) == bytes(payload));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(session.state() == SessionState::Ready);
    CHECK(script->complete());
}

void zero_length_data_is_rejected_without_publishing() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "zero.bin";
    write_file(destination, "old");

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("fetch:metadata:0x00000000:0x00000000");
    script->respond("DATA00000000");

    FastbootSession session(std::move(transport));
    PrimitiveService primitives(session);
    FileReceiveService files(primitives);
    const auto result = files.fetch(
        "metadata",
        FetchRange{.offset = 0, .size = 0},
        destination,
        16);

    CHECK(!result);
    CHECK(result.error().code == FileReceiveErrorCode::Protocol);
    CHECK(result.error().primitive_code ==
          PrimitiveErrorCode::ProtocolViolation);
    CHECK(result.error().operation == PrimitiveOperation::Fetch);
    CHECK(result.error().phase == ProtocolPhase::InitialResponse);
    CHECK(result.error().inbound_expected == 0);
    CHECK(result.error().inbound_transferred == 0);
    CHECK(result.error().inbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(result.error().session_poisoned);
    CHECK(!result.error().terminal.has_value());
    CHECK(read_file(destination) == bytes("old"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(script->cancellation_requested());
    CHECK(script->closed());
    CHECK(script->complete());
}

void device_and_terminal_failures_preserve_the_previous_file() {
    TemporaryDirectory temporary;
    const auto rejected_destination = temporary.path() / "rejected.bin";
    write_file(rejected_destination, "old-rejected");

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("upload");
    script->respond("INFOnothing staged");
    script->respond("FAILno data");
    script->expect_write("fetch:vendor");
    script->respond("INFOreading");
    script->respond("DATA00000003");
    script->respond("new");
    script->respond("TEXTverification failed");
    script->respond("FAILbad digest");

    FastbootSession session(std::move(transport));
    PrimitiveService primitives(session);
    FileReceiveService files(primitives);
    const auto rejected = files.upload(rejected_destination, 8);
    CHECK(!rejected);
    CHECK(rejected.error().code == FileReceiveErrorCode::DeviceFail);
    CHECK(rejected.error().primitive_code == PrimitiveErrorCode::DeviceFail);
    CHECK(rejected.error().terminal.has_value());
    CHECK(rejected.error().terminal->kind == ResponseKind::Fail);
    CHECK(rejected.error().terminal->payload == "no data");
    CHECK(rejected.error().informational.size() == 1);
    CHECK(!rejected.error().session_poisoned);
    CHECK(read_file(rejected_destination) == bytes("old-rejected"));

    const auto terminal_destination = temporary.path() / "terminal.bin";
    write_file(terminal_destination, "old-terminal");
    const auto terminal = files.fetch(
        "vendor", {}, terminal_destination, 8);
    CHECK(!terminal);
    CHECK(terminal.error().code == FileReceiveErrorCode::DeviceFail);
    CHECK(terminal.error().phase == ProtocolPhase::FinalResponse);
    CHECK(terminal.error().terminal.has_value());
    CHECK(terminal.error().terminal->kind == ResponseKind::Fail);
    CHECK(terminal.error().terminal->payload == "bad digest");
    CHECK(terminal.error().informational.size() == 2);
    CHECK(terminal.error().inbound_expected == 3);
    CHECK(terminal.error().inbound_transferred == 3);
    CHECK(terminal.error().inbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(read_file(terminal_destination) == bytes("old-terminal"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(session.state() == SessionState::Ready);
    CHECK(script->complete());
}

void partial_timeout_and_cancellation_are_never_published() {
    TemporaryDirectory temporary;
    const auto timeout_destination = temporary.path() / "timeout.bin";
    write_file(timeout_destination, "old-timeout");

    auto timeout_transport = std::make_unique<ScriptedTransport>();
    auto* timeout_script = timeout_transport.get();
    timeout_script->expect_write("upload");
    timeout_script->respond("DATA00000004");
    timeout_script->respond("ab");
    timeout_script->respond(
        "c",
        TransportStatus::Timeout,
        TransferCertainty::PartialOrUnknown,
        false,
        1,
        60,
        "receive deadline");

    SessionOptions timeout_options;
    timeout_options.receive_chunk_bytes = 2;
    FastbootSession timeout_session(
        std::move(timeout_transport), timeout_options);
    PrimitiveService timeout_primitives(timeout_session);
    FileReceiveService timeout_files(timeout_primitives);
    const auto timeout = timeout_files.upload(timeout_destination, 4);
    CHECK(!timeout);
    CHECK(timeout.error().primitive_code == PrimitiveErrorCode::Timeout);
    CHECK(timeout.error().phase == ProtocolPhase::DataRead);
    CHECK(timeout.error().transport_status == TransportStatus::Timeout);
    CHECK(timeout.error().transport_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(timeout.error().inbound_expected == 4);
    CHECK(timeout.error().inbound_transferred == 2);
    CHECK(timeout.error().inbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(timeout.error().native_code == 60);
    CHECK(timeout.error().session_poisoned);
    CHECK(read_file(timeout_destination) == bytes("old-timeout"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(timeout_script->complete());

    const auto cancelled_destination = temporary.path() / "cancelled.bin";
    write_file(cancelled_destination, "old-cancelled");
    auto cancelled_transport = std::make_unique<ScriptedTransport>();
    auto* cancelled_script = cancelled_transport.get();
    cancelled_script->expect_write("upload");
    cancelled_script->respond("DATA00000004");
    cancelled_script->respond("ab");

    SessionOptions cancelled_options;
    cancelled_options.receive_chunk_bytes = 2;
    FastbootSession cancelled_session(
        std::move(cancelled_transport), cancelled_options);
    PrimitiveService cancelled_primitives(cancelled_session);
    FileReceiveService cancelled_files(cancelled_primitives);
    const auto cancelled = cancelled_files.upload(
        cancelled_destination,
        4,
        [](std::uint64_t, std::uint64_t) {
            return TransferProgressAction::cancel;
        });
    CHECK(!cancelled);
    CHECK(cancelled.error().primitive_code == PrimitiveErrorCode::Cancelled);
    CHECK(cancelled.error().inbound_expected == 4);
    CHECK(cancelled.error().inbound_transferred == 2);
    CHECK(cancelled.error().inbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(cancelled.error().session_poisoned);
    CHECK(read_file(cancelled_destination) == bytes("old-cancelled"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(cancelled_script->cancellation_requested());
    CHECK(cancelled_script->closed());
    CHECK(cancelled_script->complete());

    const auto pre_cancelled_destination =
        temporary.path() / "pre-cancelled.bin";
    write_file(pre_cancelled_destination, "old-pre-cancelled");
    auto pre_cancelled_transport = std::make_unique<ScriptedTransport>();
    auto* pre_cancelled_script = pre_cancelled_transport.get();
    FastbootSession pre_cancelled_session(
        std::move(pre_cancelled_transport));
    PrimitiveService pre_cancelled_primitives(pre_cancelled_session);
    FileReceiveService pre_cancelled_files(pre_cancelled_primitives);
    pre_cancelled_files.request_cancel();
    const auto pre_cancelled = pre_cancelled_files.upload(
        pre_cancelled_destination, 4);
    CHECK(!pre_cancelled);
    CHECK(pre_cancelled.error().primitive_code ==
          PrimitiveErrorCode::Cancelled);
    CHECK(pre_cancelled.error().phase == ProtocolPhase::Validation);
    CHECK(pre_cancelled.error().transport_status ==
          TransportStatus::Cancelled);
    CHECK(pre_cancelled.error().transport_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(pre_cancelled.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(!pre_cancelled.error().inbound_expected.has_value());
    CHECK(pre_cancelled.error().inbound_transferred == 0);
    CHECK(pre_cancelled.error().inbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(pre_cancelled.error().session_poisoned);
    CHECK(read_file(pre_cancelled_destination) == bytes("old-pre-cancelled"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(pre_cancelled_script->cancellation_requested());
    CHECK(pre_cancelled_script->complete());
}

void terminal_timeout_and_short_transfer_preserve_the_previous_file() {
    TemporaryDirectory temporary;
    const auto terminal_destination = temporary.path() / "terminal-timeout.bin";
    write_file(terminal_destination, "old-terminal");

    auto terminal_transport = std::make_unique<ScriptedTransport>();
    auto* terminal_script = terminal_transport.get();
    terminal_script->expect_write("upload");
    terminal_script->respond("DATA00000003");
    terminal_script->respond("new");
    terminal_script->respond(
        "",
        TransportStatus::Timeout,
        TransferCertainty::NotTransferred,
        false,
        0,
        110,
        "terminal deadline");

    FastbootSession terminal_session(std::move(terminal_transport));
    PrimitiveService terminal_primitives(terminal_session);
    FileReceiveService terminal_files(terminal_primitives);
    const auto terminal = terminal_files.upload(terminal_destination, 3);
    CHECK(!terminal);
    CHECK(terminal.error().primitive_code == PrimitiveErrorCode::Timeout);
    CHECK(terminal.error().phase == ProtocolPhase::FinalResponse);
    CHECK(terminal.error().inbound_expected == 3);
    CHECK(terminal.error().inbound_transferred == 3);
    CHECK(terminal.error().inbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(terminal.error().native_code == 110);
    CHECK(!terminal.error().terminal.has_value());
    CHECK(read_file(terminal_destination) == bytes("old-terminal"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(terminal_script->complete());

    const auto short_destination = temporary.path() / "short.bin";
    write_file(short_destination, "old-short");
    auto short_transport = std::make_unique<ScriptedTransport>();
    auto* short_script = short_transport.get();
    short_script->expect_write("upload");
    short_script->respond("DATA00000004");
    short_script->respond("ab");
    short_script->respond("");

    FastbootSession short_session(std::move(short_transport));
    PrimitiveService short_primitives(short_session);
    FileReceiveService short_files(short_primitives);
    const auto short_result = short_files.upload(short_destination, 4);
    CHECK(!short_result);
    CHECK(short_result.error().primitive_code ==
          PrimitiveErrorCode::ProtocolViolation);
    CHECK(short_result.error().phase == ProtocolPhase::DataRead);
    CHECK(short_result.error().inbound_expected == 4);
    CHECK(short_result.error().inbound_transferred == 2);
    CHECK(short_result.error().inbound_certainty ==
          TransferCertainty::PartialOrUnknown);
    CHECK(read_file(short_destination) == bytes("old-short"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(short_script->complete());
}

void observer_failure_is_scoped_and_never_published() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "observer.bin";
    write_file(destination, "old-observer");

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("upload");
    script->respond("DATA00000003");
    script->respond("new");

    FastbootSession session(std::move(transport));
    PrimitiveService primitives(session);
    FileReceiveService files(primitives);
    const auto result = files.upload(
        destination,
        3,
        [](std::uint64_t, std::uint64_t) -> TransferProgressAction {
            throw std::runtime_error("observer failed");
        });

    CHECK(!result);
    CHECK(result.error().code == FileReceiveErrorCode::Protocol);
    CHECK(result.error().primitive_code == PrimitiveErrorCode::TransportIo);
    CHECK(result.error().phase == ProtocolPhase::DataRead);
    CHECK(result.error().transport_status == TransportStatus::IoError);
    CHECK(result.error().transport_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(result.error().inbound_expected == 3);
    CHECK(result.error().inbound_transferred == 3);
    CHECK(result.error().inbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(result.error().session_poisoned);
    CHECK(read_file(destination) == bytes("old-observer"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(script->cancellation_requested());
    CHECK(script->closed());
    CHECK(script->complete());
}

void limits_invalid_parameters_and_unavailable_sessions_are_local() {
    TemporaryDirectory temporary;
    const auto oversized_destination = temporary.path() / "oversized.bin";
    write_file(oversized_destination, "old-oversized");

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write("upload");
    script->respond("DATA00000005");
    FastbootSession session(std::move(transport));
    PrimitiveService primitives(session);
    FileReceiveService files(primitives);
    const auto oversized = files.upload(oversized_destination, 4);
    CHECK(!oversized);
    CHECK(oversized.error().code == FileReceiveErrorCode::LimitExceeded);
    CHECK(oversized.error().primitive_code ==
          PrimitiveErrorCode::ProtocolViolation);
    CHECK(oversized.error().inbound_expected == 5);
    CHECK(oversized.error().session_poisoned);
    CHECK(read_file(oversized_destination) == bytes("old-oversized"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(script->complete());

    const auto poisoned_destination = temporary.path() / "poisoned.bin";
    write_file(poisoned_destination, "old-poisoned");
    const auto poisoned = files.upload(poisoned_destination, 4);
    CHECK(!poisoned);
    CHECK(poisoned.error().primitive_code == PrimitiveErrorCode::Poisoned);
    CHECK(poisoned.error().session_poisoned);
    CHECK(read_file(poisoned_destination) == bytes("old-poisoned"));
    CHECK(temporary_count(temporary.path()) == 0);

    auto closed_transport = std::make_unique<ScriptedTransport>();
    auto* closed_script = closed_transport.get();
    FastbootSession closed_session(std::move(closed_transport));
    closed_session.close();
    PrimitiveService closed_primitives(closed_session);
    FileReceiveService closed_files(closed_primitives);
    const auto closed_destination = temporary.path() / "closed.bin";
    write_file(closed_destination, "old-closed");
    const auto closed = closed_files.upload(closed_destination, 4);
    CHECK(!closed);
    CHECK(closed.error().primitive_code == PrimitiveErrorCode::Closed);
    CHECK(read_file(closed_destination) == bytes("old-closed"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(closed_script->complete());

    auto local_transport = std::make_unique<ScriptedTransport>();
    auto* local_script = local_transport.get();
    FastbootSession local_session(std::move(local_transport));
    PrimitiveService local_primitives(local_session);
    FileReceiveService local_files(local_primitives);
    const auto local_destination = temporary.path() / "local.bin";
    write_file(local_destination, "old-local");
    const auto zero_maximum = local_files.upload(local_destination, 0);
    CHECK(!zero_maximum);
    CHECK(zero_maximum.error().code == FileReceiveErrorCode::InvalidArgument);
    CHECK(zero_maximum.error().phase == ProtocolPhase::Validation);
    CHECK(zero_maximum.error().transport_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(zero_maximum.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(!zero_maximum.error().inbound_expected.has_value());
    CHECK(read_file(local_destination) == bytes("old-local"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(local_session.state() == SessionState::Ready);
    CHECK(local_script->complete());
    const auto unsupported_maximum = local_files.upload(
        local_destination, kMaximumFileReceiveBytes + 1U);
    CHECK(!unsupported_maximum);
    CHECK(unsupported_maximum.error().code ==
          FileReceiveErrorCode::LimitExceeded);
    CHECK(unsupported_maximum.error().primitive_code ==
          PrimitiveErrorCode::InvalidArgument);
    CHECK(unsupported_maximum.error().phase == ProtocolPhase::Validation);
    CHECK(unsupported_maximum.error().transport_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(unsupported_maximum.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(!unsupported_maximum.error().inbound_expected.has_value());
    CHECK(read_file(local_destination) == bytes("old-local"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(local_session.state() == SessionState::Ready);
    CHECK(local_script->complete());
    const auto unsupported_fetch_maximum = local_files.fetch(
        "vendor", {}, local_destination, kMaximumFileReceiveBytes + 1U);
    CHECK(!unsupported_fetch_maximum);
    CHECK(unsupported_fetch_maximum.error().code ==
          FileReceiveErrorCode::LimitExceeded);
    CHECK(unsupported_fetch_maximum.error().operation ==
          PrimitiveOperation::Fetch);
    CHECK(unsupported_fetch_maximum.error().phase ==
          ProtocolPhase::Validation);
    CHECK(unsupported_fetch_maximum.error().outbound_certainty ==
          TransferCertainty::NotTransferred);
    CHECK(read_file(local_destination) == bytes("old-local"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(local_session.state() == SessionState::Ready);
    CHECK(local_script->complete());
    const auto invalid_partition = local_files.fetch(
        "bad:partition", {}, local_destination, 4);
    CHECK(!invalid_partition);
    CHECK(invalid_partition.error().code ==
          FileReceiveErrorCode::InvalidArgument);
    CHECK(invalid_partition.error().primitive_code ==
          PrimitiveErrorCode::InvalidArgument);
    const auto missing_parent = local_files.upload(
        temporary.path() / "missing" / "output.bin", 4);
    CHECK(!missing_parent);
    CHECK(missing_parent.error().code ==
          FileReceiveErrorCode::DestinationCreate);
    CHECK(missing_parent.error().file_code ==
          FileTransferSinkErrorKind::ParentUnavailable);
    CHECK(read_file(local_destination) == bytes("old-local"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(local_session.state() == SessionState::Ready);
    CHECK(local_script->complete());
}

void fetch_uses_signed_64_bit_range_and_publish_failures_keep_metadata() {
    TemporaryDirectory temporary;
    const auto range_destination = temporary.path() / "range.bin";
    write_file(range_destination, "old-range");

    auto transport = std::make_unique<ScriptedTransport>();
    auto* script = transport.get();
    script->expect_write(
        "fetch:super:0x7fffffffffffffff:0x7fffffffffffffff");
    script->respond("FAILrange unsupported");

    FastbootSession session(std::move(transport));
    PrimitiveService primitives(session);
    FileReceiveService files(primitives);
    constexpr auto maximum_signed = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    const auto maximum_range = files.fetch(
        "super",
        FetchRange{.offset = maximum_signed, .size = maximum_signed},
        range_destination,
        kMaximumFileReceiveBytes);
    CHECK(!maximum_range);
    CHECK(maximum_range.error().code == FileReceiveErrorCode::DeviceFail);
    CHECK(accepted_text(*script) ==
          "fetch:super:0x7fffffffffffffff:0x7fffffffffffffff");
    CHECK(read_file(range_destination) == bytes("old-range"));

    const auto invalid_range = files.fetch(
        "super",
        FetchRange{.offset = maximum_signed + 1U, .size = 1},
        range_destination,
        kMaximumFileReceiveBytes);
    CHECK(!invalid_range);
    CHECK(invalid_range.error().code ==
          FileReceiveErrorCode::InvalidArgument);
    CHECK(read_file(range_destination) == bytes("old-range"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(script->complete());

    const auto destination_directory = temporary.path() / "publish-target";
    CHECK(std::filesystem::create_directory(destination_directory));
    write_file(destination_directory / "old-marker", "old");
    auto publish_transport = std::make_unique<ScriptedTransport>();
    auto* publish_script = publish_transport.get();
    publish_script->expect_write("fetch:vendor");
    publish_script->respond("DATA00000003");
    publish_script->respond("new");
    publish_script->respond("INFOreceived");
    publish_script->respond("OKAYcomplete");

    FastbootSession publish_session(std::move(publish_transport));
    PrimitiveService publish_primitives(publish_session);
    FileReceiveService publish_files(publish_primitives);
    const auto publish = publish_files.fetch(
        "vendor", {}, destination_directory, 3);
    CHECK(!publish);
    CHECK(publish.error().code ==
          FileReceiveErrorCode::DestinationPublish);
    CHECK(publish.error().file_code ==
          FileTransferSinkErrorKind::PublishFailed);
    CHECK(publish.error().phase == ProtocolPhase::FinalResponse);
    CHECK(publish.error().terminal.has_value());
    CHECK(publish.error().terminal->kind == ResponseKind::Okay);
    CHECK(publish.error().terminal->payload == "complete");
    CHECK(publish.error().informational.size() == 1);
    CHECK(publish.error().inbound_expected == 3);
    CHECK(publish.error().inbound_transferred == 3);
    CHECK(publish.error().inbound_certainty ==
          TransferCertainty::FullyTransferred);
    CHECK(std::filesystem::is_directory(destination_directory));
    CHECK(read_file(destination_directory / "old-marker") == bytes("old"));
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(publish_session.state() == SessionState::Ready);
    CHECK(publish_script->complete());
}

}  // namespace

int main() {
    struct Test final {
        std::string_view name;
        void (*function)();
    };
    const std::array tests{
        Test{"binary upload publishes after OKAY",
             binary_upload_is_published_only_after_terminal_okay},
        Test{"zero DATA is rejected transactionally",
             zero_length_data_is_rejected_without_publishing},
        Test{"device failures preserve old files",
             device_and_terminal_failures_preserve_the_previous_file},
        Test{"timeout and cancellation are transactional",
             partial_timeout_and_cancellation_are_never_published},
        Test{"terminal timeout and short transfer are transactional",
             terminal_timeout_and_short_transfer_preserve_the_previous_file},
        Test{"observer failures are scoped transactionally",
             observer_failure_is_scoped_and_never_published},
        Test{"limits and unavailable sessions are local",
             limits_invalid_parameters_and_unavailable_sessions_are_local},
        Test{"64-bit range and publish error metadata",
             fetch_uses_signed_64_bit_range_and_publish_failures_keep_metadata},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
