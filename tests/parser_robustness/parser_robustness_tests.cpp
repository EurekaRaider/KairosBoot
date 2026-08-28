// SPDX-License-Identifier: MIT
#include "src/image/sparse_image.hpp"
#include "src/protocol/fastboot_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::SparseErrorKind;
using kairosboot::image::SparseImage;
using kairosboot::image::kMaxSparseChunks;
using kairosboot::protocol::ResponseKind;
using kairosboot::protocol::ResponseParseErrorCode;
using kairosboot::protocol::parse_response;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw CheckFailure(std::string("check failed: ") + #condition +  \
                               " at line " + std::to_string(__LINE__));       \
        }                                                                      \
    } while (false)

class MemorySource final : public IImageSource {
public:
    explicit MemorySource(std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset >= bytes_.size() || destination.empty()) {
            return 0U;
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

[[nodiscard]] bool ascii_space(const char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
}

[[nodiscard]] std::optional<std::uint8_t> hex_nibble(
    const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    if (character >= 'A' && character <= 'F') {
        return static_cast<std::uint8_t>(character - 'A' + 10);
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::byte> load_hex(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw CheckFailure("unable to open corpus seed: " + path.string());
    }
    const std::string text{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::vector<std::byte> result;
    std::optional<std::uint8_t> high;
    for (const char character : text) {
        if (ascii_space(character)) {
            continue;
        }
        const auto nibble = hex_nibble(character);
        if (!nibble) {
            throw CheckFailure("non-hex character in corpus seed: " +
                               path.string());
        }
        if (!high) {
            high = nibble;
            continue;
        }
        result.push_back(static_cast<std::byte>((*high << 4U) | *nibble));
        high.reset();
    }
    if (high) {
        throw CheckFailure("odd hex digit count in corpus seed: " + path.string());
    }
    return result;
}

[[nodiscard]] bool has_prefix(const std::span<const std::byte> packet,
                              const std::string_view prefix) noexcept {
    return packet.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), packet.begin(),
                      [](const char left, const std::byte right) {
                          return static_cast<unsigned char>(left) ==
                                 std::to_integer<unsigned char>(right);
                      });
}

[[nodiscard]] std::string payload_string(
    const std::span<const std::byte> packet) {
    std::string result;
    result.reserve(packet.size());
    for (const auto byte : packet) {
        result.push_back(
            static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return result;
}

struct ExpectedResponse final {
    ResponseKind kind;
    std::string payload;
    std::optional<std::uint32_t> data_size;
};

using ResponseOracle =
    std::variant<ExpectedResponse, ResponseParseErrorCode>;

[[nodiscard]] ResponseOracle response_oracle(
    const std::span<const std::byte> packet,
    const std::size_t maximum_bytes) {
    if (packet.empty()) {
        return ResponseParseErrorCode::ZeroLength;
    }
    if (packet.size() < 4U) {
        return ResponseParseErrorCode::TooShort;
    }
    if (packet.size() > maximum_bytes) {
        return ResponseParseErrorCode::TooLong;
    }

    const auto payload = packet.subspan(4U);
    for (const auto [prefix, kind] :
         std::array{std::pair{"INFO", ResponseKind::Info},
                    std::pair{"TEXT", ResponseKind::Text},
                    std::pair{"OKAY", ResponseKind::Okay},
                    std::pair{"FAIL", ResponseKind::Fail}}) {
        if (has_prefix(packet, prefix)) {
            return ExpectedResponse{
                kind, payload_string(payload), std::nullopt};
        }
    }
    if (!has_prefix(packet, "DATA")) {
        return ResponseParseErrorCode::UnknownPrefix;
    }
    if (payload.size() != 8U) {
        return ResponseParseErrorCode::InvalidDataLength;
    }

    std::uint32_t value = 0U;
    for (const auto byte : payload) {
        const auto nibble = hex_nibble(
            static_cast<char>(std::to_integer<unsigned char>(byte)));
        if (!nibble) {
            return ResponseParseErrorCode::InvalidDataHex;
        }
        value = static_cast<std::uint32_t>((value << 4U) | *nibble);
    }
    return ExpectedResponse{ResponseKind::Data, payload_string(payload), value};
}

void check_response_properties(const std::span<const std::byte> packet,
                               const std::size_t maximum_bytes) {
    const auto expected = response_oracle(packet, maximum_bytes);
    const auto actual = parse_response(packet, maximum_bytes);
    if (const auto* error = std::get_if<ResponseParseErrorCode>(&expected)) {
        CHECK(!actual);
        CHECK(actual.error().code == *error);
        return;
    }

    CHECK(actual);
    const auto& response = std::get<ExpectedResponse>(expected);
    CHECK(actual->kind == response.kind);
    CHECK(actual->payload == response.payload);
    CHECK(actual->data_size == response.data_size);
    CHECK(packet.size() <= maximum_bytes);
}

struct ResponseSeed final {
    std::size_t maximum_bytes;
    std::vector<std::byte> packet;
};

[[nodiscard]] ResponseSeed load_response_seed(
    const std::filesystem::path& path) {
    auto bytes = load_hex(path);
    if (bytes.size() < 2U) {
        throw CheckFailure("response corpus seed lacks its two-byte limit: " +
                           path.string());
    }
    const auto maximum_bytes =
        static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[0])) |
        (static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[1]))
         << 8U);
    bytes.erase(bytes.begin(), bytes.begin() + 2);
    return ResponseSeed{maximum_bytes, std::move(bytes)};
}

