// SPDX-License-Identifier: MIT
#include "file_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
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

[[nodiscard]] std::expected<FileSnapshotIdentity, FileSourceError>
snapshot_identity_from_handle(
    const HANDLE handle,
    const BY_HANDLE_FILE_INFORMATION& information) {
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            windows_error(::GetLastError()),
            "unable to inspect image snapshot timestamps"));
    }
    const bool directory =
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    const auto size = directory
                          ? std::uint64_t{0}
                          : (static_cast<std::uint64_t>(information.nFileSizeHigh)
                             << 32U) |
                                information.nFileSizeLow;
    return FileSnapshotIdentity{
        .device = information.dwVolumeSerialNumber,
        .object = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
                  information.nFileIndexLow,
        .size = size,
        // Windows exposes 100-nanosecond ticks rather than POSIX seconds. Only
        // equality is used, so retain the exact native value without conversion.
        .modified_seconds = basic.LastWriteTime.QuadPart,
        .modified_nanoseconds = 0U,
        .changed_seconds = basic.ChangeTime.QuadPart,
        .changed_nanoseconds = 0U,
        .directory = directory,
    };
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

using NativeNtStatus = LONG;

// NtCreateFile interprets ObjectName relative to RootDirectory. Opening one
// validated component at a time with FILE_OPEN_REPARSE_POINT keeps traversal
// anchored to the retained directory HANDLE and makes reparse rejection
// observable on the returned handle.
// https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntcreatefile
struct NativeUnicodeString final {
    USHORT length{};
    USHORT maximum_length{};
    PWSTR buffer{};
};

struct NativeObjectAttributes final {
    ULONG length{};
    HANDLE root_directory{};
    NativeUnicodeString* object_name{};
    ULONG attributes{};
    void* security_descriptor{};
    void* security_quality_of_service{};
};

static_assert(offsetof(NativeUnicodeString, buffer) % alignof(PWSTR) == 0U);
static_assert(offsetof(NativeObjectAttributes, root_directory) %
                      alignof(HANDLE) ==
                  0U);
static_assert(offsetof(NativeObjectAttributes, object_name) ==
              offsetof(NativeObjectAttributes, root_directory) +
                  sizeof(HANDLE));
static_assert(offsetof(NativeObjectAttributes, attributes) ==
              offsetof(NativeObjectAttributes, object_name) +
                  sizeof(NativeUnicodeString*));

struct NativeIoStatusBlock final {
    union {
        NativeNtStatus status;
        void* pointer;
    };
    ULONG_PTR information{};
};

static_assert(offsetof(NativeIoStatusBlock, information) == sizeof(void*));
static_assert(sizeof(NativeIoStatusBlock) == sizeof(void*) + sizeof(ULONG_PTR));

