// SPDX-License-Identifier: MIT
#include "file_transfer_sink.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
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
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdlib.h>
#elif defined(__linux__)
#include <sys/random.h>
#endif
#endif

namespace kairosboot::protocol {
namespace {

[[nodiscard]] FileTransferSinkError sink_error(
    const FileTransferSinkErrorKind kind,
    const int native_code,
    std::string message) {
    return {
        .kind = kind,
        .native_code = native_code,
        .message = std::move(message),
    };
}

[[nodiscard]] bool path_contains_nul(
    const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return native.find(std::filesystem::path::value_type{}) !=
           std::filesystem::path::string_type::npos;
}

#if defined(_WIN32)

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
    if (normalized == L"CON" || normalized == L"PRN" ||
        normalized == L"AUX" || normalized == L"NUL" ||
        normalized == L"CONIN$" || normalized == L"CONOUT$" ||
        normalized == L"CLOCK$") {
        return true;
    }
    return normalized.size() == 4U &&
           (normalized.starts_with(L"COM") ||
            normalized.starts_with(L"LPT")) &&
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

class ScopedHandle final {
public:
    explicit ScopedHandle(const HANDLE value = nullptr) noexcept
        : value_(value) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (value_ != nullptr) {
            (void)::CloseHandle(value_);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }

    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(value_, nullptr);
    }

private:
    HANDLE value_{};
};

[[nodiscard]] bool make_non_inheritable(const HANDLE handle) noexcept {
    return ::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0U) != FALSE;
}

[[nodiscard]] bool rename_info_ex_is_unsupported(
    const DWORD error) noexcept {
    switch (error) {
    case ERROR_INVALID_FUNCTION:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::expected<std::filesystem::path, FileTransferSinkError>
final_directory_path(const HANDLE directory) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required =
        ::GetFinalPathNameByHandleW(directory, nullptr, 0U, flags);
    if (required == 0U) {
        return std::unexpected(sink_error(
            FileTransferSinkErrorKind::ParentUnavailable,
            static_cast<int>(::GetLastError()),
            "unable to resolve the destination directory"));
    }
    try {
        std::wstring buffer(static_cast<std::size_t>(required), L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(
            directory, buffer.data(), required, flags);
        if (written == 0U || written >= required) {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::ParentUnavailable,
                static_cast<int>(::GetLastError()),
                "unable to resolve the destination directory"));
        }
        buffer.resize(static_cast<std::size_t>(written));
        return std::filesystem::path(std::move(buffer));
    } catch (const std::bad_alloc&) {
        return std::unexpected(sink_error(
            FileTransferSinkErrorKind::AllocationFailed, 0,
            "unable to allocate the resolved destination path"));
    }
}

void mark_delete_on_close(const HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    (void)::SetFileInformationByHandle(
        handle, FileDispositionInfo, &disposition, sizeof(disposition));
}

#else

class ScopedDescriptor final {
public:
    explicit ScopedDescriptor(const int value = -1) noexcept : value_(value) {}

    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

    ~ScopedDescriptor() {
        if (value_ >= 0) {
            (void)::close(value_);
        }
    }

    [[nodiscard]] int get() const noexcept { return value_; }

    [[nodiscard]] int release() noexcept {
        return std::exchange(value_, -1);
    }

private:
    int value_{-1};
};

[[nodiscard]] bool make_close_on_exec(const int descriptor) noexcept {
    const int flags = ::fcntl(descriptor, F_GETFD);
    return flags >= 0 &&
           ((flags & FD_CLOEXEC) != 0 ||
            ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0);
}

