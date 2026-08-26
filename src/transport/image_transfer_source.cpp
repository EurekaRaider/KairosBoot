// SPDX-License-Identifier: MIT
#include "image_transfer_source.hpp"

#include <limits>
#include <new>
#include <utility>

namespace kairosboot::transport {

std::expected<std::shared_ptr<ImageTransferSource>, image::ImageSourceError>
ImageTransferSource::create(std::shared_ptr<const image::IImageSource> source) {
    if (source == nullptr) {
        return std::unexpected(image::ImageSourceError{
            .message = "image transfer source is null",
        });
    }
    try {
        return std::shared_ptr<ImageTransferSource>(
            new ImageTransferSource(std::move(source)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(image::ImageSourceError{
            .message = "unable to allocate the image transfer source",
        });
    }
}

ImageTransferSource::ImageTransferSource(
    std::shared_ptr<const image::IImageSource> source)
    : source_(std::move(source)), size_(source_->size()) {}

std::uint64_t ImageTransferSource::size() const noexcept {
    return size_;
}

bool ImageTransferSource::read_exact(
    const std::uint64_t offset,
    const std::span<std::byte> destination) noexcept {
    if (destination.empty()) {
        return offset <= size_;
    }
    if (offset > size_ || destination.size() > size_ - offset) {
        return false;
    }

    std::size_t completed = 0;
    try {
        while (completed < destination.size()) {
            const auto next_offset = offset + completed;
            auto result = source_->read_at(next_offset, destination.subspan(completed));
            if (!result || *result == 0 || *result > destination.size() - completed) {
                return false;
            }
            completed += *result;
        }
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace kairosboot::transport
