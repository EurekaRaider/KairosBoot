// SPDX-License-Identifier: MIT
#pragma once

#include "sparse_image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <stop_token>
#include <string>

namespace kairosboot::image {

inline constexpr std::size_t kSha256DigestSize = 32U;
inline constexpr std::size_t kSha256SourceReadSize = 64U * 1024U;
inline constexpr std::uint64_t kSha256MaxInputSize =
    std::numeric_limits<std::uint64_t>::max() / 8U;

using Sha256Digest = std::array<std::byte, kSha256DigestSize>;

enum class Sha256ErrorKind : std::uint8_t {
    InvalidSize,
    Source,
    Truncated,
    Cancelled,
};

struct Sha256Error final {
    Sha256ErrorKind kind{Sha256ErrorKind::Source};
    std::uint64_t input_offset{};
    std::string message;
};

// Hashes the source exactly once using bounded random-access reads. Successful
// short reads are completed at the next offset. Contents and size must remain
// immutable for the duration of the call, as required by IImageSource.
[[nodiscard]] std::expected<Sha256Digest, Sha256Error> compute_sha256(
    const IImageSource& source,
    std::stop_token cancellation = {});

[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);

static_assert(sizeof(Sha256Digest) == kSha256DigestSize);

}  // namespace kairosboot::image