[[nodiscard]] bool fill_random(
    const std::span<unsigned char> destination,
    int& native_code) noexcept {
#if defined(__APPLE__)
    ::arc4random_buf(destination.data(), destination.size());
    native_code = 0;
    return true;
#elif defined(__linux__)
    std::size_t completed = 0;
    while (completed < destination.size()) {
        const auto received = ::getrandom(
            destination.data() + completed,
            destination.size() - completed,
            0U);
        if (received > 0) {
            completed += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        native_code = received == 0 ? EIO : errno;
        return false;
    }
    native_code = 0;
    return true;
#else
    int flags = O_RDONLY;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    ScopedDescriptor random_file(::open("/dev/urandom", flags));
    if (random_file.get() < 0 || !make_close_on_exec(random_file.get())) {
        native_code = errno;
        return false;
    }
    std::size_t completed = 0;
    while (completed < destination.size()) {
        const auto received = ::read(
            random_file.get(), destination.data() + completed,
            destination.size() - completed);
        if (received > 0) {
            completed += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        native_code = received == 0 ? EIO : errno;
        return false;
    }
    native_code = 0;
    return true;
#endif
}

#endif

[[nodiscard]] std::string encode_random_name(
    const std::array<unsigned char, 16>& random) {
    constexpr std::string_view hexadecimal = "0123456789abcdef";
    std::string result = ".kairosboot-receive-";
    result.reserve(result.size() + random.size() * 2U + 4U);
    for (const auto byte : random) {
        result.push_back(hexadecimal[(byte >> 4U) & 0x0fU]);
        result.push_back(hexadecimal[byte & 0x0fU]);
    }
    result += ".tmp";
    return result;
}

}  // namespace

std::expected<std::shared_ptr<FileTransferSink>, FileTransferSinkError>
FileTransferSink::create(const std::filesystem::path& requested_destination) {
    try {
        if (requested_destination.empty() ||
            requested_destination.filename().empty() ||
            requested_destination.filename() == "." ||
            requested_destination.filename() == "..") {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::InvalidArgument, 0,
                "destination path must name a file"));
        }
        if (path_contains_nul(requested_destination)) {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::UnsafePath, 0,
                "destination path contains an embedded NUL"));
        }

        std::error_code absolute_error;
        const auto destination =
            std::filesystem::absolute(requested_destination, absolute_error);
        if (absolute_error || destination.empty()) {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::ParentUnavailable,
                absolute_error.value(),
                "unable to resolve the destination path"));
        }
#if defined(_WIN32)
        if (windows_path_has_alias(destination)) {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::UnsafePath, 0,
                "destination path aliases a Win32-normalized or reserved name"));
        }
#endif

        auto directory_path = destination.parent_path();
        if (directory_path.empty()) {
            directory_path = std::filesystem::path{"."};
        }
        const auto destination_leaf = destination.filename();

