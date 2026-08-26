// SPDX-License-Identifier: MIT
#include "src/protocol/file_transfer_sink.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using kairosboot::protocol::FileTransferSink;
using kairosboot::protocol::FileTransferSinkErrorKind;
using kairosboot::protocol::TransferCertainty;
using kairosboot::protocol::TransportStatus;

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

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
            ("kairosboot-file-sink-" + std::to_string(stamp) + "-" +
             std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directory(path_);
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

[[nodiscard]] std::filesystem::path with_embedded_nul(
    const std::filesystem::path& prefix) {
    auto native = prefix.native();
    native.push_back(std::filesystem::path::value_type{});
    native.push_back(static_cast<std::filesystem::path::value_type>('x'));
    return std::filesystem::path(std::move(native));
}

void require_seal(
    FileTransferSink& sink,
    const std::uint64_t expected_size,
    const int source_line) {
    const auto sealed = sink.seal(expected_size);
    if (sealed) {
        return;
    }
    throw CheckFailure(
        "seal failed: kind=" +
        std::to_string(static_cast<unsigned int>(sealed.error().kind)) +
        ", native_code=" + std::to_string(sealed.error().native_code) +
        ", message=" + sealed.error().message + " at line " +
        std::to_string(source_line));
}

#define REQUIRE_SEAL(sink, expected_size) \
    require_seal((sink), (expected_size), __LINE__)

void exact_chunks_replace_only_after_seal() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "upload.bin";
    write_file(destination, "previous-complete-output");

    auto sink = FileTransferSink::create(destination);
    CHECK(sink);
    const auto first = bytes("abc");
    const auto second = bytes("defg");
    const auto first_result = (*sink)->write(0, first);
    CHECK(first_result.status == TransportStatus::Ok);
    CHECK(first_result.transferred == first.size());
    const auto second_result = (*sink)->write(first.size(), second);
    CHECK(second_result.status == TransportStatus::Ok);
    CHECK(second_result.transferred == second.size());
    CHECK((*sink)->bytes_written() == 7);
    CHECK(read_file(destination) == bytes("previous-complete-output"));
    CHECK(temporary_count(temporary.path()) == 1);

    REQUIRE_SEAL(**sink, 7);
    CHECK((*sink)->is_sealed());
    REQUIRE_SEAL(**sink, 7);
    CHECK(read_file(destination) == bytes("abcdefg"));
    CHECK(temporary_count(temporary.path()) == 0);

    const auto after_seal = (*sink)->write(7, bytes("x"));
    CHECK(after_seal.status == TransportStatus::IoError);
    CHECK(after_seal.certainty == TransferCertainty::NotTransferred);
}

void incomplete_and_discarded_transfers_preserve_destination() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "fetch.bin";
    write_file(destination, "old");

    {
        auto sink = FileTransferSink::create(destination);
        CHECK(sink);
        CHECK((*sink)->write(0, bytes("partial")).status ==
              TransportStatus::Ok);
        const auto sealed = (*sink)->seal(8);
        CHECK(!sealed);
        CHECK(sealed.error().kind == FileTransferSinkErrorKind::Incomplete);
        CHECK(temporary_count(temporary.path()) == 0);
        CHECK(read_file(destination) == bytes("old"));
    }

    {
        auto sink = FileTransferSink::create(destination);
        CHECK(sink);
        CHECK((*sink)->write(0, bytes("cancelled")).status ==
              TransportStatus::Ok);
        (*sink)->discard();
        (*sink)->discard();
        CHECK(temporary_count(temporary.path()) == 0);
        CHECK(read_file(destination) == bytes("old"));
    }

    {
        auto sink = FileTransferSink::create(destination);
        CHECK(sink);
        CHECK((*sink)->write(0, bytes("ab")).status == TransportStatus::Ok);
    }
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(read_file(destination) == bytes("old"));
}

void offset_contract_and_large_boundaries_fail_closed() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "offset.bin";
    write_file(destination, "stable");

    auto sink = FileTransferSink::create(destination);
    CHECK(sink);
    CHECK((*sink)->write(0, bytes("abc")).status == TransportStatus::Ok);
    const auto nonsequential = (*sink)->write(2, bytes("x"));
    CHECK(nonsequential.status == TransportStatus::IoError);
    CHECK(nonsequential.transferred == 0);
    CHECK(nonsequential.certainty == TransferCertainty::NotTransferred);
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(read_file(destination) == bytes("stable"));

    auto overflow = FileTransferSink::create(destination);
    CHECK(overflow);
    const auto huge = (*overflow)->write(
        std::numeric_limits<std::uint64_t>::max(), bytes("x"));
    CHECK(huge.status == TransportStatus::IoError);
    CHECK(huge.transferred == 0);
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(read_file(destination) == bytes("stable"));

    auto sparse_boundary = FileTransferSink::create(destination);
    CHECK(sparse_boundary);
    const auto incomplete = (*sparse_boundary)->seal(
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max()) + 1U);
    CHECK(!incomplete);
    CHECK(incomplete.error().kind == FileTransferSinkErrorKind::Incomplete);
    CHECK(temporary_count(temporary.path()) == 0);
    CHECK(read_file(destination) == bytes("stable"));
}