void check_sparse_properties(std::vector<std::byte> bytes) {
    auto source = std::make_shared<MemorySource>(std::move(bytes));
    const auto parsed = SparseImage::open(source);
    if (!parsed) {
        return;
    }

    CHECK(parsed->header().total_chunks <= kMaxSparseChunks);
    CHECK(parsed->chunks().size() == parsed->header().total_chunks);
    const auto declared_size =
        static_cast<std::uint64_t>(parsed->header().total_blocks) *
        parsed->header().block_size;
    CHECK(parsed->output_size() == declared_size);

    std::array<std::byte, 64> output{};
    const auto amount = static_cast<std::size_t>(
        std::min<std::uint64_t>(parsed->output_size(), output.size()));
    const auto first = parsed->read_at(0U, std::span(output).first(amount));
    CHECK(first);
    CHECK(*first == amount);
    if (amount != 0U) {
        const auto tail_offset = parsed->output_size() - amount;
        const auto tail = parsed->read_at(
            tail_offset, std::span(output).first(amount));
        CHECK(tail);
        CHECK(*tail == amount);
    }
    if (parsed->output_size() < std::numeric_limits<std::uint64_t>::max()) {
        const auto beyond = parsed->read_at(
            parsed->output_size() + 1U, std::span(output).first(1U));
        CHECK(!beyond);
        CHECK(beyond.error().kind == SparseErrorKind::InvalidArgument);
    }
}