#if defined(_WIN32)
        SECURITY_ATTRIBUTES security{
            .nLength = sizeof(SECURITY_ATTRIBUTES),
            .lpSecurityDescriptor = nullptr,
            .bInheritHandle = FALSE,
        };
        const auto raw_directory = ::CreateFileW(
            directory_path.c_str(),
            FILE_LIST_DIRECTORY | FILE_ADD_FILE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            &security,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (raw_directory == INVALID_HANDLE_VALUE) {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::ParentUnavailable,
                static_cast<int>(::GetLastError()),
                "destination parent is not an accessible directory"));
        }
        ScopedHandle directory(raw_directory);
        if (!make_non_inheritable(directory.get())) {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::ParentUnavailable,
                static_cast<int>(::GetLastError()),
                "unable to make the destination directory handle non-inheritable"));
        }
        auto final_parent = final_directory_path(directory.get());
        if (!final_parent) {
            return std::unexpected(std::move(final_parent.error()));
        }

        for (std::uint32_t attempt = 0; attempt < 32U; ++attempt) {
            std::array<unsigned char, 16> random{};
            const auto random_status = ::BCryptGenRandom(
                nullptr, random.data(), static_cast<ULONG>(random.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            if (!BCRYPT_SUCCESS(random_status)) {
                return std::unexpected(sink_error(
                    FileTransferSinkErrorKind::CreateFailed,
                    static_cast<int>(random_status),
                    "unable to generate a private receive file name"));
            }
            const auto temporary_leaf =
                std::filesystem::path(encode_random_name(random));
            const auto temporary_path = *final_parent / temporary_leaf;
            const auto raw_file = ::CreateFileW(
                temporary_path.c_str(),
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                &security,
                CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
                nullptr);
            if (raw_file == INVALID_HANDLE_VALUE) {
                const DWORD native = ::GetLastError();
                if (native == ERROR_FILE_EXISTS ||
                    native == ERROR_ALREADY_EXISTS) {
                    continue;
                }
                return std::unexpected(sink_error(
                    FileTransferSinkErrorKind::CreateFailed,
                    static_cast<int>(native),
                    "unable to create a private receive file"));
            }
            ScopedHandle file(raw_file);
            if (!make_non_inheritable(file.get())) {
                const int native = static_cast<int>(::GetLastError());
                mark_delete_on_close(file.get());
                return std::unexpected(sink_error(
                    FileTransferSinkErrorKind::CreateFailed,
                    native,
                    "unable to make the receive file handle non-inheritable"));
            }

            std::unique_ptr<FileTransferSink> owned;
            try {
                owned.reset(new FileTransferSink(
                    file.get(), directory.get(), destination_leaf,
                    temporary_leaf, temporary_path));
            } catch (...) {
                mark_delete_on_close(file.get());
                throw;
            }
            (void)file.release();
            (void)directory.release();
            return std::shared_ptr<FileTransferSink>(std::move(owned));
        }
        return std::unexpected(sink_error(
            FileTransferSinkErrorKind::CreateFailed, 0,
            "unable to allocate a unique private receive file"));
#else
        int directory_flags = O_RDONLY;
#if defined(O_DIRECTORY)
        directory_flags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
        directory_flags |= O_CLOEXEC;
#endif
        ScopedDescriptor directory(
            ::open(directory_path.c_str(), directory_flags));
        if (directory.get() < 0) {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::ParentUnavailable, errno,
                "destination parent is not an accessible directory"));
        }
        struct stat directory_status {};
        if (::fstat(directory.get(), &directory_status) != 0 ||
            !S_ISDIR(directory_status.st_mode) ||
            !make_close_on_exec(directory.get())) {
            const int native = errno;
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::ParentUnavailable, native,
                "destination parent is not a usable directory"));
        }

        for (std::uint32_t attempt = 0; attempt < 32U; ++attempt) {
            std::array<unsigned char, 16> random{};
            int random_error = 0;
            if (!fill_random(random, random_error)) {
                return std::unexpected(sink_error(
                    FileTransferSinkErrorKind::CreateFailed, random_error,
                    "unable to generate a private receive file name"));
            }
            const auto temporary_leaf =
                std::filesystem::path(encode_random_name(random));
            int file_flags = O_CREAT | O_EXCL | O_WRONLY;
#if defined(O_CLOEXEC)
            file_flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
            file_flags |= O_NOFOLLOW;
#endif
            const int raw_file = ::openat(
                directory.get(), temporary_leaf.c_str(), file_flags,
                S_IRUSR | S_IWUSR);
            if (raw_file < 0) {
                if (errno == EEXIST) {
                    continue;
                }
                return std::unexpected(sink_error(
                    FileTransferSinkErrorKind::CreateFailed, errno,
                    "unable to create a private receive file"));
            }
            ScopedDescriptor file(raw_file);
            if (!make_close_on_exec(file.get())) {
                const int native = errno;
                (void)::unlinkat(
                    directory.get(), temporary_leaf.c_str(), 0);
                return std::unexpected(sink_error(
                    FileTransferSinkErrorKind::CreateFailed, native,
                    "unable to make the receive file close-on-exec"));
            }

            std::unique_ptr<FileTransferSink> owned;
            try {
                owned.reset(new FileTransferSink(
                    file.get(), directory.get(), destination_leaf,
                    temporary_leaf, {}));
            } catch (...) {
                (void)::unlinkat(
                    directory.get(), temporary_leaf.c_str(), 0);
                throw;
            }
            (void)file.release();
            (void)directory.release();
            return std::shared_ptr<FileTransferSink>(std::move(owned));
        }
        return std::unexpected(sink_error(
            FileTransferSinkErrorKind::CreateFailed, 0,
            "unable to allocate a unique private receive file"));
#endif
    } catch (const std::bad_alloc&) {
        return std::unexpected(sink_error(
            FileTransferSinkErrorKind::AllocationFailed, 0,
            "unable to allocate the file receive sink"));
    } catch (...) {
        return std::unexpected(sink_error(
            FileTransferSinkErrorKind::CreateFailed, 0,
            "unexpected failure while creating the file receive sink"));
    }
}

FileTransferSink::FileTransferSink(
    const NativeHandle file,
    const NativeHandle directory,
    std::filesystem::path destination_leaf,
    std::filesystem::path temporary_leaf,
    std::filesystem::path temporary_path) noexcept
    : file_(file),
      directory_(directory),
      destination_leaf_(std::move(destination_leaf)),
      temporary_leaf_(std::move(temporary_leaf)),
      temporary_path_(std::move(temporary_path)) {}

