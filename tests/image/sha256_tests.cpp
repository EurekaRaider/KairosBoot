// SPDX-License-Identifier: MIT
#include "src/image/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::Sha256Digest;
using kairosboot::image::Sha256ErrorKind;
using kairosboot::image::compute_sha256;
using kairosboot::image::kSha256MaxInputSize;
using kairosboot::image::kSha256SourceReadSize;
using kairosboot::image::sha256_hex;

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

class ScriptedSource final : public IImageSource {
public:
    explicit ScriptedSource(
        std::vector<std::byte> bytes,
        const std::uint64_t claimed_size = std::numeric_limits<std::uint64_t>::max())
        : bytes_(std::move(bytes)),
          claimed_size_(claimed_size == std::numeric_limits<std::uint64_t>::max()
                            ? static_cast<std::uint64_t>(bytes_.size())
                            : claimed_size) {}

    void set_max_read(const std::size_t value) noexcept {
        max_read_ = value;
    }

    void fail_on_call(const std::size_t value) noexcept {
        failing_call_ = value;
    }

    void return_zero_on_call(const std::size_t value) noexcept {
        zero_call_ = value;
    }

    void return_oversized_count_on_call(const std::size_t value) noexcept {
        oversized_call_ = value;
    }

    void cancel_on_call(std::stop_source& cancellation, const std::size_t value) noexcept {
        cancellation_ = &cancellation;
        cancellation_call_ = value;
    }

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return claimed_size_;
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        const auto call = read_count_++;
        max_requested_ = std::max(max_requested_, destination.size());
        last_offset_ = offset;
        if (failing_call_ == call) {
            return std::unexpected(ImageSourceError{"scripted source failure"});
        }
        if (zero_call_ == call) {
            return std::size_t{0};
        }
        if (oversized_call_ == call) {
            return destination.size() + 1U;
        }
        if (offset >= bytes_.size() || destination.empty()) {
            return std::size_t{0};
        }

        const auto available = bytes_.size() - static_cast<std::size_t>(offset);
        const auto amount = std::min({available, destination.size(), max_read_});
        std::copy_n(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            amount,
            destination.begin());
        if (cancellation_ != nullptr && cancellation_call_ == call) {
            cancellation_->request_stop();
        }
        return amount;
    }

    [[nodiscard]] std::size_t read_count() const noexcept {
        return read_count_;
    }

    [[nodiscard]] std::size_t max_requested() const noexcept {
        return max_requested_;
    }

    [[nodiscard]] std::uint64_t last_offset() const noexcept {
        return last_offset_;
    }

private:
    std::vector<std::byte> bytes_;
    std::uint64_t claimed_size_{};
    std::size_t max_read_{std::numeric_limits<std::size_t>::max()};
    std::optional<std::size_t> failing_call_;
    std::optional<std::size_t> zero_call_;
    std::optional<std::size_t> oversized_call_;
    std::stop_source* cancellation_{};
    std::optional<std::size_t> cancellation_call_;
    mutable std::size_t read_count_{};
    mutable std::size_t max_requested_{};
    mutable std::uint64_t last_offset_{};
};

[[nodiscard]] std::vector<std::byte> bytes_from_string(const std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const auto character : text) {
        bytes.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(character)));
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> repeated_a(const std::size_t size) {
    return std::vector<std::byte>(size, static_cast<std::byte>('a'));
}

void expect_digest(ScriptedSource& source, const std::string_view expected) {
    const auto digest = compute_sha256(source);
    CHECK(digest);
    CHECK(sha256_hex(*digest) == expected);
}