class DeterministicRandom final {
public:
    [[nodiscard]] std::uint64_t next() noexcept {
        state_ ^= state_ >> 12U;
        state_ ^= state_ << 25U;
        state_ ^= state_ >> 27U;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    [[nodiscard]] std::size_t bounded(const std::size_t bound) noexcept {
        return bound == 0U ? 0U : static_cast<std::size_t>(next() % bound);
    }

    [[nodiscard]] std::byte byte() noexcept {
        return static_cast<std::byte>(next() & 0xFFU);
    }

private:
    std::uint64_t state_{0x4B6169726F73426FULL};
};

[[nodiscard]] std::vector<std::byte> random_bytes(
    DeterministicRandom& random,
    const std::size_t size) {
    std::vector<std::byte> result(size);
    std::ranges::generate(result, [&] { return random.byte(); });
    return result;
}

void replay_response_corpus(const std::filesystem::path& source_dir) {
    const auto directory =
        source_dir / "tests/parser_robustness/corpus/response";
    constexpr std::array files{
        "empty.hex",
        "truncated-prefix.hex",
        "unknown-prefix.hex",
        "invalid-data-length.hex",
        "invalid-data-hex.hex",
        "valid-data-maximum.hex",
        "configured-limit.hex",
        "valid-info-binary.hex",
    };
    for (const auto file : files) {
        const auto seed = load_response_seed(directory / file);
        check_response_properties(seed.packet, seed.maximum_bytes);
    }
}

[[nodiscard]] std::vector<std::vector<std::byte>> load_sparse_corpus(
    const std::filesystem::path& source_dir) {
    const auto directory = source_dir / "tests/sparse/corpus";
    constexpr std::array files{
        "truncated-header.hex",
        "unknown-chunk.hex",
        "valid-empty.hex",
        "valid-mixed.hex",
        "metadata-chunk-limit.hex",
        "aggregate-output-overflow.hex",
    };
    std::vector<std::vector<std::byte>> result;
    result.reserve(files.size());
    for (const auto file : files) {
        result.push_back(load_hex(directory / file));
    }
    return result;
}

void replay_sparse_corpus(const std::filesystem::path& source_dir) {
    const auto corpus = load_sparse_corpus(source_dir);
    constexpr std::array expected{
        std::optional{SparseErrorKind::Truncated},
        std::optional{SparseErrorKind::Unsupported},
        std::optional<SparseErrorKind>{},
        std::optional<SparseErrorKind>{},
        std::optional{SparseErrorKind::Unsupported},
        std::optional{SparseErrorKind::Malformed},
    };
    CHECK(corpus.size() == expected.size());
    for (std::size_t index = 0U; index < corpus.size(); ++index) {
        auto source = std::make_shared<MemorySource>(corpus[index]);
        const auto parsed = SparseImage::open(source);
        if (expected[index]) {
            CHECK(!parsed);
            CHECK(parsed.error().kind == *expected[index]);
        } else {
            CHECK(parsed);
        }
        check_sparse_properties(corpus[index]);
    }
}

void randomized_response_properties(const std::size_t cases,
                                    DeterministicRandom& random) {
    constexpr std::array<std::string_view, 5> prefixes{
        "INFO", "TEXT", "OKAY", "FAIL", "DATA"};
    for (std::size_t index = 0U; index < cases; ++index) {
        auto packet = random_bytes(random, random.bounded(513U));
        if (packet.size() >= 4U && index % 3U == 0U) {
            const auto prefix = prefixes[index % prefixes.size()];
            for (std::size_t byte = 0U; byte < prefix.size(); ++byte) {
                packet[byte] = static_cast<std::byte>(prefix[byte]);
            }
        }
        const auto maximum_bytes = random.bounded(513U);
        check_response_properties(packet, maximum_bytes);
    }
}

void randomized_sparse_properties(
    const std::size_t cases,
    DeterministicRandom& random,
    const std::vector<std::vector<std::byte>>& corpus) {
    CHECK(!corpus.empty());
    for (std::size_t index = 0U; index < cases; ++index) {
        std::vector<std::byte> input;
        const auto mode = index % 4U;
        if (mode == 0U) {
            input = random_bytes(random, random.bounded(1025U));
        } else {
            input = corpus[random.bounded(corpus.size())];
            if (mode == 1U) {
                input.resize(random.bounded(input.size() + 1U));
            } else if (mode == 2U && !input.empty()) {
                const auto changes = 1U + random.bounded(8U);
                for (std::size_t change = 0U; change < changes; ++change) {
                    input[random.bounded(input.size())] = random.byte();
                }
            } else if (mode == 3U) {
                const auto suffix = random_bytes(random, random.bounded(65U));
                input.insert(input.end(), suffix.begin(), suffix.end());
            }
        }
        CHECK(input.size() <= 4096U);
        check_sparse_properties(std::move(input));
    }
}

struct Arguments final {
    std::filesystem::path source_dir;
    std::size_t random_cases{};
};

[[nodiscard]] std::size_t parse_count(const std::string_view value) {
    std::size_t result = 0U;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() ||
        result > 100'000U) {
        throw CheckFailure("invalid --random-cases value");
    }
    return result;
}

[[nodiscard]] Arguments parse_arguments(const int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--source-dir" && index + 1 < argc) {
            result.source_dir = argv[++index];
        } else if (argument == "--random-cases" && index + 1 < argc) {
            result.random_cases = parse_count(argv[++index]);
        } else if (argument != "--replay-only") {
            throw CheckFailure("unknown or incomplete test argument");
        }
    }
    if (result.source_dir.empty()) {
        throw CheckFailure("--source-dir is required");
    }
    return result;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        const auto arguments = parse_arguments(argc, argv);
        replay_response_corpus(arguments.source_dir);
        replay_sparse_corpus(arguments.source_dir);
        if (arguments.random_cases != 0U) {
            DeterministicRandom random;
            randomized_response_properties(arguments.random_cases, random);
            const auto sparse_corpus = load_sparse_corpus(arguments.source_dir);
            randomized_sparse_properties(
                arguments.random_cases, random, sparse_corpus);
        }
        std::cout << "parser robustness checks passed";
        if (arguments.random_cases != 0U) {
            std::cout << ": " << arguments.random_cases
                      << " response and " << arguments.random_cases
                      << " sparse randomized cases";
        }
        std::cout << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "parser robustness failure: " << error.what() << '\n';
        return 1;
    }
}