FileTransferSink::~FileTransferSink() {
    try {
        std::scoped_lock lock(mutex_);
        cleanup_locked();
    } catch (...) {
        // A sink must never leak a native handle even if locking itself fails.
#if defined(_WIN32)
        if (file_ != nullptr) {
            mark_delete_on_close(static_cast<HANDLE>(file_));
            (void)::CloseHandle(static_cast<HANDLE>(file_));
            file_ = nullptr;
        }
        if (directory_ != nullptr) {
            (void)::CloseHandle(static_cast<HANDLE>(directory_));
            directory_ = nullptr;
        }
#else
        if (file_ >= 0) {
            (void)::close(file_);
            file_ = -1;
        }
        if (directory_ >= 0) {
            if (!temporary_leaf_.empty()) {
                (void)::unlinkat(directory_, temporary_leaf_.c_str(), 0);
            }
            (void)::close(directory_);
            directory_ = -1;
        }
#endif
    }
}

TransferResult FileTransferSink::write(
    const std::uint64_t offset,
    const std::span<const std::byte> source) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (state_ != State::Open) {
            return {
                .status = TransportStatus::IoError,
                .transferred = 0,
                .certainty = TransferCertainty::NotTransferred,
                .truncated = false,
                .detail = "file receive sink is not open",
                .native_code = 0,
            };
        }
        if (offset != committed_) {
            return fail_write_locked(
                0, 0, "file receive sink offset is not sequential");
        }
        if (source.size() >
            std::numeric_limits<std::uint64_t>::max() - offset) {
            return fail_write_locked(
                0, 0, "file receive sink offset overflows 64 bits");
        }
        if (source.empty()) {
            return {
                .status = TransportStatus::Ok,
                .transferred = 0,
                .certainty = TransferCertainty::FullyTransferred,
                .truncated = false,
                .detail = {},
                .native_code = 0,
            };
        }

#if defined(_WIN32)
        if (offset >
            static_cast<std::uint64_t>(
                std::numeric_limits<LONGLONG>::max())) {
            return fail_write_locked(
                0, ERROR_ARITHMETIC_OVERFLOW,
                "file receive sink offset exceeds the platform limit");
        }
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (::SetFilePointerEx(
                static_cast<HANDLE>(file_), position, nullptr, FILE_BEGIN) ==
            FALSE) {
            return fail_write_locked(
                0, static_cast<int>(::GetLastError()),
                "unable to seek the private receive file");
        }
        std::size_t completed = 0;
        while (completed < source.size()) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                source.size() - completed,
                static_cast<std::size_t>(
                    std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (::WriteFile(
                    static_cast<HANDLE>(file_), source.data() + completed,
                    chunk, &written, nullptr) == FALSE) {
                return fail_write_locked(
                    completed, static_cast<int>(::GetLastError()),
                    "unable to write the private receive file");
            }
            if (written == 0U) {
                return fail_write_locked(
                    completed, ERROR_WRITE_FAULT,
                    "private receive file write made no progress");
            }
            completed += static_cast<std::size_t>(written);
        }
#else
        if (offset >
            static_cast<std::uint64_t>(
                std::numeric_limits<off_t>::max())) {
            return fail_write_locked(
                0, EOVERFLOW,
                "file receive sink offset exceeds the platform limit");
        }
        std::size_t completed = 0;
        while (completed < source.size()) {
            const std::uint64_t current = offset + completed;
            if (current >
                static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max())) {
                return fail_write_locked(
                    completed, EOVERFLOW,
                    "file receive sink offset exceeds the platform limit");
            }
            const auto requested = std::min<std::size_t>(
                source.size() - completed,
                static_cast<std::size_t>(
                    std::numeric_limits<ssize_t>::max()));
            const auto written = ::pwrite(
                file_, source.data() + completed, requested,
                static_cast<off_t>(current));
            if (written > 0) {
                completed += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return fail_write_locked(
                completed, written == 0 ? EIO : errno,
                written == 0
                    ? "private receive file write made no progress"
                    : "unable to write the private receive file");
        }
#endif
        committed_ += source.size();
        return {
            .status = TransportStatus::Ok,
            .transferred = source.size(),
            .certainty = TransferCertainty::FullyTransferred,
            .truncated = false,
            .detail = {},
            .native_code = 0,
        };
    } catch (...) {
        return {
            .status = TransportStatus::IoError,
            .transferred = 0,
            .certainty = TransferCertainty::NotTransferred,
            .truncated = false,
            .detail = "unexpected file receive sink write failure",
            .native_code = 0,
        };
    }
}