void standard_known_answer_vectors_match() {
    struct Vector final {
        std::size_t length;
        std::string_view digest;
    };
    constexpr std::array vectors{
        Vector{0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        Vector{55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        Vector{56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        Vector{63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        Vector{64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        Vector{65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    };
    for (const auto& vector : vectors) {
        ScriptedSource source(repeated_a(vector.length));
        expect_digest(source, vector.digest);
    }

    ScriptedSource abc(bytes_from_string("abc"));
    expect_digest(
        abc,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void million_a_vector_uses_bounded_short_reads() {
    ScriptedSource source(repeated_a(1'000'000U));
    source.set_max_read(97U);
    expect_digest(
        source,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(source.read_count() > 10'000U);
    CHECK(source.max_requested() <= kSha256SourceReadSize);
}

void zero_progress_is_reported_at_the_exact_cursor() {
    ScriptedSource source(bytes_from_string("abcdef"));
    source.set_max_read(2U);
    source.return_zero_on_call(1U);
    const auto digest = compute_sha256(source);
    CHECK(!digest);
    CHECK(digest.error().kind == Sha256ErrorKind::Truncated);
    CHECK(digest.error().input_offset == 2U);
    CHECK(source.read_count() == 2U);
    CHECK(source.last_offset() == 2U);
}

void source_errors_are_preserved_at_the_exact_cursor() {
    ScriptedSource source(bytes_from_string("abcdef"));
    source.set_max_read(2U);
    source.fail_on_call(1U);
    const auto digest = compute_sha256(source);
    CHECK(!digest);
    CHECK(digest.error().kind == Sha256ErrorKind::Source);
    CHECK(digest.error().input_offset == 2U);
    CHECK(digest.error().message.find("scripted source failure") != std::string::npos);
}

void invalid_source_counts_are_rejected_before_offset_addition() {
    ScriptedSource source(bytes_from_string("abcdef"));
    source.return_oversized_count_on_call(0U);
    const auto digest = compute_sha256(source);
    CHECK(!digest);
    CHECK(digest.error().kind == Sha256ErrorKind::Source);
    CHECK(digest.error().input_offset == 0U);
    CHECK(source.read_count() == 1U);
}

void pre_cancelled_hashes_do_not_read() {
    ScriptedSource source(repeated_a(1024U));
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto digest = compute_sha256(source, cancellation.get_token());
    CHECK(!digest);
    CHECK(digest.error().kind == Sha256ErrorKind::Cancelled);
    CHECK(digest.error().input_offset == 0U);
    CHECK(source.read_count() == 0U);
}

void large_hashes_cancel_between_bounded_windows() {
    ScriptedSource source(repeated_a(kSha256SourceReadSize * 16U));
    std::stop_source cancellation;
    source.cancel_on_call(cancellation, 2U);
    const auto digest = compute_sha256(source, cancellation.get_token());
    CHECK(!digest);
    CHECK(digest.error().kind == Sha256ErrorKind::Cancelled);
    CHECK(digest.error().input_offset == kSha256SourceReadSize * 2U);
    CHECK(source.read_count() == 3U);
    CHECK(source.max_requested() == kSha256SourceReadSize);
}

void unrepresentable_bit_lengths_are_rejected_without_reads() {
    ScriptedSource source({}, kSha256MaxInputSize + 1U);
    const auto digest = compute_sha256(source);
    CHECK(!digest);
    CHECK(digest.error().kind == Sha256ErrorKind::InvalidSize);
    CHECK(digest.error().input_offset == 0U);
    CHECK(source.read_count() == 0U);
}

void maximum_representable_size_keeps_reads_bounded() {
    ScriptedSource source({}, kSha256MaxInputSize);
    const auto digest = compute_sha256(source);
    CHECK(!digest);
    CHECK(digest.error().kind == Sha256ErrorKind::Truncated);
    CHECK(digest.error().input_offset == 0U);
    CHECK(source.read_count() == 1U);
    CHECK(source.max_requested() == kSha256SourceReadSize);
}

void hexadecimal_output_is_fixed_width_and_lowercase() {
    Sha256Digest digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        digest[index] = static_cast<std::byte>(index * 8U + 7U);
    }
    const auto hex = sha256_hex(digest);
    CHECK(hex.size() == 64U);
    CHECK(hex == "070f171f272f373f474f575f676f777f878f979fa7afb7bfc7cfd7dfe7eff7ff");
    CHECK(hex.find_first_of("ABCDEF") == std::string::npos);
}

using Test = std::pair<std::string_view, void (*)()>;

}  // namespace

int main() {
    const std::array tests{
        Test{"standard known-answer vectors", standard_known_answer_vectors_match},
        Test{"million-a bounded short reads", million_a_vector_uses_bounded_short_reads},
        Test{"zero progress", zero_progress_is_reported_at_the_exact_cursor},
        Test{"source error", source_errors_are_preserved_at_the_exact_cursor},
        Test{"invalid source count", invalid_source_counts_are_rejected_before_offset_addition},
        Test{"pre-cancelled", pre_cancelled_hashes_do_not_read},
        Test{"large-input cancellation", large_hashes_cancel_between_bounded_windows},
        Test{"unrepresentable bit length", unrepresentable_bit_lengths_are_rejected_without_reads},
        Test{"maximum input size", maximum_representable_size_keeps_reads_bounded},
        Test{"lowercase hexadecimal", hexadecimal_output_is_fixed_width_and_lowercase},
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
    if (failures != 0U) {
        std::cerr << failures << " SHA-256 test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " SHA-256 tests passed\n";
    return 0;
}
