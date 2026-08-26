// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace kairosboot::image {

inline constexpr std::uint32_t kAndroidSparseMagic = 0xED26FF3AU;
inline constexpr std::uint16_t kAndroidSparseMajorVersion = 1;
inline constexpr std::uint16_t kSparseChunkRaw = 0xCAC1U;
inline constexpr std::uint16_t kSparseChunkFill = 0xCAC2U;
inline constexpr std::uint16_t kSparseChunkDontCare = 0xCAC3U;
inline constexpr std::uint16_t kSparseChunkCrc32 = 0xCAC4U;
// Bounds parser metadata independently of a source's claimed size. Inputs above
// this limit are classified Unsupported rather than risking multi-gigabyte
// allocation from an untrusted total_chunks field.
inline constexpr std::uint32_t kMaxSparseChunks = 1'000'000U;

enum class SparseErrorKind : std::uint8_t {
    Malformed,
    Truncated,
    Unsupported,
    Source,
    InvalidArgument,
    Cancelled,
};

struct SparseError {
    SparseErrorKind kind;
    std::uint64_t input_offset;
    std::string message;
};

struct ImageSourceError {
    std::string message;
};

// Internal random-access source seam. Implementations may be file-, memory-, or
// spool-backed and may return successful short reads. A zero-byte read before
// size() is reached is treated as truncation by the sparse reader. Contents and
// size must remain immutable for the lifetime of every SparseImage using it.
class IImageSource {
public:
    virtual ~IImageSource() = default;

    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual std::expected<std::size_t, ImageSourceError> read_at(
        std::uint64_t offset,
        std::span<std::byte> destination) const = 0;
};

enum class SparseChunkKind : std::uint8_t {
    Raw,
    Fill,
    DontCare,
    Crc32,
};

struct SparseChunk {
    SparseChunkKind kind;
    std::uint32_t block_count;
    std::uint64_t input_offset;
    std::uint64_t output_offset;
    std::uint64_t output_size;
    std::optional<std::uint32_t> fill_value;
    std::optional<std::uint32_t> checksum;
};

struct SparseHeader {
    std::uint16_t major_version;
    std::uint16_t minor_version;
    std::uint16_t file_header_size;
    std::uint16_t chunk_header_size;
    std::uint32_t block_size;
    std::uint32_t total_blocks;
    std::uint32_t total_chunks;
    std::uint32_t image_checksum;
};

// Parsed sparse image metadata plus a bounded-memory output view. open()
// validates the complete chunk table and declared checksums. read_at() expands
// only the requested output window.
class SparseImage final {
public:
    [[nodiscard]] static std::expected<SparseImage, SparseError> open(
        std::shared_ptr<const IImageSource> source,
        std::stop_token cancellation = {});

    [[nodiscard]] const SparseHeader& header() const noexcept;
    [[nodiscard]] std::span<const SparseChunk> chunks() const noexcept;
    [[nodiscard]] std::uint64_t output_size() const noexcept;

    [[nodiscard]] std::expected<std::size_t, SparseError> read_at(
        std::uint64_t output_offset,
        std::span<std::byte> destination) const;

private:
    SparseImage(
        std::shared_ptr<const IImageSource> source,
        SparseHeader header,
        std::uint64_t output_size,
        std::vector<SparseChunk> chunks,
        std::vector<std::size_t> output_chunk_indices);

    std::shared_ptr<const IImageSource> source_;
    SparseHeader header_;
    std::uint64_t output_size_;
    std::vector<SparseChunk> chunks_;
    std::vector<std::size_t> output_chunk_indices_;
};

}  // namespace kairosboot::image