std::expected<void, FileTransferSinkError> FileTransferSink::seal(
    const std::uint64_t expected_size) {
    try {
        std::scoped_lock lock(mutex_);
        if (state_ == State::Sealed) {
            if (expected_size == committed_) {
                return {};
            }
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::InvalidState, 0,
                "sealed file receive sink size does not match"));
        }
        if (state_ != State::Open) {
            return std::unexpected(sink_error(
                FileTransferSinkErrorKind::InvalidState, 0,
                "file receive sink is no longer open"));
        }
        if (expected_size != committed_) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::Incomplete, 0,
                "received byte count does not match the expected size");
        }

#if defined(_WIN32)
        if (::FlushFileBuffers(static_cast<HANDLE>(file_)) == FALSE) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::SyncFailed,
                static_cast<int>(::GetLastError()),
                "unable to synchronize the private receive file");
        }
        LARGE_INTEGER actual_size{};
        if (::GetFileSizeEx(static_cast<HANDLE>(file_), &actual_size) == FALSE) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::Incomplete,
                static_cast<int>(::GetLastError()),
                "unable to determine the private receive file size");
        }
        if (actual_size.QuadPart < 0 ||
            static_cast<std::uint64_t>(actual_size.QuadPart) != committed_) {
            const int native = actual_size.QuadPart < 0
                ? ERROR_FILE_INVALID
                : 0;
            return fail_seal_locked(
                FileTransferSinkErrorKind::Incomplete, native,
                "private receive file size changed before publication");
        }

        const auto& destination_name = destination_leaf_.native();
        if (destination_name.size() >
            static_cast<std::size_t>(
                std::numeric_limits<DWORD>::max() / sizeof(wchar_t))) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::InvalidArgument,
                ERROR_FILENAME_EXCED_RANGE,
                "destination file name exceeds the platform limit");
        }
        const std::size_t name_bytes =
            destination_name.size() * sizeof(wchar_t);
        constexpr std::size_t rename_base = sizeof(FILE_RENAME_INFO);
        if (name_bytes >
                std::numeric_limits<std::size_t>::max() - rename_base ||
            name_bytes + rename_base >
                static_cast<std::size_t>(
                    std::numeric_limits<DWORD>::max())) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::AllocationFailed,
                ERROR_ARITHMETIC_OVERFLOW,
                "destination rename information is too large");
        }
        std::vector<std::byte> rename_buffer(
            rename_base + name_bytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(
            rename_buffer.data());
        constexpr DWORD rename_replace_if_exists = 0x00000001U;
        constexpr DWORD rename_posix_semantics = 0x00000002U;
        rename->Flags = rename_replace_if_exists | rename_posix_semantics;
        // A simple name with no RootDirectory renames the already-open file
        // within its current parent. This retains handle-relative semantics
        // without relying on the Win32 wrapper's inconsistent forwarding of a
        // non-null RootDirectory.
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(name_bytes);
        std::memcpy(rename->FileName, destination_name.data(), name_bytes);
        const auto rename_size = static_cast<DWORD>(rename_buffer.size());
        BOOL renamed = ::SetFileInformationByHandle(
            static_cast<HANDLE>(file_), FileRenameInfoEx, rename, rename_size);
        DWORD rename_error = renamed == FALSE ? ::GetLastError() : ERROR_SUCCESS;
        if (renamed == FALSE && rename_info_ex_is_unsupported(rename_error)) {
            rename->ReplaceIfExists = TRUE;
            renamed = ::SetFileInformationByHandle(
                static_cast<HANDLE>(file_), FileRenameInfo, rename, rename_size);
            if (renamed == FALSE) {
                rename_error = ::GetLastError();
            }
        }
        if (renamed == FALSE) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::PublishFailed,
                static_cast<int>(rename_error),
                "unable to atomically publish the received file");
        }
