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

// Stable metadata captured from one already-open filesystem object. Package
// snapshots use it to reject path replacement and in-place mutation between
// inventory and materialization. This is an internal type, not public ABI.
struct FileSnapshotIdentity final {
    std::uint64_t device{};
    std::uint64_t object{};
    std::uint64_t size{};
    std::int64_t modified_seconds{};
    std::uint32_t modified_nanoseconds{};
    std::int64_t changed_seconds{};
    std::uint32_t changed_nanoseconds{};
    bool directory{};

    friend bool operator==(const FileSnapshotIdentity&,
                           const FileSnapshotIdentity&) = default;
};

// A bounded-memory random-access view of one file. The size is snapshotted at
// open time; reads never observe bytes appended later and a truncated file
// produces a short read that TransferSource adapters reject.
class FileImageSource final : public IImageSource {
public:
    [[nodiscard]] static std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
    open(const std::filesystem::path& path);

    // Opens a path without following a final symlink/reparse point and returns
    // identity metadata for either a regular file or directory.
    [[nodiscard]] static std::expected<FileSnapshotIdentity, FileSourceError>
    inspect_snapshot_identity(const std::filesystem::path& path);

    // Opens a regular file below one directory without following symlinks or
    // reparse points. The relative path must contain only normal components.
    [[nodiscard]] static std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
    open_beneath(
        const std::filesystem::path& directory,
        const std::filesystem::path& relative_path,
        const FileSnapshotIdentity* expected_directory_identity = nullptr,
        const FileSnapshotIdentity* expected_file_identity = nullptr);

    FileImageSource(const FileImageSource&) = delete;
    FileImageSource& operator=(const FileImageSource&) = delete;
    ~FileImageSource() override;

    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] std::expected<FileSnapshotIdentity, FileSourceError>
    snapshot_identity() const;
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
