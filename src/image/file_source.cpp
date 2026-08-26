// SPDX-License-Identifier: MIT
#include "file_source.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

namespace kairosboot::image {
namespace {

[[nodiscard]] FileSourceError file_error(
    const FileSourceErrorKind kind,
    const std::error_code& native,
    std::string message) {
    return {
        .kind = kind,
        .native_code = native.value(),
        .message = std::move(message),
    };
}

[[nodiscard]] ImageSourceError read_error(std::string message) {
    return {.message = std::move(message)};
}

}  // namespace

std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
FileImageSource::open(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::InvalidArgument, {}, "image path is empty"));
    }

    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error) {
        const auto kind = status_error == std::errc::no_such_file_or_directory
                              ? FileSourceErrorKind::NotFound
                              : FileSourceErrorKind::OpenFailed;
        return std::unexpected(file_error(
            kind, status_error, "unable to inspect the image file"));
    }
    if (!std::filesystem::exists(status)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotFound, {}, "image file does not exist"));
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile,
            {},
            "image path is not a regular file"));
    }

    std::error_code size_error;
    const auto raw_size = std::filesystem::file_size(path, size_error);
    if (size_error || !std::in_range<std::uint64_t>(raw_size)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            size_error,
            "unable to determine the image file size"));
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed, {}, "unable to open the image file"));
    }

    try {
        return std::shared_ptr<FileImageSource>(
            new FileImageSource(std::move(stream), static_cast<std::uint64_t>(raw_size)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            std::make_error_code(std::errc::not_enough_memory),
            "unable to allocate the image file source"));
    }
}

FileImageSource::FileImageSource(std::ifstream stream, const std::uint64_t size)
    : stream_(std::move(stream)), size_(size) {}

std::uint64_t FileImageSource::size() const noexcept {
    return size_;
}

std::expected<std::size_t, ImageSourceError> FileImageSource::read_at(
    const std::uint64_t offset,
    const std::span<std::byte> destination) const {
    if (destination.empty() || offset >= size_) {
        return std::size_t{0};
    }
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return std::unexpected(read_error("image read offset is not representable"));
    }

    const auto available = size_ - offset;
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        available, static_cast<std::uint64_t>(destination.size())));
    if (amount > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected(read_error("image read size is not representable"));
    }

    std::lock_guard lock(mutex_);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) {
        return std::unexpected(read_error("unable to seek in the image file"));
    }
    stream_.read(
        reinterpret_cast<char*>(destination.data()),
        static_cast<std::streamsize>(amount));
    const auto transferred = stream_.gcount();
    if (transferred < 0 || stream_.bad()) {
        return std::unexpected(read_error("unable to read the image file"));
    }
    return static_cast<std::size_t>(transferred);
}

}  // namespace kairosboot::image
