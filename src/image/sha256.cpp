// SPDX-License-Identifier: MIT
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace kairosboot::image {
namespace {

inline constexpr std::size_t kSha256BlockSize = 64U;

inline constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] constexpr std::uint32_t load_big_endian_u32(
    const std::byte* bytes) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[3]);
}

class Sha256State final {
public:
    void update(const std::span<const std::byte> bytes) noexcept {
        total_bytes_ += static_cast<std::uint64_t>(bytes.size());
        std::size_t cursor = 0;

        if (buffered_ != 0U) {
            const auto amount = std::min(kSha256BlockSize - buffered_, bytes.size());
            std::copy_n(bytes.begin(), amount, buffer_.begin() + buffered_);
            buffered_ += amount;
            cursor += amount;
            if (buffered_ == kSha256BlockSize) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }

        while (bytes.size() - cursor >= kSha256BlockSize) {
            transform(bytes.data() + cursor);
            cursor += kSha256BlockSize;
        }

        const auto remaining = bytes.size() - cursor;
        if (remaining != 0U) {
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                        remaining,
                        buffer_.begin());
            buffered_ = remaining;
        }
    }

    [[nodiscard]] Sha256Digest finish() noexcept {
        const auto bit_length = total_bytes_ * 8U;
        buffer_[buffered_++] = std::byte{0x80U};

        if (buffered_ > 56U) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                      buffer_.end(),
                      std::byte{0});
            transform(buffer_.data());
            buffered_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                  buffer_.begin() + 56,
                  std::byte{0});
        for (std::size_t index = 0; index < 8U; ++index) {
            buffer_[63U - index] = static_cast<std::byte>(
                (bit_length >> static_cast<unsigned int>(index * 8U)) & 0xffU);
        }
        transform(buffer_.data());

        Sha256Digest digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (std::size_t byte = 0; byte < 4U; ++byte) {
                digest[word * 4U + byte] = static_cast<std::byte>(
                    (state_[word] >>
                     static_cast<unsigned int>((3U - byte) * 8U)) &
                    0xffU);
            }
        }
        return digest;
    }

private:
    void transform(const std::byte* block) noexcept {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16U; ++index) {
            schedule[index] = load_big_endian_u32(block + index * 4U);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            const auto sigma0 = std::rotr(schedule[index - 15U], 7) ^
                                std::rotr(schedule[index - 15U], 18) ^
                                (schedule[index - 15U] >> 3U);
            const auto sigma1 = std::rotr(schedule[index - 2U], 17) ^
                                std::rotr(schedule[index - 2U], 19) ^
                                (schedule[index - 2U] >> 10U);
            schedule[index] = schedule[index - 16U] + sigma0 +
                              schedule[index - 7U] + sigma1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];

        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choice = (e & f) ^ ((~e) & g);
            const auto temporary1 =
                h + sum1 + choice + kRoundConstants[index] + schedule[index];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };
    std::array<std::byte, kSha256BlockSize> buffer_{};
    std::uint64_t total_bytes_{};
    std::size_t buffered_{};
};

[[nodiscard]] Sha256Error make_error(
    const Sha256ErrorKind kind,
    const std::uint64_t offset,
    std::string message) {
    return Sha256Error{kind, offset, std::move(message)};
}

}  // namespace

struct Sha256Accumulator::Implementation final {
    Sha256State state;
};

Sha256Accumulator::Sha256Accumulator()
    : implementation_(std::make_unique<Implementation>()) {}

Sha256Accumulator::~Sha256Accumulator() = default;

void Sha256Accumulator::update(
    const std::span<const std::byte> bytes) noexcept {
    implementation_->state.update(bytes);
}

Sha256Digest Sha256Accumulator::finish() noexcept {
    return implementation_->state.finish();
}

std::expected<Sha256Digest, Sha256Error> compute_sha256(
    const IImageSource& source,
    const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        return std::unexpected(make_error(
            Sha256ErrorKind::Cancelled, 0, "SHA-256 computation was cancelled"));
    }

    const auto source_size = source.size();
    if (source_size > kSha256MaxInputSize) {
        return std::unexpected(make_error(
            Sha256ErrorKind::InvalidSize,
            0,
            "image source is too large for SHA-256"));
    }

    Sha256Accumulator state;
    std::array<std::byte, kSha256SourceReadSize> buffer{};
    std::uint64_t offset = 0;
    while (offset < source_size) {
        if (cancellation.stop_requested()) {
            return std::unexpected(make_error(
                Sha256ErrorKind::Cancelled,
                offset,
                "SHA-256 computation was cancelled"));
        }

        const auto remaining = source_size - offset;
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        auto read = source.read_at(offset, std::span(buffer).first(requested));
        if (!read) {
            auto message = std::string{"unable to read image source"};
            if (!read.error().message.empty()) {
                message += ": ";
                message += read.error().message;
            }
            return std::unexpected(make_error(
                Sha256ErrorKind::Source, offset, std::move(message)));
        }
        if (*read > requested) {
            return std::unexpected(make_error(
                Sha256ErrorKind::Source,
                offset,
                "image source returned more bytes than requested"));
        }
        if (*read == 0U) {
            return std::unexpected(make_error(
                Sha256ErrorKind::Truncated,
                offset,
                "image source made no progress before its declared size"));
        }
        if (cancellation.stop_requested()) {
            return std::unexpected(make_error(
                Sha256ErrorKind::Cancelled,
                offset,
                "SHA-256 computation was cancelled"));
        }

        state.update(std::span(buffer).first(*read));
        // *read is bounded by requested, which is bounded by source_size -
        // offset. The addition therefore cannot overflow.
        offset += static_cast<std::uint64_t>(*read);
    }

    if (cancellation.stop_requested()) {
        return std::unexpected(make_error(
            Sha256ErrorKind::Cancelled,
            offset,
            "SHA-256 computation was cancelled"));
    }
    return state.finish();
}

std::string sha256_hex(const Sha256Digest& digest) {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result(digest.size() * 2U, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = std::to_integer<unsigned int>(digest[index]);
        result[index * 2U] = digits[value >> 4U];
        result[index * 2U + 1U] = digits[value & 0x0fU];
    }
    return result;
}

}  // namespace kairosboot::image
