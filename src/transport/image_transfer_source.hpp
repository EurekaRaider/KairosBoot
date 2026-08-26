// SPDX-License-Identifier: MIT
#pragma once

#include "src/image/sparse_image.hpp"
#include "transfer_ring.hpp"

#include <expected>
#include <memory>

namespace kairosboot::transport {

// Adapts the single image::IImageSource contract to the exact-read contract
// consumed by TransferRing. This keeps file, memory, sparse, and future spool
// sources behind one random-access abstraction.
class ImageTransferSource final : public TransferSource {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<ImageTransferSource>,
                                       image::ImageSourceError>
    create(std::shared_ptr<const image::IImageSource> source);

    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] bool read_exact(
        std::uint64_t offset,
        std::span<std::byte> destination) noexcept override;

private:
    explicit ImageTransferSource(std::shared_ptr<const image::IImageSource> source);

    std::shared_ptr<const image::IImageSource> source_;
    std::uint64_t size_{};
};

}  // namespace kairosboot::transport