using NtCreateFileFunction = NativeNtStatus(NTAPI*)(
    HANDLE*, ACCESS_MASK, NativeObjectAttributes*, NativeIoStatusBlock*,
    LARGE_INTEGER*, ULONG, ULONG, ULONG, ULONG, void*, ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(NativeNtStatus);

template <typename Function>
[[nodiscard]] Function resolve_ntdll_function(
    const HMODULE module, const char* const name) noexcept {
    const auto procedure = ::GetProcAddress(module, name);
    static_assert(sizeof(Function) == sizeof(procedure));
    Function result{};
    std::memcpy(&result, &procedure, sizeof(result));
    return result;
}

struct NativeOpenApi final {
    NtCreateFileFunction create_file{};
    RtlNtStatusToDosErrorFunction status_to_dos_error{};

    [[nodiscard]] bool available() const noexcept {
        return create_file != nullptr && status_to_dos_error != nullptr;
    }
};

[[nodiscard]] const NativeOpenApi& native_open_api() noexcept {
    static const NativeOpenApi api = [] {
        const auto module = ::GetModuleHandleW(L"ntdll.dll");
        if (module == nullptr) {
            return NativeOpenApi{};
        }
        return NativeOpenApi{
            .create_file = resolve_ntdll_function<NtCreateFileFunction>(
                module, "NtCreateFile"),
            .status_to_dos_error =
                resolve_ntdll_function<RtlNtStatusToDosErrorFunction>(
                    module, "RtlNtStatusToDosError"),
        };
    }();
    return api;
}

[[nodiscard]] std::expected<ScopedHandle, FileSourceError>
open_relative_windows(const HANDLE directory,
                      const std::filesystem::path& component,
                      const bool leaf) {
    const auto& api = native_open_api();
    if (!api.available()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            windows_error(ERROR_PROC_NOT_FOUND),
            "native handle-relative file open is unavailable"));
    }
    const auto& native = component.native();
    const auto byte_length = native.size() * sizeof(wchar_t);
    if (!std::in_range<USHORT>(byte_length)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image path component is too long for a native relative open"));
    }
    auto name = NativeUnicodeString{
        .length = static_cast<USHORT>(byte_length),
        .maximum_length = static_cast<USHORT>(byte_length),
        .buffer = const_cast<PWSTR>(native.data()),
    };
    auto attributes = NativeObjectAttributes{
        .length = sizeof(NativeObjectAttributes),
        .root_directory = directory,
        .object_name = &name,
        .attributes = 0x00000040UL,  // OBJ_CASE_INSENSITIVE
        .security_descriptor = nullptr,
        .security_quality_of_service = nullptr,
    };
    NativeIoStatusBlock io_status{};
    HANDLE raw_handle{};
    constexpr ULONG file_open = 0x00000001UL;
    constexpr ULONG file_directory_file = 0x00000001UL;
    constexpr ULONG file_non_directory_file = 0x00000040UL;
    constexpr ULONG file_open_reparse_point = 0x00200000UL;
    const auto desired_access = leaf
        ? static_cast<ACCESS_MASK>(GENERIC_READ | FILE_READ_ATTRIBUTES)
        : static_cast<ACCESS_MASK>(FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                                   FILE_READ_ATTRIBUTES);
    const auto create_options = file_open_reparse_point |
        (leaf ? file_non_directory_file : file_directory_file);
    const auto status = api.create_file(
        &raw_handle,
        desired_access,
        &attributes,
        &io_status,
        nullptr,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        file_open,
        create_options,
        nullptr,
        0U);
    if (status < 0) {
        const auto native_error = api.status_to_dos_error(status);
        const auto kind = native_error == ERROR_FILE_NOT_FOUND ||
                                  native_error == ERROR_PATH_NOT_FOUND
                              ? FileSourceErrorKind::NotFound
                          : native_error == ERROR_DIRECTORY
                              ? FileSourceErrorKind::NotRegularFile
                              : FileSourceErrorKind::OpenFailed;
        return std::unexpected(file_error(
            kind,
            windows_error(native_error),
            kind == FileSourceErrorKind::NotFound
                ? "image file below directory does not exist"
            : kind == FileSourceErrorKind::NotRegularFile
                ? "image path below directory is not a regular file"
                : "unable to open image path relative to its directory handle"));
    }
    if (raw_handle == nullptr || raw_handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            {},
            "native relative open returned no image handle"));
    }
    ScopedHandle opened(raw_handle);
    if (auto inherited = make_non_inheritable(
            opened.get(), leaf ? "image file" : "image directory");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }
    return opened;
}

#else

[[nodiscard]] std::error_code posix_error(const int value) {
    return {value, std::generic_category()};
}

[[nodiscard]] std::expected<FileSnapshotIdentity, FileSourceError>
snapshot_identity_from_stat(const struct stat& information) {
    const bool directory = S_ISDIR(information.st_mode);
    if (!directory && !S_ISREG(information.st_mode)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile,
            {},
            "image snapshot path is neither a regular file nor a directory"));
    }
    if (!directory &&
        (information.st_size < 0 ||
         !std::in_range<std::uint64_t>(information.st_size))) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            {},
            "unable to determine image snapshot size"));
    }
#if defined(__APPLE__)
    const auto modified_seconds = information.st_mtimespec.tv_sec;
    const auto modified_nanoseconds = information.st_mtimespec.tv_nsec;
    const auto changed_seconds = information.st_ctimespec.tv_sec;
    const auto changed_nanoseconds = information.st_ctimespec.tv_nsec;
#else
    const auto modified_seconds = information.st_mtim.tv_sec;
    const auto modified_nanoseconds = information.st_mtim.tv_nsec;
    const auto changed_seconds = information.st_ctim.tv_sec;
    const auto changed_nanoseconds = information.st_ctim.tv_nsec;
