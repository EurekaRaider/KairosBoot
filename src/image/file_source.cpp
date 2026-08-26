// SPDX-License-Identifier: MIT
#include "file_source.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

#if defined(_WIN32)

[[nodiscard]] std::error_code windows_error(const DWORD value) {
    return {static_cast<int>(value), std::system_category()};
}

class ScopedHandle final {
public:
    explicit ScopedHandle(const HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (handle_ != nullptr) {
            (void)::CloseHandle(handle_);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, nullptr);
    }

private:
    HANDLE handle_{};
};

#else

[[nodiscard]] std::error_code posix_error(const int value) {
    return {value, std::generic_category()};
}

class ScopedFileDescriptor final {
public:
    explicit ScopedFileDescriptor(const int descriptor) noexcept
        : descriptor_(descriptor) {}

    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

    ~ScopedFileDescriptor() {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

private:
    int descriptor_{-1};
};

#endif

}  // namespace

std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
FileImageSource::open(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::InvalidArgument, {}, "image path is empty"));
    }

#if defined(_WIN32)
    const auto native_handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (native_handle == INVALID_HANDLE_VALUE) {
        const auto native = ::GetLastError();
        const auto kind = native == ERROR_FILE_NOT_FOUND || native == ERROR_PATH_NOT_FOUND
                              ? FileSourceErrorKind::NotFound
                              : FileSourceErrorKind::OpenFailed;
        const auto message = kind == FileSourceErrorKind::NotFound
                                 ? "image file does not exist"
                                 : "unable to open the image file";
        return std::unexpected(file_error(kind, windows_error(native), message));
    }
    ScopedHandle handle(native_handle);

    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle.get(), &information) == FALSE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            windows_error(::GetLastError()),
            "unable to determine the image file size"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        ::GetFileType(handle.get()) != FILE_TYPE_DISK) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile,
            {},
            "image path is not a regular file"));
    }

    LARGE_INTEGER native_size{};
    if (::GetFileSizeEx(handle.get(), &native_size) == FALSE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            windows_error(::GetLastError()),
            "unable to determine the image file size"));
    }
    if (native_size.QuadPart < 0) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            {},
            "unable to determine the image file size"));
    }
    const auto raw_size = static_cast<std::uint64_t>(native_size.QuadPart);

    try {
        auto source = std::unique_ptr<FileImageSource>(
            new FileImageSource(handle.get(), raw_size));
        (void)handle.release();
        return std::shared_ptr<FileImageSource>(std::move(source));
    } catch (const std::bad_alloc&) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            std::make_error_code(std::errc::not_enough_memory),
            "unable to allocate the image file source"));
    }
#else
    int flags = O_RDONLY | O_NONBLOCK;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int native_descriptor = ::open(path.c_str(), flags);
    if (native_descriptor < 0) {
        const int native = errno;
        const auto kind = native == ENOENT || native == ENOTDIR
                              ? FileSourceErrorKind::NotFound
                              : FileSourceErrorKind::OpenFailed;
        const auto message = kind == FileSourceErrorKind::NotFound
                                 ? "image file does not exist"
                                 : "unable to open the image file";
        return std::unexpected(file_error(kind, posix_error(native), message));
    }
    ScopedFileDescriptor descriptor(native_descriptor);

    struct stat information {};
    if (::fstat(descriptor.get(), &information) != 0) {
        const int native = errno;
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            posix_error(native),
            "unable to determine the image file size"));
    }
    if (!S_ISREG(information.st_mode)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile,
            {},
            "image path is not a regular file"));
    }
    if (information.st_size < 0 ||
        !std::in_range<std::uint64_t>(information.st_size)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            {},
            "unable to determine the image file size"));
    }
    const auto raw_size = static_cast<std::uint64_t>(information.st_size);

    try {
        auto source = std::unique_ptr<FileImageSource>(
            new FileImageSource(descriptor.get(), raw_size));
        (void)descriptor.release();
        return std::shared_ptr<FileImageSource>(std::move(source));
    } catch (const std::bad_alloc&) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            std::make_error_code(std::errc::not_enough_memory),
            "unable to allocate the image file source"));
    }
#endif
}

FileImageSource::FileImageSource(const NativeHandle handle, const std::uint64_t size)
    : handle_(handle), size_(size) {}

FileImageSource::~FileImageSource() {
#if defined(_WIN32)
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        (void)::CloseHandle(static_cast<HANDLE>(handle_));
    }
#else
    if (handle_ >= 0) {
        (void)::close(handle_);
    }
#endif
}

std::uint64_t FileImageSource::size() const noexcept {
    return size_;
}

std::expected<std::size_t, ImageSourceError> FileImageSource::read_at(
    const std::uint64_t offset,
    const std::span<std::byte> destination) const {
    if (destination.empty() || offset >= size_) {
        return std::size_t{0};
    }

    const auto available = size_ - offset;
    const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        available, static_cast<std::uint64_t>(destination.size())));

#if defined(_WIN32)
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<LONGLONG>::max())) {
        return std::unexpected(read_error("image read offset is not representable"));
    }

    std::size_t completed = 0;
    while (completed < amount) {
        const auto current_offset = offset + completed;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(current_offset);

        OVERLAPPED operation{};
        operation.Offset = position.LowPart;
        operation.OffsetHigh = static_cast<DWORD>(position.HighPart);
        ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (event.get() == nullptr) {
            return std::unexpected(read_error("unable to read the image file"));
        }
        operation.hEvent = event.get();

        const auto remaining = amount - completed;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD transferred = 0;
        if (::ReadFile(
                static_cast<HANDLE>(handle_),
                destination.data() + completed,
                chunk,
                &transferred,
                &operation) == FALSE) {
            auto native = ::GetLastError();
            if (native == ERROR_IO_PENDING) {
                if (::GetOverlappedResult(
                        static_cast<HANDLE>(handle_),
                        &operation,
                        &transferred,
                        TRUE) != FALSE) {
                    native = ERROR_SUCCESS;
                } else {
                    native = ::GetLastError();
                }
            }
            if (native == ERROR_HANDLE_EOF) {
                return completed;
            }
            if (native != ERROR_SUCCESS) {
                return std::unexpected(read_error("unable to read the image file"));
            }
        }
        if (transferred == 0) {
            return completed;
        }
        if (transferred > chunk) {
            return std::unexpected(read_error("unable to read the image file"));
        }
        completed += static_cast<std::size_t>(transferred);
    }
    return completed;
#else
    if (!std::in_range<off_t>(offset)) {
        return std::unexpected(read_error("image read offset is not representable"));
    }

    std::size_t completed = 0;
    while (completed < amount) {
        const auto current_offset = offset + completed;
        if (!std::in_range<off_t>(current_offset)) {
            return std::unexpected(read_error("image read offset is not representable"));
        }
        const auto remaining = amount - completed;
        const auto chunk = std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto transferred = ::pread(
            handle_,
            destination.data() + completed,
            chunk,
            static_cast<off_t>(current_offset));
        if (transferred > 0) {
            completed += static_cast<std::size_t>(transferred);
            continue;
        }
        if (transferred == 0) {
            return completed;
        }
        if (errno == EINTR) {
            continue;
        }
        return std::unexpected(read_error("unable to read the image file"));
    }
    return completed;
#endif
}

}  // namespace kairosboot::image
