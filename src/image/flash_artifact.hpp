// SPDX-License-Identifier: MIT
#pragma once

#include "sparse_image.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace kairosboot::image {

enum class FlashArtifactKind : std::uint8_t {
    Raw,
    AndroidSparse,
};

struct FlashArtifactMetadata final {
    FlashArtifactKind kind{FlashArtifactKind::Raw};
    // Bytes sent to the device. For Android sparse images this remains the
    // encoded sparse file size, not the expanded image size.
    std::uint64_t transfer_size{0};
    std::uint64_t expanded_size{0};
    std::optional<SparseHeader> sparse_header;
};

// A validated flash input. Sparse classification parses and verifies the
// complete sparse structure and declared CRCs, while transfer_source() always
// preserves the caller's original encoded source.
class FlashArtifact final {
public:
    [[nodiscard]] static std::expected<FlashArtifact, SparseError> inspect(
        std::shared_ptr<const IImageSource> source);

    FlashArtifact(const FlashArtifact&) = delete;
    FlashArtifact& operator=(const FlashArtifact&) = delete;
    FlashArtifact(FlashArtifact&&) noexcept = default;
    FlashArtifact& operator=(FlashArtifact&&) noexcept = default;

    [[nodiscard]] const FlashArtifactMetadata& metadata() const noexcept;
    [[nodiscard]] const std::shared_ptr<const IImageSource>& transfer_source()
        const noexcept;
    [[nodiscard]] const SparseImage* sparse_image() const noexcept;

private:
    FlashArtifact(
        std::shared_ptr<const IImageSource> transfer_source,
        FlashArtifactMetadata metadata,
        std::optional<SparseImage> sparse_image);

    std::shared_ptr<const IImageSource> transfer_source_;
    FlashArtifactMetadata metadata_;
    std::optional<SparseImage> sparse_image_;
};

}  // namespace kairosboot::image