#endif
    if (!std::in_range<std::int64_t>(modified_seconds) ||
        !std::in_range<std::uint32_t>(modified_nanoseconds) ||
        !std::in_range<std::int64_t>(changed_seconds) ||
        !std::in_range<std::uint32_t>(changed_nanoseconds)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            {},
            "image snapshot timestamps are not representable"));
    }
    return FileSnapshotIdentity{
        .device = static_cast<std::uint64_t>(information.st_dev),
        .object = static_cast<std::uint64_t>(information.st_ino),
        .size = directory ? std::uint64_t{0}
                          : static_cast<std::uint64_t>(information.st_size),
        .modified_seconds = static_cast<std::int64_t>(modified_seconds),
        .modified_nanoseconds = static_cast<std::uint32_t>(modified_nanoseconds),
        .changed_seconds = static_cast<std::int64_t>(changed_seconds),
        .changed_nanoseconds = static_cast<std::uint32_t>(changed_nanoseconds),
        .directory = directory,
    };
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

std::expected<FileDirectoryBoundary, FileSourceError>
FileDirectoryBoundary::capture(const std::filesystem::path& path) {
    return capture_impl(path, true);
}

std::expected<FileDirectoryBoundary, FileSourceError>
FileDirectoryBoundary::capture_impl(const std::filesystem::path& path,
                                    const bool follow_root_symlink) {
    if (path.empty()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::InvalidArgument,
            {},
            "image directory boundary path is empty"));
    }
    if (path_contains_nul(path)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image directory boundary path contains an embedded NUL"));
    }
#if defined(_WIN32)
    if (windows_path_has_alias(path)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image directory boundary aliases a Win32-normalized name"));
    }
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS |
        (follow_root_symlink ? 0U : FILE_FLAG_OPEN_REPARSE_POINT);
    const auto raw_handle = ::CreateFileW(
        path.c_str(),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr);
    if (raw_handle == INVALID_HANDLE_VALUE) {
        const auto native = ::GetLastError();
        const auto kind = native == ERROR_FILE_NOT_FOUND ||
                                  native == ERROR_PATH_NOT_FOUND
                              ? FileSourceErrorKind::NotFound
                              : FileSourceErrorKind::OpenFailed;
        return std::unexpected(file_error(
            kind,
            windows_error(native),
            kind == FileSourceErrorKind::NotFound
                ? "image directory boundary does not exist"
                : "unable to capture the image directory boundary"));
    }
    ScopedHandle handle(raw_handle);
    if (auto inherited = make_non_inheritable(
            handle.get(), "image directory boundary");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle.get(), &information) == FALSE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            windows_error(::GetLastError()),
            "unable to inspect the image directory boundary"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image directory boundary is a reparse point"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        ::GetFileType(handle.get()) != FILE_TYPE_DISK) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile,
            {},
            "image directory boundary is not a directory"));
    }
    auto identity = snapshot_identity_from_handle(handle.get(), information);
    if (!identity) {
        return std::unexpected(std::move(identity.error()));
    }
    return FileDirectoryBoundary(handle.release(), std::move(*identity));
#else
    int flags = O_RDONLY | O_DIRECTORY;
    if (!follow_root_symlink) {
        flags |= O_NOFOLLOW;
    }
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int raw_descriptor = ::open(path.c_str(), flags);
    if (raw_descriptor < 0) {
        const int native = errno;
        const auto kind = native == ENOENT
                              ? FileSourceErrorKind::NotFound
                          : native == ELOOP || native == ENOTDIR
                              ? FileSourceErrorKind::UnsafePath
                              : FileSourceErrorKind::OpenFailed;
        return std::unexpected(file_error(
            kind,
            posix_error(native),
            kind == FileSourceErrorKind::NotFound
                ? "image directory boundary does not exist"
            : kind == FileSourceErrorKind::UnsafePath
                ? "image directory boundary is a symbolic link or unsafe path"
                : "unable to capture the image directory boundary"));
    }
    ScopedFileDescriptor descriptor(raw_descriptor);
    if (auto inherited = make_close_on_exec(
            descriptor.get(), "image directory boundary");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }
    struct stat information {};
    if (::fstat(descriptor.get(), &information) != 0) {
        const int native = errno;
        return std::unexpected(file_error(
            FileSourceErrorKind::OpenFailed,
            posix_error(native),
            "unable to inspect the image directory boundary"));
    }
    if (!S_ISDIR(information.st_mode)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile,
            {},
            "image directory boundary is not a directory"));
    }
    auto identity = snapshot_identity_from_stat(information);
    if (!identity) {
        return std::unexpected(std::move(identity.error()));
    }
    return FileDirectoryBoundary(descriptor.release(), std::move(*identity));