void publish_failure_cleans_temporary_and_preserves_new_entry() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "publish.bin";
    write_file(destination, "stable");

    auto sink = FileTransferSink::create(destination);
    CHECK(sink);
    CHECK((*sink)->write(0, bytes("new")).status == TransportStatus::Ok);
    std::filesystem::remove(destination);
    std::filesystem::create_directory(destination);

    const auto sealed = (*sink)->seal(3);
    CHECK(!sealed);
    CHECK(sealed.error().kind == FileTransferSinkErrorKind::PublishFailed);
    CHECK(std::filesystem::is_directory(destination));
    CHECK(temporary_count(temporary.path()) == 0);
}

void large_receive_uses_reusable_bounded_chunks() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "large.bin";
    auto sink = FileTransferSink::create(destination);
    CHECK(sink);

    constexpr std::size_t chunk_size = 64U * 1024U;
    constexpr std::uint64_t total_size = 32U * 1024U * 1024U + 317U;
    std::array<std::byte, chunk_size> chunk{};
    std::uint64_t offset = 0;
    while (offset < total_size) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), total_size - offset));
        for (std::size_t index = 0; index < count; ++index) {
            chunk[index] = static_cast<std::byte>(
                (offset + index) * 131U + 17U);
        }
        const auto result = (*sink)->write(offset, std::span(chunk).first(count));
        CHECK(result.status == TransportStatus::Ok);
        CHECK(result.transferred == count);
        offset += count;
    }
    REQUIRE_SEAL(**sink, total_size);
    CHECK(std::filesystem::file_size(destination) == total_size);

    std::ifstream input(destination, std::ios::binary);
    CHECK(input.good());
    const std::array<std::uint64_t, 3> probes{
        0U, total_size / 2U, total_size - 1U};
    for (const auto probe : probes) {
        input.seekg(static_cast<std::streamoff>(probe));
        char value = 0;
        input.read(&value, 1);
        CHECK(input.good());
        CHECK(static_cast<unsigned char>(value) ==
              static_cast<unsigned char>(probe * 131U + 17U));
    }
    CHECK(temporary_count(temporary.path()) == 0);
}

void concurrent_publish_never_exposes_a_mixed_file() {
    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "concurrent.bin";
    write_file(destination, "old");

    constexpr std::size_t writers = 8;
    constexpr std::size_t payload_size = 1024U * 1024U;
    std::vector<std::shared_ptr<FileTransferSink>> sinks;
    std::vector<std::vector<std::byte>> payloads;
    sinks.reserve(writers);
    payloads.reserve(writers);
    for (std::size_t writer = 0; writer < writers; ++writer) {
        auto sink = FileTransferSink::create(destination);
        CHECK(sink);
        sinks.push_back(*sink);
        payloads.emplace_back(
            payload_size,
            static_cast<std::byte>(static_cast<unsigned char>(writer + 1U)));
    }

    std::barrier ready(static_cast<std::ptrdiff_t>(writers));
    std::atomic<bool> valid{true};
    std::vector<std::thread> threads;
    threads.reserve(writers);
    for (std::size_t writer = 0; writer < writers; ++writer) {
        threads.emplace_back([&, writer] {
            constexpr std::size_t chunk_size = 32U * 1024U;
            std::size_t offset = 0;
            while (offset < payloads[writer].size()) {
                const auto count = std::min(
                    chunk_size, payloads[writer].size() - offset);
                const auto result = sinks[writer]->write(
                    offset,
                    std::span(payloads[writer]).subspan(offset, count));
                if (result.status != TransportStatus::Ok ||
                    result.transferred != count) {
                    valid.store(false, std::memory_order_relaxed);
                    ready.arrive_and_drop();
                    return;
                }
                offset += count;
            }
            ready.arrive_and_wait();
            if (!sinks[writer]->seal(payload_size)) {
                valid.store(false, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(valid.load(std::memory_order_relaxed));

    const auto published = read_file(destination);
    CHECK(published.size() == payload_size);
    CHECK(std::ranges::any_of(payloads, [&](const auto& payload) {
        return payload == published;
    }));
    CHECK(temporary_count(temporary.path()) == 0);
}

void invalid_paths_are_rejected_without_side_effects() {
    TemporaryDirectory temporary;
    const auto empty = FileTransferSink::create({});
    CHECK(!empty);
    CHECK(empty.error().kind == FileTransferSinkErrorKind::InvalidArgument);

    const auto nul = FileTransferSink::create(
        with_embedded_nul(temporary.path() / "nul.bin"));
    CHECK(!nul);
    CHECK(nul.error().kind == FileTransferSinkErrorKind::UnsafePath);

    const auto missing_parent = FileTransferSink::create(
        temporary.path() / "missing" / "output.bin");
    CHECK(!missing_parent);
    CHECK(missing_parent.error().kind ==
          FileTransferSinkErrorKind::ParentUnavailable);
    CHECK(temporary_count(temporary.path()) == 0);
}

}  // namespace

int main() {
    struct Test final {
        std::string_view name;
        void (*function)();
    };
    const std::array tests{
        Test{"exact chunks replace only after seal",
             exact_chunks_replace_only_after_seal},
        Test{"incomplete and discarded transfers preserve destination",
             incomplete_and_discarded_transfers_preserve_destination},
        Test{"offset contract and large boundaries fail closed",
             offset_contract_and_large_boundaries_fail_closed},
        Test{"publish failure cleans temporary",
             publish_failure_cleans_temporary_and_preserves_new_entry},
        Test{"large receive uses bounded chunks",
             large_receive_uses_reusable_bounded_chunks},
        Test{"concurrent publish is complete",
             concurrent_publish_never_exposes_a_mixed_file},
        Test{"invalid paths have no side effects",
             invalid_paths_are_rejected_without_side_effects},
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