#else
        if (::fsync(file_) != 0) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::SyncFailed, errno,
                "unable to synchronize the private receive file");
        }
        struct stat actual_status {};
        if (::fstat(file_, &actual_status) != 0) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::Incomplete, errno,
                "unable to determine the private receive file size");
        }
        if (!S_ISREG(actual_status.st_mode) || actual_status.st_size < 0 ||
            static_cast<std::uint64_t>(actual_status.st_size) != committed_) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::Incomplete, 0,
                "private receive file size changed before publication");
        }
        if (::renameat(
                directory_, temporary_leaf_.c_str(), directory_,
                destination_leaf_.c_str()) != 0) {
            return fail_seal_locked(
                FileTransferSinkErrorKind::PublishFailed, errno,
                "unable to atomically publish the received file");
        }
#endif
        state_ = State::Sealed;
        temporary_leaf_.clear();
        temporary_path_.clear();
        close_after_publish_locked();
        return {};
    } catch (const std::bad_alloc&) {
        try {
            std::scoped_lock lock(mutex_);
            state_ = State::Failed;
            cleanup_locked();
        } catch (...) {
        }
        return std::unexpected(FileTransferSinkError{
            .kind = FileTransferSinkErrorKind::AllocationFailed,
            .native_code = 0,
            .message = {},
        });
    } catch (...) {
        try {
            std::scoped_lock lock(mutex_);
            state_ = State::Failed;
            cleanup_locked();
        } catch (...) {
        }
        return std::unexpected(FileTransferSinkError{
            .kind = FileTransferSinkErrorKind::PublishFailed,
            .native_code = 0,
            .message = {},
        });
    }
}

void FileTransferSink::discard() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (state_ == State::Sealed || state_ == State::Discarded) {
            return;
        }
        cleanup_locked();
        state_ = State::Discarded;
    } catch (...) {
        // The destructor remains the final cleanup backstop.
    }
}

std::uint64_t FileTransferSink::bytes_written() const {
    std::scoped_lock lock(mutex_);
    return committed_;
}

bool FileTransferSink::is_sealed() const {
    std::scoped_lock lock(mutex_);
    return state_ == State::Sealed;
}

void FileTransferSink::cleanup_locked() noexcept {
#if defined(_WIN32)
    if (file_ != nullptr) {
        mark_delete_on_close(static_cast<HANDLE>(file_));
        (void)::CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    if (!temporary_path_.empty()) {
        (void)::DeleteFileW(temporary_path_.c_str());
    }
    if (directory_ != nullptr) {
        (void)::CloseHandle(static_cast<HANDLE>(directory_));
        directory_ = nullptr;
    }
#else
    if (file_ >= 0) {
        (void)::close(file_);
        file_ = -1;
    }
    if (directory_ >= 0 && !temporary_leaf_.empty()) {
        (void)::unlinkat(directory_, temporary_leaf_.c_str(), 0);
    }
    if (directory_ >= 0) {
        (void)::close(directory_);
        directory_ = -1;
    }
#endif
    temporary_leaf_.clear();
    temporary_path_.clear();
}

void FileTransferSink::close_after_publish_locked() noexcept {
#if defined(_WIN32)
    if (file_ != nullptr) {
        (void)::CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
    if (directory_ != nullptr) {
        (void)::CloseHandle(static_cast<HANDLE>(directory_));
        directory_ = nullptr;
    }
#else
    if (file_ >= 0) {
        (void)::close(file_);
        file_ = -1;
    }
    if (directory_ >= 0) {
        (void)::close(directory_);
        directory_ = -1;
    }
#endif
}

TransferResult FileTransferSink::fail_write_locked(
    const std::size_t transferred,
    const int native_code,
    const char* const detail) noexcept {
    state_ = State::Failed;
    cleanup_locked();
    try {
        return {
            .status = TransportStatus::IoError,
            .transferred = transferred,
            .certainty = transferred == 0
                ? TransferCertainty::NotTransferred
                : TransferCertainty::PartialOrUnknown,
            .truncated = false,
            .detail = detail,
            .native_code = native_code,
        };
    } catch (...) {
        return {
            .status = TransportStatus::IoError,
            .transferred = transferred,
            .certainty = transferred == 0
                ? TransferCertainty::NotTransferred
                : TransferCertainty::PartialOrUnknown,
            .truncated = false,
            .detail = {},
            .native_code = native_code,
        };
    }
}

std::unexpected<FileTransferSinkError> FileTransferSink::fail_seal_locked(
    const FileTransferSinkErrorKind kind,
    const int native_code,
    std::string message) {
    state_ = State::Failed;
    cleanup_locked();
    return std::unexpected(sink_error(kind, native_code, std::move(message)));
}

}  // namespace kairosboot::protocol
