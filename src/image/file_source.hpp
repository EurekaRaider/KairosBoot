// SPDX-License-Identifier: MIT
#pragma once

#include "sparse_image.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace kairosboot::image {

enum class FileSourceErrorKind : std::uint8_t {
    InvalidArgument,
    NotFound,
    UnsafePath,
    NotRegularFile,
    SizeUnavailable,
    OpenFailed,
};

struct FileSourceError final {
    FileSourceErrorKind kind{FileSourceErrorKind::OpenFailed};
    int native_code{};
    std::string message;
};

// A bounded-memory random-access view of one file. The size is snapshotted at
// open time; reads never observe bytes appended later and a truncated file
// produces a short read that TransferSource adapters reject.
class FileImageSource final : public IImageSource {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
    open(const std::filesystem::path& path);

    // Opens a regular file below one directory without following symlinks or
    // reparse points. The relative path must contain only normal components.
    [[nodiscard]] static std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
    open_beneath(
        const std::filesystem::path& directory,
        const std::filesystem::path& relative_path);

    FileImageSource(const FileImageSource&) = delete;
    FileImageSource& operator=(const FileImageSource&) = delete;
    ~FileImageSource() override;

    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        std::uint64_t offset,
        std::span<std::byte> destination) const override;

private:
#if defined(_WIN32)
    using NativeHandle = void*;
#else
    using NativeHandle = int;
#endif

    FileImageSource(NativeHandle handle, std::uint64_t size);

    NativeHandle handle_;
    std::uint64_t size_{};
};

}  // namespace kairosboot::image
