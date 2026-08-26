// SPDX-License-Identifier: MIT
#include "file_source.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

[[nodiscard]] bool path_contains_nul(
    const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return native.find(std::filesystem::path::value_type{}) !=
           std::filesystem::path::string_type::npos;
}

#if defined(_WIN32)

[[nodiscard]] std::error_code windows_error(const DWORD value) {
    return {static_cast<int>(value), std::system_category()};
}

[[nodiscard]] bool windows_reserved_component(
    const std::wstring_view component) noexcept {
    const auto separator = component.find(L'.');
    const auto stem = component.substr(0U, separator);
    std::array<wchar_t, 7> folded{};
    if (stem.size() > folded.size()) {
        return false;
    }
    for (std::size_t index = 0; index < stem.size(); ++index) {
        const auto character = stem[index];
        folded[index] = character >= L'a' && character <= L'z'
                            ? static_cast<wchar_t>(character - L'a' + L'A')
                            : character;
    }
    const auto normalized = std::wstring_view(folded.data(), stem.size());
    if (normalized == L"CON" || normalized == L"PRN" || normalized == L"AUX" ||
        normalized == L"NUL" || normalized == L"CONIN$" ||
        normalized == L"CONOUT$" || normalized == L"CLOCK$") {
        return true;
    }
    return normalized.size() == 4U &&
           (normalized.starts_with(L"COM") || normalized.starts_with(L"LPT")) &&
           normalized[3] >= L'1' && normalized[3] <= L'9';
}

[[nodiscard]] bool windows_path_has_alias(
    const std::filesystem::path& path) {
    const auto root_name = path.root_name();
    const auto root_directory = path.root_directory();
    for (const auto& component : path) {
        if (component.empty() || component == root_name ||
            component == root_directory) {
            continue;
        }
        const auto& native = component.native();
        if (native == L"." || native == L".." || native.back() == L'.' ||
            native.back() == L' ' || native.find(L':') != std::wstring::npos ||
            windows_reserved_component(native)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::expected<void, FileSourceError> make_non_inheritable(
    const HANDLE handle, const std::string_view description) {
    if (::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0U) != FALSE) {
        return {};
    }
    return std::unexpected(file_error(
        FileSourceErrorKind::OpenFailed,
        windows_error(::GetLastError()),
        std::string("unable to make ") + std::string(description) +
            " non-inheritable"));
}

[[nodiscard]] std::expected<std::wstring, FileSourceError> final_path(
    const HANDLE handle) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = ::GetFinalPathNameByHandleW(handle, nullptr, 0U, flags);
    if (required == 0U) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            windows_error(::GetLastError()),
            "unable to resolve the final image path"));
    }
    try {
        std::wstring path(static_cast<std::size_t>(required), L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(
            handle, path.data(), required, flags);
        if (written == 0U || written >= required) {
            return std::unexpected(file_error(
                FileSourceErrorKind::OpenFailed,
                windows_error(::GetLastError()),
                "unable to resolve the final image path"));
        }
        path.resize(static_cast<std::size_t>(written));
        while (path.size() > 1U &&
               (path.back() == L'\\' || path.back() == L'/')) {
            path.pop_back();
        }
        return path;
    } catch (const std::bad_alloc&) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            std::make_error_code(std::errc::not_enough_memory),
            "unable to allocate the final image path"));
    }
}

[[nodiscard]] bool is_immediate_final_child(
    const std::wstring_view parent,
    const std::wstring_view child) noexcept {
    if (child.size() <= parent.size() + 1U ||
        parent != child.substr(0U, parent.size()) ||
        (child[parent.size()] != L'\\' && child[parent.size()] != L'/')) {
        return false;
    }
    const auto leaf = child.substr(parent.size() + 1U);
    return leaf.find_first_of(L"\\/") == std::wstring_view::npos;
}

class ScopedHandle final {
public:
    explicit ScopedHandle(const HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_(other.release()) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                (void)::CloseHandle(handle_);
            }
            handle_ = other.release();
        }
        return *this;
    }

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