#endif
}

FileDirectoryBoundary::FileDirectoryBoundary(
    const NativeHandle handle, FileSnapshotIdentity identity) noexcept
    : handle_(handle), identity_(std::move(identity)) {}

FileDirectoryBoundary::FileDirectoryBoundary(
    FileDirectoryBoundary&& other) noexcept
#if defined(_WIN32)
    : handle_(std::exchange(other.handle_, nullptr)),
#else
    : handle_(std::exchange(other.handle_, -1)),
#endif
      identity_(std::move(other.identity_)) {}

FileDirectoryBoundary& FileDirectoryBoundary::operator=(
    FileDirectoryBoundary&& other) noexcept {
    if (this != &other) {
#if defined(_WIN32)
        if (handle_ != nullptr) {
            (void)::CloseHandle(static_cast<HANDLE>(handle_));
        }
        handle_ = std::exchange(other.handle_, nullptr);
#else
        if (handle_ >= 0) {
            (void)::close(handle_);
        }
        handle_ = std::exchange(other.handle_, -1);
#endif
        identity_ = std::move(other.identity_);
    }
    return *this;
}

FileDirectoryBoundary::~FileDirectoryBoundary() {
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

const FileSnapshotIdentity& FileDirectoryBoundary::identity() const noexcept {
    return identity_;
}

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

std::expected<FileSnapshotIdentity, FileSourceError>
FileImageSource::inspect_snapshot_identity(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::unexpected(file_error(
            FileSourceErrorKind::InvalidArgument, {}, "image snapshot path is empty"));
    }
    if (path_contains_nul(path)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath, {},
            "image snapshot path contains an embedded NUL"));
    }

#if defined(_WIN32)
    if (windows_path_has_alias(path)) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath, {},
            "image snapshot path aliases a Win32-normalized or reserved name"));
    }
    const auto native_handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (native_handle == INVALID_HANDLE_VALUE) {
        const auto native = ::GetLastError();
        const auto kind = native == ERROR_FILE_NOT_FOUND || native == ERROR_PATH_NOT_FOUND
                              ? FileSourceErrorKind::NotFound
                              : FileSourceErrorKind::OpenFailed;
        return std::unexpected(file_error(
            kind, windows_error(native),
            kind == FileSourceErrorKind::NotFound
                ? "image snapshot path does not exist"
                : "unable to open image snapshot path"));
    }
    ScopedHandle handle(native_handle);
    if (auto inherited = make_non_inheritable(handle.get(), "image snapshot path");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle.get(), &information) == FALSE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            windows_error(::GetLastError()),
            "unable to inspect image snapshot path"));
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath, {},
            "image snapshot path is a reparse point"));
    }
    if (::GetFileType(handle.get()) != FILE_TYPE_DISK) {
        return std::unexpected(file_error(
            FileSourceErrorKind::NotRegularFile, {},
            "image snapshot path is not a disk file or directory"));
    }
    return snapshot_identity_from_handle(handle.get(), information);
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
        return std::unexpected(file_error(
            kind, posix_error(native),
            kind == FileSourceErrorKind::NotFound
                ? "image snapshot path does not exist"
            : kind == FileSourceErrorKind::UnsafePath
                ? "image snapshot path is a symbolic link"
                : "unable to open image snapshot path"));
    }
    ScopedFileDescriptor descriptor(native_descriptor);
    if (auto inherited = make_close_on_exec(descriptor.get(), "image snapshot path");
        !inherited) {
        return std::unexpected(std::move(inherited.error()));
    }
    struct stat information {};
    if (::fstat(descriptor.get(), &information) != 0) {
        const int native = errno;
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable, posix_error(native),
            "unable to inspect image snapshot path"));
    }
    return snapshot_identity_from_stat(information);
#endif
}

