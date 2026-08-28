// SPDX-License-Identifier: MIT
#include "vbmeta_flag_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace kairosboot::image {
namespace {

inline constexpr std::uint64_t kVbmetaHeaderSize = 256U;
inline constexpr std::uint64_t kVbmetaFlagsByteOffset = 123U;
inline constexpr std::array<std::byte, 4> kVbmetaMagic{
    std::byte{'A'}, std::byte{'V'}, std::byte{'B'}, std::byte{'0'}};

[[nodiscard]] VbmetaFlagError failure(const VbmetaFlagErrorKind kind,
                                      const std::uint64_t offset,
                                      std::string message) {
    return VbmetaFlagError{kind, offset, std::move(message)};
}

class VbmetaFlagSource final : public IImageSource {
public:
    VbmetaFlagSource(std::shared_ptr<const IImageSource> source,
                     const std::byte flags_byte) noexcept
        : source_(std::move(source)), flags_byte_(flags_byte) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return source_->size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        auto read = source_->read_at(offset, destination);
        if (!read || *read == 0U || offset > kVbmetaFlagsByteOffset) {
            return read;
        }
        const auto distance = kVbmetaFlagsByteOffset - offset;
        if (distance < *read &&
            distance <= std::numeric_limits<std::size_t>::max()) {
            destination[static_cast<std::size_t>(distance)] = flags_byte_;
        }
        return read;
    }

private:
    std::shared_ptr<const IImageSource> source_;
    std::byte flags_byte_{};
};

}  // namespace

bool is_vbmeta_partition(const std::string_view partition) noexcept {
    return partition.ends_with("vbmeta") || partition.ends_with("vbmeta_a") ||
           partition.ends_with("vbmeta_b");
}

std::expected<std::shared_ptr<const IImageSource>, VbmetaFlagError>
apply_vbmeta_flags(std::shared_ptr<const IImageSource> source,
                   const VbmetaFlags flags,
                   const std::stop_token cancellation) {
    if (!source) {
        return std::unexpected(failure(
            VbmetaFlagErrorKind::Source, 0U,
            "vbmeta flag mutation requires a non-null image source"));
    }
    if (!flags.any() || source->size() < kVbmetaHeaderSize) {
        return source;
    }
    if (cancellation.stop_requested()) {
        return std::unexpected(failure(
            VbmetaFlagErrorKind::Cancelled, 0U,
            "vbmeta flag mutation was cancelled"));
    }

    std::array<std::byte, kVbmetaHeaderSize> header{};
    std::size_t completed = 0U;
    while (completed < header.size()) {
        if (cancellation.stop_requested()) {
            return std::unexpected(failure(
                VbmetaFlagErrorKind::Cancelled, completed,
                "vbmeta flag mutation was cancelled"));
        }
        auto read = source->read_at(
            completed, std::span(header).subspan(completed));
        if (!read) {
            return std::unexpected(failure(
                VbmetaFlagErrorKind::Source, completed,
                "failed reading vbmeta header: " + read.error().message));
        }
        if (*read == 0U || *read > header.size() - completed) {
            return std::unexpected(failure(
                VbmetaFlagErrorKind::Source, completed,
                "vbmeta source was truncated while reading its header"));
        }
        completed += *read;
    }

    if (!std::equal(kVbmetaMagic.begin(), kVbmetaMagic.end(),
                    header.begin())) {
        return std::unexpected(failure(
            VbmetaFlagErrorKind::Malformed, 0U,
            "failed to find AVB0 magic at offset 0"));
    }

    auto flag_byte = header[kVbmetaFlagsByteOffset];
    if (flags.disable_verity) {
        flag_byte |= std::byte{0x01U};
    }
    if (flags.disable_verification) {
        flag_byte |= std::byte{0x02U};
    }
    if (flag_byte == header[kVbmetaFlagsByteOffset]) {
        return source;
    }
    return std::shared_ptr<const IImageSource>{
        std::make_shared<VbmetaFlagSource>(std::move(source), flag_byte)};
}

}  // namespace kairosboot::image