[[nodiscard]] std::expected<void, FileSourceError> make_close_on_exec(
    const int descriptor, const std::string_view description) {
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags >= 0 && (flags & FD_CLOEXEC) != 0) {
        return {};
    }
    if (flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0) {
        return {};
    }
    return std::unexpected(file_error(
        FileSourceErrorKind::OpenFailed,
        posix_error(errno),
        std::string("unable to make ") + std::string(description) +
            " close-on-exec"));
}

class ScopedFileDescriptor final {
public:
    explicit ScopedFileDescriptor(const int descriptor) noexcept
        : descriptor_(descriptor) {}

    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

    ScopedFileDescriptor(ScopedFileDescriptor&& other) noexcept
        : descriptor_(other.release()) {}

    ScopedFileDescriptor& operator=(ScopedFileDescriptor&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) {
                (void)::close(descriptor_);
            }
            descriptor_ = other.release();
        }
        return *this;
    }

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

[[nodiscard]] std::expected<void, FileSourceError> validate_relative_path(
    const std::filesystem::path& path) {
    if (path.empty() || path_contains_nul(path) || path.is_absolute() ||
        path.has_root_path()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image path below directory must be relative"));
    }
#if defined(_WIN32)
    if (windows_path_has_alias(path)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image path below directory aliases a Win32-normalized name"));
    }
#endif
    for (const auto& component : path) {
        if (component.empty() || component == "." || component == "..") {
            return std::unexpected(file_error(
                FileSourceErrorKind::UnsafePath,
                {},
                "image path below directory contains an unsafe component"));
        }
    }
    return {};
}

}  // namespace

std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
FileImageSource::open(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::InvalidArgument, {}, "image path is empty"));
    }
    if (path_contains_nul(path)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath, {},
            "image path contains an embedded NUL"));
    }

#if defined(_WIN32)
    if (windows_path_has_alias(path)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath, {},
            "image path aliases a Win32-normalized or reserved name"));
    }
    const auto native_handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
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
    if (auto inherited = make_non_inheritable(handle.get(), "image file");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle.get(), &information) == FALSE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            windows_error(::GetLastError()),
            "unable to determine the image file size"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image path is a reparse point"));
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
    int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int native_descriptor = ::open(path.c_str(), flags);
    if (native_descriptor < 0) {
        const int native = errno;
        const auto kind = native == ENOENT || native == ENOTDIR
                              ? FileSourceErrorKind::NotFound
                          : native == ELOOP
                              ? FileSourceErrorKind::UnsafePath
                              : FileSourceErrorKind::OpenFailed;
        const auto message = kind == FileSourceErrorKind::NotFound
                                 ? "image file does not exist"
                             : kind == FileSourceErrorKind::UnsafePath
                                 ? "image path is a symbolic link"
                                 : "unable to open the image file";
        return std::unexpected(file_error(kind, posix_error(native), message));
    }
    ScopedFileDescriptor descriptor(native_descriptor);
    if (auto inherited = make_close_on_exec(descriptor.get(), "image file");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }

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

std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
FileImageSource::open_beneath(
    const std::filesystem::path& directory,
    const std::filesystem::path& relative_path) {
    if (directory.empty()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::InvalidArgument,
            {},
            "image base directory is empty"));
    }
    if (path_contains_nul(directory)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image base directory contains an embedded NUL"));
    }
#if defined(_WIN32)
    if (windows_path_has_alias(directory)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image base directory aliases a Win32-normalized or reserved name"));
    }
#endif
    if (auto validated = validate_relative_path(relative_path); !validated) {
        return std::unexpected(std::move(validated.error()));
    }

    std::vector<std::filesystem::path> components;
    for (const auto& component : relative_path) {
        components.push_back(component);
    }