std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
FileImageSource::open_beneath(
    const std::filesystem::path& directory,
    const std::filesystem::path& relative_path,
    const FileSnapshotIdentity* const expected_directory_identity,
    const FileSnapshotIdentity* const expected_file_identity) {
    if (auto validated = validate_relative_path(relative_path); !validated) {
        return std::unexpected(std::move(validated.error()));
    }
    auto boundary = FileDirectoryBoundary::capture_impl(directory, false);
    if (!boundary) {
        return std::unexpected(std::move(boundary.error()));
    }
    if (expected_directory_identity != nullptr &&
        boundary->identity() != *expected_directory_identity) {
        return std::unexpected(file_error(
            FileSourceErrorKind::UnsafePath,
            {},
            "image base directory changed after package inventory"));
    }
    return open_beneath(*boundary, relative_path, expected_file_identity);
}

std::expected<std::shared_ptr<FileImageSource>, FileSourceError>
FileImageSource::open_beneath(
    const FileDirectoryBoundary& boundary,
    const std::filesystem::path& relative_path,
    const FileSnapshotIdentity* const expected_file_identity) {
    if (auto validated = validate_relative_path(relative_path); !validated) {
        return std::unexpected(std::move(validated.error()));
    }

    std::vector<std::filesystem::path> components;
    for (const auto& component : relative_path) {
        components.push_back(component);
    }

#if defined(_WIN32)
    std::vector<ScopedHandle> held_directories;
    auto current = static_cast<HANDLE>(boundary.handle_);
    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool leaf = index + 1U == components.size();
        auto opened = open_relative_windows(current, components[index], leaf);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (::GetFileInformationByHandle(opened->get(), &information) == FALSE) {
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
        if (!leaf) {
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
                return std::unexpected(file_error(
                    FileSourceErrorKind::UnsafePath,
                    {},
                    "image path below directory contains a non-directory component"));
            }
            current = opened->get();
            held_directories.push_back(std::move(*opened));
            continue;
        }
        if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
            ::GetFileType(opened->get()) != FILE_TYPE_DISK) {
            return std::unexpected(file_error(
                FileSourceErrorKind::NotRegularFile,
                {},
                "image path below directory is not a regular file"));
        }
        auto file_identity = snapshot_identity_from_handle(opened->get(), information);
        if (!file_identity) {
            return std::unexpected(std::move(file_identity.error()));
        }
        if (expected_file_identity != nullptr &&
            *file_identity != *expected_file_identity) {
            return std::unexpected(file_error(
                FileSourceErrorKind::UnsafePath, {},
                "image file changed after package inventory"));
        }
        LARGE_INTEGER native_size{};
        if (::GetFileSizeEx(opened->get(), &native_size) == FALSE) {
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
                opened->get(), static_cast<std::uint64_t>(native_size.QuadPart)));
            (void)opened->release();
            return std::shared_ptr<FileImageSource>(std::move(source));
        } catch (const std::bad_alloc&) {
            return std::unexpected(file_error(
                FileSourceErrorKind::OpenFailed,
                std::make_error_code(std::errc::not_enough_memory),
                "unable to allocate image source below directory"));
        }
    }
#else
    auto current = static_cast<int>(boundary.handle_);
    ScopedFileDescriptor held_directory(-1);
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
            current, components[index].c_str(), flags);
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
            held_directory = std::move(opened);
            current = held_directory.get();
            continue;
        }
        if (!S_ISREG(information.st_mode)) {
            return std::unexpected(file_error(
                FileSourceErrorKind::NotRegularFile,
                {},
                "image path below directory is not a regular file"));
        }
        auto file_identity = snapshot_identity_from_stat(information);
        if (!file_identity) {
            return std::unexpected(std::move(file_identity.error()));
        }
        if (expected_file_identity != nullptr &&
            *file_identity != *expected_file_identity) {
            return std::unexpected(file_error(
                FileSourceErrorKind::UnsafePath, {},
                "image file changed after package inventory"));
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

std::expected<FileSnapshotIdentity, FileSourceError>
FileImageSource::snapshot_identity() const {
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(
            static_cast<HANDLE>(handle_), &information) == FALSE) {
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable,
            windows_error(::GetLastError()),
            "unable to revalidate image snapshot identity"));
    }
    return snapshot_identity_from_handle(static_cast<HANDLE>(handle_), information);
#else
    struct stat information {};
    if (::fstat(handle_, &information) != 0) {
        const int native = errno;
        return std::unexpected(file_error(
            FileSourceErrorKind::SizeUnavailable, posix_error(native),
            "unable to revalidate image snapshot identity"));
    }
    return snapshot_identity_from_stat(information);
#endif
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