#if defined(_WIN32)
    constexpr DWORD directory_sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
    const auto root_handle = ::CreateFileW(
        directory.c_str(),
        FILE_READ_ATTRIBUTES,
        directory_sharing,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (root_handle == INVALID_HANDLE_VALUE) {
        const auto native = ::GetLastError();
        const auto kind = native == ERROR_FILE_NOT_FOUND || native == ERROR_PATH_NOT_FOUND
                              ? FileSourceErrorKind::NotFound
                              : FileSourceErrorKind::OpenFailed;
        return std::unexpected(file_error(
            kind,
            windows_error(native),
            kind == FileSourceErrorKind::NotFound
                ? "image base directory does not exist"
                : "unable to open the image base directory"));
    }
    std::vector<ScopedHandle> held_directories;
    held_directories.emplace_back(root_handle);
    if (auto inherited =
            make_non_inheritable(held_directories.front().get(), "image directory");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }

    BY_HANDLE_FILE_INFORMATION root_information{};
    if (::GetFileInformationByHandle(
            held_directories.front().get(), &root_information) == FALSE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            windows_error(::GetLastError()),
            "unable to inspect the image base directory"));
    }
    if ((root_information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image base directory is a reparse point"));
    }
    if ((root_information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile,
            {},
            "image base path is not a directory"));
    }
    auto current_final_path = final_path(held_directories.front().get());
    if (!current_final_path) {
        return std::unexpected(std::move(current_final_path.error()));
    }

    auto candidate = directory;
    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool leaf = index + 1U == components.size();
        candidate /= components[index];
        const auto handle = ::CreateFileW(
            candidate.c_str(),
            leaf ? GENERIC_READ : FILE_READ_ATTRIBUTES,
            leaf ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
                 : directory_sharing,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                (leaf ? FILE_FLAG_OVERLAPPED : 0U),
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto native = ::GetLastError();
            const auto kind = native == ERROR_FILE_NOT_FOUND ||
                                      native == ERROR_PATH_NOT_FOUND
                                  ? FileSourceErrorKind::NotFound
                                  : FileSourceErrorKind::OpenFailed;
            return std::unexpected(file_error(
                kind,
                windows_error(native),
                kind == FileSourceErrorKind::NotFound
                    ? "image file below directory does not exist"
                    : "unable to open image path below directory"));
        }
        ScopedHandle opened(handle);
        if (auto inherited = make_non_inheritable(
                opened.get(), leaf ? "image file" : "image directory");
            !inherited) {
            return std::unexpected(std::move(inherited.error()));
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (::GetFileInformationByHandle(opened.get(), &information) == FALSE) {
            return std::unexpected(file_error(
                FileSourceErrorKind::OpenFailed,
                windows_error(::GetLastError()),
                "unable to inspect image path below directory"));
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return std::unexpected(file_error(
                FileSourceErrorKind::UnsafePath,
                {},
                "image path below directory traverses a reparse point"));
        }
        auto opened_final_path = final_path(opened.get());
        if (!opened_final_path) {
            return std::unexpected(std::move(opened_final_path.error()));
        }
        if (!is_immediate_final_child(*current_final_path, *opened_final_path)) {
            return std::unexpected(file_error(
                FileSourceErrorKind::UnsafePath,
                {},
                "image path below directory escaped its parent handle"));
        }
        if (!leaf) {
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
                return std::unexpected(file_error(
                    FileSourceErrorKind::UnsafePath,
                    {},
                    "image path below directory contains a non-directory component"));
            }
            held_directories.push_back(std::move(opened));
            current_final_path = std::move(opened_final_path);
            continue;
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
            ::GetFileType(opened.get()) != FILE_TYPE_DISK) {
            return std::unexpected(file_error(
                FileSourceErrorKind::NotRegularFile,
                {},
                "image path below directory is not a regular file"));
        }
        LARGE_INTEGER native_size{};
        if (::GetFileSizeEx(opened.get(), &native_size) == FALSE) {
            const auto native = ::GetLastError();
            return std::unexpected(file_error(
                FileSourceErrorKind::SizeUnavailable,
                windows_error(native),
                "unable to determine image size below directory"));
        }
        if (native_size.QuadPart < 0) {
            return std::unexpected(file_error(
                FileSourceErrorKind::SizeUnavailable,
                {},
                "image size below directory is invalid"));
        }
        try {
            auto source = std::unique_ptr<FileImageSource>(new FileImageSource(
                opened.get(), static_cast<std::uint64_t>(native_size.QuadPart)));
            (void)opened.release();
            return std::shared_ptr<FileImageSource>(std::move(source));
        } catch (const std::bad_alloc&) {
            return std::unexpected(file_error(
                FileSourceErrorKind::OpenFailed,
                std::make_error_code(std::errc::not_enough_memory),
                "unable to allocate image source below directory"));
        }
    }
#else
    int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
    directory_flags |= O_CLOEXEC;
#endif
    const int root_descriptor = ::open(directory.c_str(), directory_flags);
    if (root_descriptor < 0) {
        const int native = errno;
        const auto kind = native == ENOENT
                              ? FileSourceErrorKind::NotFound
                          : native == ELOOP || native == ENOTDIR
                              ? FileSourceErrorKind::UnsafePath
                              : FileSourceErrorKind::OpenFailed;
        return std::unexpected(file_error(
            kind,
            posix_error(native),
            kind == FileSourceErrorKind::UnsafePath
                ? "image base directory is a symbolic link or unsafe path"
                : "unable to open the image base directory"));
    }
    ScopedFileDescriptor current(root_descriptor);
    if (auto inherited = make_close_on_exec(current.get(), "image directory");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }
    struct stat root_information {};
    if (::fstat(current.get(), &root_information) != 0 ||
        !S_ISDIR(root_information.st_mode)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile,
            {},
            "image base path is not a directory"));
    }

    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool leaf = index + 1U == components.size();
        int flags = O_RDONLY | O_NOFOLLOW;
#if defined(O_CLOEXEC)
        flags |= O_CLOEXEC;
#endif
        if (leaf) {
            flags |= O_NONBLOCK;
        } else {
            flags |= O_DIRECTORY;
        }
        const int descriptor = ::openat(
            current.get(), components[index].c_str(), flags);
        if (descriptor < 0) {
            const int native = errno;
            const auto kind = native == ENOENT
                                  ? FileSourceErrorKind::NotFound
                              : native == ELOOP || native == ENOTDIR
                                  ? FileSourceErrorKind::UnsafePath
                                  : FileSourceErrorKind::OpenFailed;
            return std::unexpected(file_error(
                kind,
                posix_error(native),
                kind == FileSourceErrorKind::UnsafePath
                    ? "image path below directory traverses a symbolic link or unsafe component"
                    : "unable to open image path below directory"));
        }
        ScopedFileDescriptor opened(descriptor);
        if (auto inherited = make_close_on_exec(
                opened.get(), leaf ? "image file" : "image directory");
            !inherited) {
            return std::unexpected(std::move(inherited.error()));
        }
        struct stat information {};
        if (::fstat(opened.get(), &information) != 0) {
            const int native = errno;
            return std::unexpected(file_error(
                FileSourceErrorKind::SizeUnavailable,
                posix_error(native),
                "unable to inspect image path below directory"));
        }
        if (!leaf) {
            if (!S_ISDIR(information.st_mode)) {
                return std::unexpected(file_error(
                    FileSourceErrorKind::UnsafePath,
                    {},
                    "image path below directory contains a non-directory component"));
            }
            current = std::move(opened);
            continue;
        }
        if (!S_ISREG(information.st_mode)) {
            return std::unexpected(file_error(
                FileSourceErrorKind::NotRegularFile,
                {},
                "image path below directory is not a regular file"));
        }
        if (information.st_size < 0 ||
            !std::in_range<std::uint64_t>(information.st_size)) {
            return std::unexpected(file_error(
                FileSourceErrorKind::SizeUnavailable,
                {},
                "unable to determine image size below directory"));
        }
        try {
            auto source = std::unique_ptr<FileImageSource>(new FileImageSource(
                opened.get(), static_cast<std::uint64_t>(information.st_size)));
            (void)opened.release();
            return std::shared_ptr<FileImageSource>(std::move(source));
        } catch (const std::bad_alloc&) {
            return std::unexpected(file_error(
                FileSourceErrorKind::OpenFailed,
                std::make_error_code(std::errc::not_enough_memory),
                "unable to allocate image source below directory"));
        }
    }
#endif
    return std::unexpected(file_error(
        FileSourceErrorKind::InvalidArgument,
        {},
        "image path below directory contains no components"));
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
