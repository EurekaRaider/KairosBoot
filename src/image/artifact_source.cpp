// SPDX-License-Identifier: MIT
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif

#include "artifact_source.hpp"

#include "file_source.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <span>
#include <system_error>
#include <thread>
#include <type_traits>
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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kairosboot::image {
namespace {

using Clock = std::chrono::steady_clock;
inline constexpr std::size_t kSpoolBufferSize = 64U * 1024U;
inline constexpr std::size_t kMaximumEocdSearch = 65'557U;
inline constexpr std::uint32_t kLocalFileHeaderSignature = 0x04034b50U;
inline constexpr std::uint32_t kEocdSignature = 0x06054b50U;
inline constexpr std::uint32_t kZip64EocdSignature = 0x06064b50U;
inline constexpr std::uint32_t kZip64LocatorSignature = 0x07064b50U;

#if defined(_WIN32)
class ScopedWindowsHandle final {
    public:
    explicit ScopedWindowsHandle(const HANDLE handle) noexcept : handle_(handle) {}
    ScopedWindowsHandle(const ScopedWindowsHandle&) = delete;
    ScopedWindowsHandle& operator=(const ScopedWindowsHandle&) = delete;
    ~ScopedWindowsHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
        }
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, INVALID_HANDLE_VALUE);
    }

    private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};
#else
class ScopedPosixDescriptor final {
    public:
    explicit ScopedPosixDescriptor(const int descriptor) noexcept
        : descriptor_(descriptor) {}
    ScopedPosixDescriptor(const ScopedPosixDescriptor&) = delete;
    ScopedPosixDescriptor& operator=(const ScopedPosixDescriptor&) = delete;
    ~ScopedPosixDescriptor() {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
        }
    }
    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    private:
    int descriptor_{-1};
};
#endif

[[nodiscard]] ArtifactSourceError make_error(const ArtifactSourceErrorKind kind,
                                             std::string message,
                                             const int native_code = 0) {
    return {.kind = kind, .native_code = native_code, .message = std::move(message)};
}

static_assert(std::is_nothrow_move_assignable_v<ArtifactSourceError>);

[[nodiscard]] bool path_contains_nul(
    const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return native.find(std::filesystem::path::value_type{}) !=
           std::filesystem::path::string_type::npos;
}

#if defined(_WIN32)
[[nodiscard]] bool windows_reserved_path_component(
    const std::wstring_view component) noexcept {
    const auto stem = component.substr(0U, component.find(L'.'));
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
            windows_reserved_path_component(native)) {
            return true;
        }
    }
    return false;
}
#endif

[[nodiscard]] ArtifactSourceError from_file_error(const FileSourceError& error) {
    auto kind = ArtifactSourceErrorKind::Io;
    if (error.kind == FileSourceErrorKind::NotFound) {
        kind = ArtifactSourceErrorKind::NotFound;
    } else if (error.kind == FileSourceErrorKind::UnsafePath ||
               error.kind == FileSourceErrorKind::NotRegularFile) {
        kind = ArtifactSourceErrorKind::UnsafePath;
    }
    return make_error(kind, error.message, error.native_code);
}

class Budget final {
    public:
    Budget(const std::stop_token cancellation, const Clock::time_point deadline)
        : cancellation_(cancellation), deadline_(deadline) {}

    [[nodiscard]] std::optional<ArtifactSourceError> check() const {
        if (cancellation_.stop_requested()) {
            return make_error(ArtifactSourceErrorKind::Cancelled,
                              "artifact materialization was cancelled");
        }
        if (Clock::now() >= deadline_) {
            return make_error(ArtifactSourceErrorKind::TimedOut,
                              "artifact materialization exceeded its time budget");
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::stop_token& cancellation() const noexcept {
        return cancellation_;
    }

    [[nodiscard]] Clock::time_point deadline() const noexcept { return deadline_; }

    private:
    std::stop_token cancellation_;
    Clock::time_point deadline_;
};

[[nodiscard]] Clock::time_point
deadline_for(const ArtifactSourceLimits& limits) noexcept {
    if (limits.max_elapsed == std::chrono::milliseconds::max()) {
        return Clock::time_point::max();
    }
    const auto now = Clock::now();
    const auto remaining = Clock::time_point::max() - now;
    if (limits.max_elapsed >=
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)) {
        return Clock::time_point::max();
    }
    return now + std::chrono::duration_cast<Clock::duration>(limits.max_elapsed);
}

[[nodiscard]] std::uint16_t read_u16(const std::byte* bytes) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[0]) |
                                      (std::to_integer<std::uint16_t>(bytes[1]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32(const std::byte* bytes) noexcept {
    return std::to_integer<std::uint32_t>(bytes[0]) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] std::uint64_t read_u64(const std::byte* bytes) noexcept {
    return static_cast<std::uint64_t>(read_u32(bytes)) |
           (static_cast<std::uint64_t>(read_u32(bytes + 4)) << 32U);
}

[[nodiscard]] std::expected<void, ArtifactSourceError>
read_exact(const IImageSource& source, const std::uint64_t offset,
           const std::span<std::byte> destination, const Budget& budget) {
    if (offset > source.size() || destination.size() > source.size() - offset) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::InvalidArchive,
                       "archive read lies outside the declared file size"));
    }
    std::size_t completed = 0;
    while (completed < destination.size()) {
        if (auto stopped = budget.check()) {
            return std::unexpected(std::move(*stopped));
        }
        const auto current = offset + static_cast<std::uint64_t>(completed);
        auto read = source.read_at(current, destination.subspan(completed));
        if (!read) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::Io,
                           "unable to read artifact source: " + read.error().message));
        }
        if (*read == 0U || *read > destination.size() - completed) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::InvalidArchive,
                "artifact source was truncated or returned an invalid byte count"));
        }
        completed += *read;
    }
    return {};
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first < 0x80U) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xe0U) == 0xc0U) {
            length = 2;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            length = 3;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            length = 4;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (length > value.size() - index) {
            return false;
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] bool windows_reserved_ascii_segment(
    const std::string_view segment) noexcept {
    auto stem = segment.substr(0U, segment.find('.'));
    std::array<char, 7> folded{};
    if (stem.size() > folded.size()) {
        return false;
    }
    for (std::size_t index = 0; index < stem.size(); ++index) {
        const auto character = stem[index];
        folded[index] = character >= 'a' && character <= 'z'
                            ? static_cast<char>(character - 'a' + 'A')
                            : character;
    }
    const auto normalized = std::string_view(folded.data(), stem.size());
    if (normalized == "CON" || normalized == "PRN" || normalized == "AUX" ||
        normalized == "NUL" || normalized == "CONIN$" ||
        normalized == "CONOUT$" || normalized == "CLOCK$") {
        return true;
    }
    return normalized.size() == 4U &&
           (normalized.starts_with("COM") || normalized.starts_with("LPT")) &&
           normalized[3] >= '1' && normalized[3] <= '9';
}

struct ValidatedName final {
    std::string path;
    bool directory{};
};

[[nodiscard]] std::expected<ValidatedName, ArtifactSourceError>
validate_name(std::string_view raw, const std::uint32_t maximum,
              const bool allow_directory) {
    if (raw.empty()) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::UnsafePath,
                                          "artifact entry name is empty"));
    }
    if (raw.size() > maximum) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::LimitExceeded,
                       "artifact entry name exceeds the configured byte limit"));
    }
    if (!valid_utf8(raw)) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::UnsafePath,
                                          "artifact entry name is not valid UTF-8"));
    }
    if (std::ranges::any_of(raw, [](const char value) {
            return static_cast<unsigned char>(value) >= 0x80U;
        })) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::UnsupportedFeature,
                       "non-ASCII artifact names are rejected by the "
                       "cross-platform collision policy"));
    }
    if (raw.front() == '/' || raw.front() == '\\' || raw.starts_with("//") ||
        raw.starts_with("\\\\")) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::UnsafePath,
                       "absolute and UNC artifact entry names are forbidden"));
    }
    if (raw.find('\\') != std::string_view::npos ||
        raw.find(':') != std::string_view::npos ||
        raw.find('\0') != std::string_view::npos) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::UnsafePath,
                                          "artifact entry contains a forbidden "
                                          "separator, drive, stream, or NUL"));
    }

    const bool directory = raw.back() == '/';
    if (directory && !allow_directory) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::UnsafePath,
                       "a file artifact entry cannot end with a separator"));
    }
    if (directory) {
        raw.remove_suffix(1);
    }
    if (raw.empty()) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::UnsafePath,
                       "archive root directory entries are forbidden"));
    }

    std::size_t segment_start = 0;
    while (segment_start <= raw.size()) {
        const auto separator = raw.find('/', segment_start);
        const auto segment_end =
            separator == std::string_view::npos ? raw.size() : separator;
        const auto segment = raw.substr(segment_start, segment_end - segment_start);
        if (segment.empty() || segment == "." || segment == "..") {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::UnsafePath,
                "artifact entry contains an empty, dot, or dot-dot segment"));
        }
        if (segment.back() == '.' || segment.back() == ' ' ||
            windows_reserved_ascii_segment(segment)) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::UnsafePath,
                "artifact entry aliases a Win32-normalized or reserved name"));
        }
        for (const auto character : segment) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20U || byte == 0x7fU) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::UnsafePath,
                               "artifact entry contains a control character"));
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        segment_start = separator + 1U;
    }
    return ValidatedName{std::string(raw), directory};
}

[[nodiscard]] std::string ascii_fold(std::string_view path) {
    std::string folded(path);
    for (auto& character : folded) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return folded;
}

class SpoolImageSource final : public IImageSource {
    public:
#if defined(_WIN32)
    using NativeHandle = HANDLE;
#else
    using NativeHandle = int;
#endif

    [[nodiscard]] static std::expected<std::unique_ptr<SpoolImageSource>,
                                       ArtifactSourceError>
    create(const std::filesystem::path& requested_directory) {
        if (path_contains_nul(requested_directory)) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsafePath,
                           "temporary artifact directory contains an embedded NUL"));
        }
        std::error_code filesystem_error;
        auto directory = requested_directory;
        if (directory.empty()) {
            directory = std::filesystem::temp_directory_path(filesystem_error);
            if (filesystem_error) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::Io,
                               "unable to locate the temporary directory",
                               filesystem_error.value()));
            }
        }
        if (path_contains_nul(directory)) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsafePath,
                           "temporary artifact directory contains an embedded NUL"));
        }

#if defined(_WIN32)
        SECURITY_ATTRIBUTES security{
            .nLength = sizeof(SECURITY_ATTRIBUTES),
            .lpSecurityDescriptor = nullptr,
            .bInheritHandle = FALSE,
        };
        constexpr std::wstring_view hexadecimal = L"0123456789abcdef";
        for (std::uint32_t attempt = 0; attempt < 16U; ++attempt) {
            // rand_s is backed by the Windows OS CSPRNG. CREATE_NEW below is
            // still authoritative if a candidate was pre-created.
            std::array<unsigned int, 4> random_words{};
            for (auto& word : random_words) {
                const auto random_error = ::rand_s(&word);
                if (random_error != 0) {
                    return std::unexpected(make_error(
                        ArtifactSourceErrorKind::Io,
                        "unable to generate a private artifact spool name",
                        static_cast<int>(random_error)));
                }
            }
            std::wstring suffix;
            suffix.reserve(random_words.size() * 8U);
            for (const auto word : random_words) {
                for (int shift = 28; shift >= 0; shift -= 4) {
                    suffix.push_back(hexadecimal[static_cast<std::size_t>(
                        (word >> static_cast<unsigned int>(shift)) & 0x0fU)]);
                }
            }
            const auto candidate =
                directory / (L"kairosboot-spool-" + suffix + L".tmp");
            const auto handle =
                ::CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0U, &security, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE |
                                  FILE_FLAG_OVERLAPPED,
                              nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                ScopedWindowsHandle owned(handle);
                if (::SetHandleInformation(owned.get(), HANDLE_FLAG_INHERIT, 0U) ==
                    FALSE) {
                    return std::unexpected(make_error(
                        ArtifactSourceErrorKind::Io,
                        "unable to make the temporary artifact spool private",
                        static_cast<int>(::GetLastError())));
                }
                auto source = std::unique_ptr<SpoolImageSource>(
                    new SpoolImageSource(owned.get()));
                (void)owned.release();
                return source;
            }
            const auto native = ::GetLastError();
            if (native != ERROR_FILE_EXISTS && native != ERROR_ALREADY_EXISTS) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::Io,
                               "unable to create a private temporary artifact spool",
                               static_cast<int>(native)));
            }
        }
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::Io,
                       "unable to allocate a unique temporary artifact spool"));
#else
        auto pattern = (directory / "kairosboot-spool-XXXXXX").native();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        int descriptor_value = -1;
#if defined(__linux__) && defined(O_CLOEXEC)
        descriptor_value = ::mkostemp(writable.data(), O_CLOEXEC);
#else
        descriptor_value = ::mkstemp(writable.data());
#endif
        if (descriptor_value < 0) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::Io,
                "unable to create a private temporary artifact spool", errno));
        }
        ScopedPosixDescriptor descriptor(descriptor_value);
#if !defined(__linux__) || !defined(O_CLOEXEC)
        const int descriptor_flags = ::fcntl(descriptor.get(), F_GETFD);
        if (descriptor_flags < 0 ||
            ::fcntl(descriptor.get(), F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
            const int native = errno;
            (void)::unlink(writable.data());
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::Io,
                "unable to make the temporary artifact spool close-on-exec", native));
        }
#endif
        if (::fchmod(descriptor.get(), S_IRUSR | S_IWUSR) != 0) {
            const int native = errno;
            (void)::unlink(writable.data());
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::Io,
                "unable to restrict temporary artifact spool permissions", native));
        }
        if (::unlink(writable.data()) != 0) {
            const int native = errno;
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::Io,
                "unable to unlink the private temporary artifact spool", native));
        }
        auto source = std::unique_ptr<SpoolImageSource>(
            new SpoolImageSource(descriptor.get()));
        (void)descriptor.release();
        return source;
#endif
    }

    SpoolImageSource(const SpoolImageSource&) = delete;
    SpoolImageSource& operator=(const SpoolImageSource&) = delete;

    ~SpoolImageSource() override {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
        }
#else
        if (handle_ >= 0) {
            (void)::close(handle_);
        }
#endif
    }

    [[nodiscard]] std::expected<void, ArtifactSourceError>
    append(const std::uint64_t offset, const std::span<const std::byte> bytes) {
        if (sealed_ || offset != size_ || bytes.size() > UINT64_MAX - size_) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::Io,
                           "artifact spool received an invalid write offset or size"));
        }

        std::size_t completed = 0;
        while (completed < bytes.size()) {
            const auto current = offset + static_cast<std::uint64_t>(completed);
#if defined(_WIN32)
            if (current >
                static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::LimitExceeded,
                               "artifact spool offset is not representable"));
            }
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(current);
            OVERLAPPED operation{};
            operation.Offset = position.LowPart;
            operation.OffsetHigh = static_cast<DWORD>(position.HighPart);
            ScopedWindowsHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (event.get() == nullptr) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::Io,
                               "unable to create an artifact spool write event",
                               static_cast<int>(::GetLastError())));
            }
            operation.hEvent = event.get();
            const auto remaining = bytes.size() - completed;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD transferred = 0;
            if (::WriteFile(handle_, bytes.data() + completed, chunk, &transferred,
                            &operation) == FALSE) {
                auto native = ::GetLastError();
                if (native == ERROR_IO_PENDING &&
                    ::GetOverlappedResult(handle_, &operation, &transferred, TRUE) !=
                        FALSE) {
                    native = ERROR_SUCCESS;
                }
                if (native != ERROR_SUCCESS) {
                    return std::unexpected(
                        make_error(ArtifactSourceErrorKind::Io,
                                   "unable to write the temporary artifact spool",
                                   static_cast<int>(native)));
                }
            }
            if (transferred == 0U || transferred > chunk) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::Io,
                               "temporary artifact spool write made no progress"));
            }
            completed += static_cast<std::size_t>(transferred);
#else
            if (!std::in_range<off_t>(current)) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::LimitExceeded,
                               "artifact spool offset is not representable"));
            }
            const auto remaining = bytes.size() - completed;
            const auto chunk = std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto transferred = ::pwrite(handle_, bytes.data() + completed, chunk,
                                              static_cast<off_t>(current));
            if (transferred < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::Io,
                               "unable to write the temporary artifact spool", errno));
            }
            if (transferred == 0) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::Io,
                               "temporary artifact spool write made no progress"));
            }
            completed += static_cast<std::size_t>(transferred);
#endif
        }
        size_ += static_cast<std::uint64_t>(bytes.size());
        return {};
    }

    void seal() noexcept { sealed_ = true; }

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError>
    read_at(const std::uint64_t offset,
            const std::span<std::byte> destination) const override {
        if (!sealed_) {
            return std::unexpected(ImageSourceError{"artifact spool is not sealed"});
        }
        if (destination.empty() || offset >= size_) {
            return std::size_t{0};
        }
        const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
            size_ - offset, static_cast<std::uint64_t>(destination.size())));
        std::size_t completed = 0;
        while (completed < amount) {
            const auto current = offset + static_cast<std::uint64_t>(completed);
#if defined(_WIN32)
            if (current >
                static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
                return std::unexpected(ImageSourceError{
                    "artifact spool read offset is not representable"});
            }
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(current);
            OVERLAPPED operation{};
            operation.Offset = position.LowPart;
            operation.OffsetHigh = static_cast<DWORD>(position.HighPart);
            ScopedWindowsHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (event.get() == nullptr) {
                return std::unexpected(
                    ImageSourceError{"unable to create an artifact spool read event"});
            }
            operation.hEvent = event.get();
            const auto remaining = amount - completed;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                remaining,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD transferred = 0;
            if (::ReadFile(handle_, destination.data() + completed, chunk, &transferred,
                           &operation) == FALSE) {
                auto native = ::GetLastError();
                if (native == ERROR_IO_PENDING &&
                    ::GetOverlappedResult(handle_, &operation, &transferred, TRUE) !=
                        FALSE) {
                    native = ERROR_SUCCESS;
                }
                if (native == ERROR_HANDLE_EOF) {
                    return completed;
                }
                if (native != ERROR_SUCCESS) {
                    return std::unexpected(
                        ImageSourceError{"unable to read the artifact spool"});
                }
            }
            if (transferred == 0U) {
                return completed;
            }
            completed += static_cast<std::size_t>(transferred);
#else
            if (!std::in_range<off_t>(current)) {
                return std::unexpected(ImageSourceError{
                    "artifact spool read offset is not representable"});
            }
            const auto transferred =
                ::pread(handle_, destination.data() + completed, amount - completed,
                        static_cast<off_t>(current));
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
            return std::unexpected(
                ImageSourceError{"unable to read the artifact spool"});
#endif
        }
        return completed;
    }

    private:
    explicit SpoolImageSource(const NativeHandle handle) noexcept : handle_(handle) {}

    NativeHandle handle_;
    std::uint64_t size_{};
    bool sealed_{};
};

class SpoolReservation final {
    public:
    SpoolReservation(std::mutex& mutex, std::uint64_t& reserved_bytes,
                     std::uint64_t& completed_bytes,
                     std::optional<std::uint64_t>& observed_capacity,
                     std::uint64_t& entry_reserved_bytes,
                     const ArtifactSourceLimits& limits) noexcept
        : mutex_(mutex), reserved_bytes_(reserved_bytes),
          completed_bytes_(completed_bytes),
          observed_capacity_(observed_capacity),
          entry_reserved_bytes_(entry_reserved_bytes), limits_(limits) {}

    SpoolReservation(const SpoolReservation&) = delete;
    SpoolReservation& operator=(const SpoolReservation&) = delete;

    ~SpoolReservation() {
        if (held_) {
            std::lock_guard lock(mutex_);
            release_locked();
        }
    }

    [[nodiscard]] std::expected<void, ArtifactSourceError>
    acquire(const std::uint64_t expected_size) {
        if (held_) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::Io,
                           "artifact spool reservation was requested more than once"));
        }
        if (expected_size > limits_.max_spool_bytes) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::LimitExceeded,
                           "artifact exceeds the configured spool byte budget"));
        }

        std::error_code filesystem_error;
        auto directory = limits_.temporary_directory;
        if (directory.empty()) {
            directory = std::filesystem::temp_directory_path(filesystem_error);
            if (filesystem_error) {
                return std::unexpected(make_error(
                    ArtifactSourceErrorKind::Io,
                    "unable to locate the temporary directory",
                    filesystem_error.value()));
            }
        }
        if (path_contains_nul(directory)) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsafePath,
                           "temporary artifact directory contains an embedded NUL"));
        }

        std::expected<std::uint64_t, std::error_code> available =
            std::unexpected(std::make_error_code(std::errc::io_error));
        if (limits_.available_space_provider) {
            available = limits_.available_space_provider(directory);
        } else {
            const auto space = std::filesystem::space(directory, filesystem_error);
            if (!filesystem_error) {
                available = space.available;
            } else {
                available = std::unexpected(filesystem_error);
            }
        }
        if (!available) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::Io,
                "unable to query temporary disk space", available.error().value()));
        }
        std::unique_lock lock(mutex_);
        if (*available > UINT64_MAX - completed_bytes_) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::LimitExceeded,
                "artifact disk-space capacity overflows uint64"));
        }
        const auto current_capacity = *available + completed_bytes_;
        if (!observed_capacity_ || current_capacity < *observed_capacity_) {
            observed_capacity_ = current_capacity;
        }
        if (expected_size > UINT64_MAX - reserved_bytes_ ||
            reserved_bytes_ + expected_size > limits_.max_spool_bytes ||
            limits_.minimum_free_space_bytes >
                UINT64_MAX - reserved_bytes_ - expected_size) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::LimitExceeded,
                "artifact batch spool reservation exceeds its configured limit"));
        }
        const auto required = reserved_bytes_ + expected_size +
                              limits_.minimum_free_space_bytes;
        if (required > *observed_capacity_) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::LimitExceeded,
                "temporary disk has insufficient batch space for the artifact spool"));
        }
        reserved_bytes_ += expected_size;
        entry_reserved_bytes_ = expected_size;
        held_ = true;
        return {};
    }

    void complete_locked() noexcept {
        if (!held_) {
            return;
        }
        completed_bytes_ += entry_reserved_bytes_;
        held_ = false;
    }

    void release_locked() noexcept {
        if (!held_) {
            return;
        }
        if (entry_reserved_bytes_ <= reserved_bytes_) {
            reserved_bytes_ -= entry_reserved_bytes_;
        } else {
            reserved_bytes_ = 0U;
        }
        entry_reserved_bytes_ = 0U;
        held_ = false;
    }

    private:
    std::mutex& mutex_;
    std::uint64_t& reserved_bytes_;
    std::uint64_t& completed_bytes_;
    std::optional<std::uint64_t>& observed_capacity_;
    std::uint64_t& entry_reserved_bytes_;
    const ArtifactSourceLimits& limits_;
    bool held_{};
};

[[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                            ArtifactSourceError>
materialize_source(const IImageSource& source, const std::uint64_t declared_size,
                   const ArtifactSourceLimits& limits, const Budget& budget,
                   SpoolReservation& reservation,
                   const ArtifactSourceOrigin origin, std::string logical_name) {
    if (declared_size > limits.max_single_entry_size ||
        declared_size > kSha256MaxInputSize) {
        return std::unexpected(make_error(
            ArtifactSourceErrorKind::LimitExceeded,
            "artifact exceeds the configured single-entry or SHA-256 size limit"));
    }
    if (auto reserved = reservation.acquire(declared_size); !reserved) {
        return std::unexpected(std::move(reserved.error()));
    }
    if (auto stopped = budget.check()) {
        return std::unexpected(std::move(*stopped));
    }
    auto spool = SpoolImageSource::create(limits.temporary_directory);
    if (!spool) {
        return std::unexpected(std::move(spool.error()));
    }

    Sha256Accumulator hash;
    std::array<std::byte, kSpoolBufferSize> buffer{};
    std::uint64_t offset = 0;
    while (offset < declared_size) {
        if (auto stopped = budget.check()) {
            return std::unexpected(std::move(*stopped));
        }
        const auto amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(declared_size - offset, buffer.size()));
        auto read = source.read_at(offset, std::span(buffer).first(amount));
        if (!read) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::Io,
                           "unable to read artifact during materialization: " +
                               read.error().message));
        }
        if (*read == 0U || *read > amount) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::Integrity,
                "artifact size changed or source returned an invalid byte count"));
        }
        const auto bytes = std::span(buffer).first(*read);
        if (auto written = (*spool)->append(offset, bytes); !written) {
            return std::unexpected(std::move(written.error()));
        }
        hash.update(bytes);
        offset += static_cast<std::uint64_t>(*read);
    }
    if (auto stopped = budget.check()) {
        return std::unexpected(std::move(*stopped));
    }
    (*spool)->seal();
    std::shared_ptr<SpoolImageSource> owned(std::move(*spool));
    std::shared_ptr<const IImageSource> immutable = owned;
    return std::make_shared<const ResolvedArtifact>(ResolvedArtifact{
        .source = std::move(immutable),
        .sha256 = hash.finish(),
        .origin = origin,
        .logical_name = std::move(logical_name),
    });
}

struct EocdMetadata final {
    std::uint64_t entry_count{};
    std::uint64_t central_size{};
    std::uint64_t central_offset{};
    bool zip64{};
};

[[nodiscard]] std::expected<EocdMetadata, ArtifactSourceError>
inspect_eocd(const IImageSource& source, const ArtifactSourceLimits& limits,
             const Budget& budget) {
    if (source.size() > limits.max_archive_size) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::LimitExceeded,
                       "ZIP archive exceeds the configured archive size limit"));
    }
    if (source.size() < 22U) {
        return std::unexpected(make_error(
            ArtifactSourceErrorKind::InvalidArchive,
            "ZIP archive is too small for an end-of-central-directory record"));
    }
    const auto tail_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(source.size(), kMaximumEocdSearch));
    std::vector<std::byte> tail(tail_size);
    const auto tail_offset = source.size() - tail_size;
    if (auto read = read_exact(source, tail_offset, tail, budget); !read) {
        return std::unexpected(std::move(read.error()));
    }

    std::optional<std::size_t> eocd_in_tail;
    for (std::size_t candidate = tail.size() - 22U;; --candidate) {
        if (read_u32(tail.data() + candidate) == kEocdSignature) {
            const auto comment = read_u16(tail.data() + candidate + 20U);
            if (candidate + 22U + comment == tail.size()) {
                eocd_in_tail = candidate;
                break;
            }
        }
        if (candidate == 0U) {
            break;
        }
    }
    if (!eocd_in_tail) {
        return std::unexpected(make_error(
            ArtifactSourceErrorKind::InvalidArchive,
            "ZIP end-of-central-directory record is missing or has trailing data"));
    }

    const auto* eocd = tail.data() + *eocd_in_tail;
    const auto disk = read_u16(eocd + 4U);
    const auto central_disk = read_u16(eocd + 6U);
    const auto entries_on_disk = read_u16(eocd + 8U);
    const auto entries_total = read_u16(eocd + 10U);
    auto central_size = static_cast<std::uint64_t>(read_u32(eocd + 12U));
    auto central_offset = static_cast<std::uint64_t>(read_u32(eocd + 16U));
    auto entry_count = static_cast<std::uint64_t>(entries_total);
    const auto eocd_absolute = tail_offset + *eocd_in_tail;
    auto central_end_limit = eocd_absolute;
    const bool needs_zip64 = entries_on_disk == UINT16_MAX ||
                             entries_total == UINT16_MAX ||
                             central_size == UINT32_MAX || central_offset == UINT32_MAX;

    if (!needs_zip64) {
        if (disk != 0U || central_disk != 0U || entries_on_disk != entries_total) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsupportedFeature,
                           "multi-disk ZIP archives are not supported"));
        }
    } else {
        if (eocd_absolute < 20U) {
            return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArchive,
                                              "ZIP64 locator is missing"));
        }
        std::array<std::byte, 20> locator{};
        if (auto read =
                read_exact(source, eocd_absolute - locator.size(), locator, budget);
            !read) {
            return std::unexpected(std::move(read.error()));
        }
        if (read_u32(locator.data()) != kZip64LocatorSignature ||
            read_u32(locator.data() + 4U) != 0U ||
            read_u32(locator.data() + 16U) != 1U) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsupportedFeature,
                           "multi-disk or malformed ZIP64 archives are not supported"));
        }
        const auto zip64_offset = read_u64(locator.data() + 8U);
        std::array<std::byte, 56> zip64{};
        if (auto read = read_exact(source, zip64_offset, zip64, budget); !read) {
            return std::unexpected(std::move(read.error()));
        }
        const auto zip64_record_size = read_u64(zip64.data() + 4U);
        const auto locator_offset = eocd_absolute - locator.size();
        if (read_u32(zip64.data()) != kZip64EocdSignature || zip64_record_size < 44U ||
            zip64_offset > locator_offset || locator_offset - zip64_offset < 12U ||
            zip64_record_size > locator_offset - zip64_offset - 12U ||
            zip64_offset + 12U + zip64_record_size != locator_offset ||
            read_u32(zip64.data() + 16U) != 0U || read_u32(zip64.data() + 20U) != 0U) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::InvalidArchive,
                           "ZIP64 end-of-central-directory record is malformed"));
        }
        const auto zip64_entries_on_disk = read_u64(zip64.data() + 24U);
        entry_count = read_u64(zip64.data() + 32U);
        central_size = read_u64(zip64.data() + 40U);
        central_offset = read_u64(zip64.data() + 48U);
        if (zip64_entries_on_disk != entry_count) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsupportedFeature,
                           "multi-disk ZIP64 archives are not supported"));
        }
        central_end_limit = zip64_offset;
    }

    if (entry_count > limits.max_entry_count) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::LimitExceeded,
                       "ZIP entry count exceeds the configured limit"));
    }
    if (central_size > limits.max_central_directory_size) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::LimitExceeded,
                       "ZIP central directory exceeds the configured size limit"));
    }
    if (central_offset > central_end_limit ||
        central_size > central_end_limit - central_offset) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::InvalidArchive,
                       "ZIP central directory lies outside the archive"));
    }
    return EocdMetadata{
        .entry_count = entry_count,
        .central_size = central_size,
        .central_offset = central_offset,
        .zip64 = needs_zip64,
    };
}

struct ArchiveReadContext final {
    const IImageSource* source{};
    const Budget* budget{};
    std::optional<ArtifactSourceError> error;
};

size_t archive_read_callback(void* opaque, const mz_uint64 offset, void* destination,
                             const size_t amount) noexcept {
    auto& context = *static_cast<ArchiveReadContext*>(opaque);
    try {
        if (context.error || context.source == nullptr || context.budget == nullptr) {
            return 0U;
        }
        if (auto stopped = context.budget->check()) {
            context.error = std::move(*stopped);
            return 0U;
        }
        if (offset > context.source->size() ||
            amount > context.source->size() - offset) {
            context.error =
                make_error(ArtifactSourceErrorKind::InvalidArchive,
                           "ZIP reader requested bytes outside the archive");
            return 0U;
        }
        auto bytes = std::span(static_cast<std::byte*>(destination), amount);
        auto read = read_exact(*context.source, offset, bytes, *context.budget);
        if (!read) {
            context.error = std::move(read.error());
            return 0U;
        }
        return amount;
    } catch (const std::bad_alloc&) {
        context.error =
            make_error(ArtifactSourceErrorKind::Io,
                       "memory allocation failed while reading ZIP metadata");
        return 0U;
    } catch (...) {
        context.error = make_error(ArtifactSourceErrorKind::Io,
                                   "unexpected failure while reading ZIP metadata");
        return 0U;
    }
}

class ZipReader final {
    public:
    ZipReader(const IImageSource& source, const Budget& budget)
        : context_{.source = &source, .budget = &budget, .error = {}} {
        mz_zip_zero_struct(&archive_);
        archive_.m_pRead = archive_read_callback;
        archive_.m_pIO_opaque = &context_;
    }

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    ~ZipReader() {
        if (initialized_) {
            (void)mz_zip_reader_end(&archive_);
        }
    }

    [[nodiscard]] std::expected<void, ArtifactSourceError>
    initialize(const std::uint64_t size) {
        if (mz_zip_reader_init(&archive_, size,
                               MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY) == MZ_FALSE) {
            if (context_.error) {
                return std::unexpected(std::move(*context_.error));
            }
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::InvalidArchive,
                std::string("unable to initialize ZIP reader: ") +
                    mz_zip_get_error_string(mz_zip_get_last_error(&archive_))));
        }
        initialized_ = true;
        return {};
    }

    [[nodiscard]] mz_zip_archive& get() noexcept { return archive_; }

    [[nodiscard]] std::optional<ArtifactSourceError> take_context_error() {
        return std::exchange(context_.error, std::nullopt);
    }

    private:
    mz_zip_archive archive_{};
    ArchiveReadContext context_{};
    bool initialized_{};
};

struct ZipEntry final {
    std::uint32_t index{};
    std::string name;
    bool directory{};
    std::uint64_t uncompressed_size{};
};

[[nodiscard]] std::expected<void, ArtifactSourceError>
validate_local_identity(ZipReader& reader, const mz_zip_archive_file_stat& stat,
                        const std::string_view exact_name) {
    auto& archive = reader.get();
    constexpr std::uint64_t local_header_size = 30U;
    const auto archive_size = mz_zip_get_archive_size(&archive);
    if (stat.m_local_header_ofs > archive_size ||
        local_header_size > archive_size - stat.m_local_header_ofs) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArchive,
                                          "ZIP local header lies outside the archive"));
    }
    std::array<std::byte, local_header_size> header{};
    if (mz_zip_read_archive_data(&archive, stat.m_local_header_ofs, header.data(),
                                 header.size()) != header.size()) {
        if (auto callback_error = reader.take_context_error()) {
            return std::unexpected(std::move(*callback_error));
        }
        return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArchive,
                                          "unable to read ZIP local header"));
    }
    if (read_u32(header.data()) != kLocalFileHeaderSignature ||
        read_u16(header.data() + 6U) != stat.m_bit_flag ||
        read_u16(header.data() + 8U) != stat.m_method) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::InvalidArchive,
                       "ZIP central and local flags or compression method differ"));
    }
    const auto name_size = static_cast<std::uint64_t>(read_u16(header.data() + 26U));
    const auto extra_size = static_cast<std::uint64_t>(read_u16(header.data() + 28U));
    if (name_size != exact_name.size()) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::InvalidArchive,
                       "ZIP central and local entry name lengths differ"));
    }
    auto cursor = stat.m_local_header_ofs + local_header_size;
    if (name_size > archive_size - cursor) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::InvalidArchive,
                       "ZIP local filename lies outside the archive"));
    }
    std::vector<std::byte> local_name(static_cast<std::size_t>(name_size));
    if (!local_name.empty() &&
        mz_zip_read_archive_data(&archive, cursor, local_name.data(),
                                 local_name.size()) != local_name.size()) {
        if (auto callback_error = reader.take_context_error()) {
            return std::unexpected(std::move(*callback_error));
        }
        return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArchive,
                                          "unable to read exact ZIP local filename"));
    }
    if (!std::equal(local_name.begin(), local_name.end(),
                    reinterpret_cast<const std::byte*>(exact_name.data()))) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArchive,
                                          "ZIP central and local entry names differ"));
    }
    cursor += name_size;
    if (extra_size > archive_size - cursor) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::InvalidArchive,
                       "ZIP local extra data lies outside the archive"));
    }
    cursor += extra_size;
    if (stat.m_comp_size > archive_size - cursor) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::InvalidArchive,
                       "ZIP compressed payload lies outside the archive"));
    }
    return {};
}

[[nodiscard]] bool
special_archive_entry(const mz_zip_archive_file_stat& stat) noexcept {
    const auto host = static_cast<std::uint8_t>(stat.m_version_made_by >> 8U);
    if (host == 3U) {
        constexpr std::uint32_t type_mask = 0170000U;
        constexpr std::uint32_t regular = 0100000U;
        constexpr std::uint32_t directory = 0040000U;
        const auto mode = stat.m_external_attr >> 16U;
        const auto type = mode & type_mask;
        if (type != 0U && type != regular && type != directory) {
            return true;
        }
        if (type == regular && stat.m_is_directory != MZ_FALSE) {
            return true;
        }
        if (type == directory && stat.m_is_directory == MZ_FALSE) {
            return true;
        }
    }
    constexpr std::uint32_t dos_volume_label = 0x08U;
    return (stat.m_external_attr & dos_volume_label) != 0U;
}

[[nodiscard]] std::expected<std::vector<ZipEntry>, ArtifactSourceError>
inventory_zip(ZipReader& reader, const EocdMetadata& eocd,
              const ArtifactSourceLimits& limits, const Budget& budget) {
    auto& archive = reader.get();
    const auto count = mz_zip_reader_get_num_files(&archive);
    if (static_cast<std::uint64_t>(count) != eocd.entry_count) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::InvalidArchive,
                       "ZIP entry count differs between parsed end record and miniz"));
    }
    // A classic EOCD may legitimately describe an archive whose individual
    // entries use ZIP64 extended-information fields. Miniz reports that hybrid
    // as ZIP64, so only the impossible opposite direction is inconsistent.
    if (eocd.zip64 && mz_zip_is_zip64(&archive) == MZ_FALSE) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArchive,
                                          "ZIP64 markers are inconsistent"));
    }

    std::vector<ZipEntry> entries;
    entries.reserve(count);
    std::set<std::string> exact_names;
    std::map<std::string, std::string> folded_names;
    std::set<std::string> files;
    std::set<std::string> folded_files;
    std::uint64_t aggregate = 0;

    for (mz_uint index = 0; index < count; ++index) {
        if (auto stopped = budget.check()) {
            return std::unexpected(std::move(*stopped));
        }
        const auto required = mz_zip_reader_get_filename(&archive, index, nullptr, 0U);
        if (required == 0U) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::InvalidArchive,
                           "unable to read an exact ZIP entry filename"));
        }
        if (required - 1U > limits.max_name_bytes ||
            required > MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::LimitExceeded,
                "ZIP entry filename exceeds the exact-name validation limit"));
        }
        std::vector<char> filename(required);
        if (mz_zip_reader_get_filename(&archive, index, filename.data(), required) !=
                required ||
            filename.back() != '\0' ||
            std::find(filename.begin(), filename.end() - 1, '\0') !=
                filename.end() - 1) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::InvalidArchive,
                "ZIP entry filename is truncated or contains an embedded NUL"));
        }
        std::string exact(filename.data(), required - 1U);
        auto validated = validate_name(exact, limits.max_name_bytes, true);
        if (!validated) {
            return std::unexpected(std::move(validated.error()));
        }

        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&archive, index, &stat) == MZ_FALSE) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::InvalidArchive,
                           "unable to read ZIP central-directory metadata"));
        }
        if (std::string_view(stat.m_filename) != exact) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::InvalidArchive,
                "miniz file stat did not preserve the exact ZIP entry name"));
        }
        if (auto local = validate_local_identity(reader, stat, exact); !local) {
            return std::unexpected(std::move(local.error()));
        }
        if ((stat.m_is_directory != MZ_FALSE) != validated->directory) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::InvalidArchive,
                           "ZIP directory metadata conflicts with its entry name"));
        }
        if (stat.m_is_encrypted != MZ_FALSE ||
            mz_zip_reader_is_file_encrypted(&archive, index) != MZ_FALSE ||
            (stat.m_bit_flag & 0x0001U) != 0U) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsupportedFeature,
                           "encrypted ZIP entries are not supported"));
        }
        constexpr std::uint16_t allowed_flags = 0x080eU;
        if ((stat.m_bit_flag & static_cast<std::uint16_t>(~allowed_flags)) != 0U ||
            stat.m_is_supported == MZ_FALSE ||
            mz_zip_reader_is_file_supported(&archive, index) == MZ_FALSE) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsupportedFeature,
                           "ZIP entry uses patching, strong encryption, or another "
                           "unsupported feature"));
        }
        if (stat.m_method != 0U && stat.m_method != 8U) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsupportedFeature,
                           "only stored and deflate ZIP entries are supported"));
        }
        if (special_archive_entry(stat)) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsafePath,
                           "ZIP symlink or special-file entries are forbidden"));
        }
        if (validated->directory &&
            (stat.m_comp_size != 0U || stat.m_uncomp_size != 0U)) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::UnsafePath,
                           "ZIP directory entries must not carry file payload bytes"));
        }
        if (stat.m_uncomp_size > limits.max_single_entry_size ||
            stat.m_uncomp_size > kSha256MaxInputSize) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::LimitExceeded,
                           "ZIP entry exceeds the configured single-entry size limit"));
        }
        if (aggregate > limits.max_total_uncompressed_size ||
            stat.m_uncomp_size > limits.max_total_uncompressed_size - aggregate) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::LimitExceeded,
                "ZIP aggregate uncompressed size exceeds the configured limit"));
        }
        aggregate += stat.m_uncomp_size;
        if (stat.m_uncomp_size != 0U) {
            if (stat.m_comp_size == 0U || limits.max_compression_ratio == 0U ||
                (stat.m_uncomp_size - 1U) / stat.m_comp_size >=
                    limits.max_compression_ratio) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::LimitExceeded,
                               "ZIP entry exceeds the configured compression ratio"));
            }
        }

        if (!exact_names.insert(validated->path).second) {
            return std::unexpected(make_error(ArtifactSourceErrorKind::UnsafePath,
                                              "ZIP contains duplicate entry names"));
        }
        const auto folded = ascii_fold(validated->path);
        if (const auto [position, inserted] =
                folded_names.emplace(folded, validated->path);
            !inserted) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::UnsafePath,
                "ZIP contains cross-platform case-folding name collisions: " +
                    position->second + " and " + validated->path));
        }
        if (!validated->directory) {
            files.insert(validated->path);
            folded_files.insert(folded);
        }

        // This performs miniz's available central/local filename, flag, CRC,
        // size, ZIP64-extra, data-descriptor, and boundary consistency checks
        // without decompressing every unselected payload.
        if (mz_zip_validate_file(&archive, index, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY) ==
            MZ_FALSE) {
            if (auto callback_error = reader.take_context_error()) {
                return std::unexpected(std::move(*callback_error));
            }
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::InvalidArchive,
                std::string("ZIP central/local metadata validation failed: ") +
                    mz_zip_get_error_string(mz_zip_get_last_error(&archive))));
        }

        entries.push_back(ZipEntry{
            .index = static_cast<std::uint32_t>(index),
            .name = std::move(validated->path),
            .directory = validated->directory,
            .uncompressed_size = stat.m_uncomp_size,
        });
    }

    for (const auto& entry : entries) {
        std::size_t separator = entry.name.find('/');
        while (separator != std::string::npos) {
            const auto prefix = entry.name.substr(0, separator);
            if (files.contains(prefix) || folded_files.contains(ascii_fold(prefix))) {
                return std::unexpected(
                    make_error(ArtifactSourceErrorKind::UnsafePath,
                               "ZIP contains a file/directory path conflict"));
            }
            separator = entry.name.find('/', separator + 1U);
        }
    }
    return entries;
}

struct ExtractContext final {
    SpoolImageSource* spool{};
    const Budget* budget{};
    Sha256Accumulator hash;
    std::uint64_t declared_size{};
    std::uint64_t written{};
    std::optional<ArtifactSourceError> error;
};

size_t extract_callback(void* opaque, const mz_uint64 offset, const void* bytes,
                        const size_t amount) noexcept {
    auto& context = *static_cast<ExtractContext*>(opaque);
    try {
        if (context.error || context.spool == nullptr || context.budget == nullptr) {
            return 0U;
        }
        if (auto stopped = context.budget->check()) {
            context.error = std::move(*stopped);
            return 0U;
        }
        if (context.written > context.declared_size || offset != context.written ||
            amount > context.declared_size - context.written) {
            context.error = make_error(
                ArtifactSourceErrorKind::Integrity,
                "ZIP extraction exceeded its declared size or wrote out of order");
            return 0U;
        }
        const auto payload = std::span(static_cast<const std::byte*>(bytes), amount);
        if (auto written = context.spool->append(offset, payload); !written) {
            context.error = std::move(written.error());
            return 0U;
        }
        context.hash.update(payload);
        context.written += static_cast<std::uint64_t>(amount);
        return amount;
    } catch (const std::bad_alloc&) {
        context.error = make_error(ArtifactSourceErrorKind::Io,
                                   "memory allocation failed during ZIP extraction");
        return 0U;
    } catch (...) {
        context.error = make_error(ArtifactSourceErrorKind::Io,
                                   "unexpected failure during ZIP extraction");
        return 0U;
    }
}

[[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                            ArtifactSourceError>
materialize_zip_entry(ZipReader& reader, const ZipEntry& entry,
                      const ArtifactSourceLimits& limits, const Budget& budget,
                      SpoolReservation& reservation) {
    if (auto reserved = reservation.acquire(entry.uncompressed_size); !reserved) {
        return std::unexpected(std::move(reserved.error()));
    }
    auto spool = SpoolImageSource::create(limits.temporary_directory);
    if (!spool) {
        return std::unexpected(std::move(spool.error()));
    }
    ExtractContext extraction{
        .spool = spool->get(),
        .budget = &budget,
        .hash = {},
        .declared_size = entry.uncompressed_size,
        .written = 0U,
        .error = {},
    };
    auto& archive = reader.get();
    if (mz_zip_reader_extract_to_callback(&archive, entry.index, extract_callback,
                                          &extraction, 0U) == MZ_FALSE) {
        if (extraction.error) {
            return std::unexpected(std::move(*extraction.error));
        }
        if (auto callback_error = reader.take_context_error()) {
            return std::unexpected(std::move(*callback_error));
        }
        const auto native = mz_zip_get_last_error(&archive);
        const auto kind = native == MZ_ZIP_CRC_CHECK_FAILED ||
                                  native == MZ_ZIP_UNEXPECTED_DECOMPRESSED_SIZE
                              ? ArtifactSourceErrorKind::Integrity
                              : ArtifactSourceErrorKind::InvalidArchive;
        return std::unexpected(
            make_error(kind, std::string("ZIP entry extraction validation failed: ") +
                                 mz_zip_get_error_string(native)));
    }
    if (extraction.written != entry.uncompressed_size ||
        (*spool)->size() != entry.uncompressed_size) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::Integrity,
                       "ZIP entry actual size differs from its declared size"));
    }
    if (auto stopped = budget.check()) {
        return std::unexpected(std::move(*stopped));
    }
    (*spool)->seal();
    std::shared_ptr<SpoolImageSource> owned(std::move(*spool));
    std::shared_ptr<const IImageSource> immutable = owned;
    return std::make_shared<const ResolvedArtifact>(ResolvedArtifact{
        .source = std::move(immutable),
        .sha256 = extraction.hash.finish(),
        .origin = ArtifactSourceOrigin::ZipEntry,
        .logical_name = entry.name,
    });
}

[[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                            ArtifactSourceError>
resolve_zip(const std::filesystem::path& archive_path,
            const std::string_view requested_entry, const ArtifactSourceLimits& limits,
            const Budget& budget, SpoolReservation& reservation,
            std::mutex& archive_mutex) {
    auto selected_name = validate_name(requested_entry, limits.max_name_bytes, false);
    if (!selected_name) {
        return std::unexpected(std::move(selected_name.error()));
    }
    std::unique_lock archive_lock(archive_mutex, std::defer_lock);
    while (!archive_lock.try_lock()) {
        if (auto stopped = budget.check()) {
            return std::unexpected(std::move(*stopped));
        }
        std::this_thread::sleep_until(std::min(
            budget.deadline(), Clock::now() + std::chrono::milliseconds(1)));
    }
    if (limits.archive_reader_observer) {
        limits.archive_reader_observer();
    }
    auto opened = FileImageSource::open(archive_path);
    if (!opened) {
        return std::unexpected(from_file_error(opened.error()));
    }
    const auto& source = **opened;
    auto eocd = inspect_eocd(source, limits, budget);
    if (!eocd) {
        return std::unexpected(std::move(eocd.error()));
    }
    ZipReader reader(source, budget);
    if (auto initialized = reader.initialize(source.size()); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    auto entries = inventory_zip(reader, *eocd, limits, budget);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }
    const auto selected = std::ranges::find_if(*entries, [&](const ZipEntry& entry) {
        return entry.name == selected_name->path;
    });
    if (selected == entries->end() || selected->directory) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::NotFound,
                       "requested file entry is not present in the ZIP archive"));
    }
    return materialize_zip_entry(reader, *selected, limits, budget, reservation);
}

[[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                            ArtifactSourceError>
resolve_directory(const std::filesystem::path& directory,
                  const std::string_view requested_entry,
                  const ArtifactSourceLimits& limits, const Budget& budget,
                  SpoolReservation& reservation) {
    auto selected = validate_name(requested_entry, limits.max_name_bytes, false);
    if (!selected) {
        return std::unexpected(std::move(selected.error()));
    }
    auto source = FileImageSource::open_beneath(directory, selected->path);
    if (!source) {
        return std::unexpected(from_file_error(source.error()));
    }
    return materialize_source(**source, (*source)->size(), limits, budget, reservation,
                              ArtifactSourceOrigin::DirectoryEntry, selected->path);
}

[[nodiscard]] std::expected<std::shared_ptr<const ResolvedArtifact>,
                            ArtifactSourceError>
resolve_uncached(const std::filesystem::path& container, const std::string_view entry,
                 const ArtifactSourceLimits& limits, const Budget& budget,
                 SpoolReservation& reservation,
                 std::mutex& archive_mutex) {
    if (container.empty()) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArgument,
                                          "artifact container path is empty"));
    }
    if (limits.max_elapsed.count() < 0 || limits.max_name_bytes == 0U) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArgument,
                                          "artifact source limits are invalid"));
    }
    if (auto stopped = budget.check()) {
        return std::unexpected(std::move(*stopped));
    }

    std::error_code filesystem_error;
    const auto status = std::filesystem::symlink_status(container, filesystem_error);
    if (filesystem_error || !std::filesystem::exists(status)) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::NotFound,
                                          "artifact container does not exist",
                                          filesystem_error.value()));
    }
    if (std::filesystem::is_symlink(status)) {
        return std::unexpected(make_error(ArtifactSourceErrorKind::UnsafePath,
                                          "artifact container symlinks are forbidden"));
    }
    if (std::filesystem::is_directory(status)) {
        return resolve_directory(container, entry, limits, budget, reservation);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::UnsafePath,
                       "artifact container is not a regular file or directory"));
    }
    if (!entry.empty()) {
        return resolve_zip(container, entry, limits, budget, reservation,
                           archive_mutex);
    }

    auto source = FileImageSource::open(container);
    if (!source) {
        return std::unexpected(from_file_error(source.error()));
    }
    return materialize_source(**source, (*source)->size(), limits, budget, reservation,
                              ArtifactSourceOrigin::DirectFile,
                              container.filename().string());
}

}  // namespace

ArtifactSourceResolver::ArtifactSourceResolver(ArtifactSourceLimits limits)
    : limits_(std::move(limits)) {}

std::expected<std::shared_ptr<const ResolvedArtifact>, ArtifactSourceError>
ArtifactSourceResolver::resolve(const std::filesystem::path& archive_directory_or_file,
                                const std::string_view entry_name,
                                const std::stop_token cancellation) {
    try {
        if (archive_directory_or_file.empty()) {
            return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArgument,
                                              "artifact container path is empty"));
        }
        if (limits_.max_elapsed.count() < 0 || limits_.max_name_bytes == 0U) {
            return std::unexpected(make_error(ArtifactSourceErrorKind::InvalidArgument,
                                              "artifact source limits are invalid"));
        }
        if (path_contains_nul(archive_directory_or_file) ||
            path_contains_nul(limits_.temporary_directory)) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::UnsafePath,
                "artifact source path contains an embedded NUL"));
        }
#if defined(_WIN32)
        if (windows_path_has_alias(archive_directory_or_file) ||
            windows_path_has_alias(limits_.temporary_directory)) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::UnsafePath,
                "artifact source path aliases a Win32-normalized or reserved name"));
        }
#endif
        if (entry_name.size() > limits_.max_name_bytes) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::LimitExceeded,
                "artifact entry name exceeds the configured byte limit"));
        }
        if (entry_name.find('\0') != std::string_view::npos) {
            return std::unexpected(make_error(
                ArtifactSourceErrorKind::UnsafePath,
                "artifact entry name contains an embedded NUL"));
        }
        const auto deadline = deadline_for(limits_);
        Budget budget(cancellation, deadline);
        if (auto stopped = budget.check()) {
            return std::unexpected(std::move(*stopped));
        }

        std::error_code filesystem_error;
        auto normalized =
            std::filesystem::absolute(archive_directory_or_file, filesystem_error);
        if (filesystem_error) {
            return std::unexpected(
                make_error(ArtifactSourceErrorKind::InvalidArgument,
                           "unable to normalize artifact container path",
                           filesystem_error.value()));
        }
        normalized = normalized.lexically_normal();
        CacheKey key{normalized, std::string(entry_name)};
        for (;;) {
            std::shared_ptr<CacheEntry> cache_entry;
            bool owner = false;
            bool retry = false;
            {
                std::unique_lock lock(cache_mutex_);
                const auto position = cache_.find(key);
                if (position == cache_.end()) {
                    auto candidate = std::make_shared<CacheEntry>();
                    const auto [inserted_position, inserted] =
                        cache_.emplace(key, candidate);
                    cache_entry = inserted_position->second;
                    owner = inserted;
                } else {
                    cache_entry = position->second;
                }
                while (!owner && !cache_entry->done) {
                    if (auto stopped = budget.check()) {
                        return std::unexpected(std::move(*stopped));
                    }
                    const auto next_poll = std::min(
                        deadline, Clock::now() + std::chrono::milliseconds(10));
                    (void)cache_entry->ready.wait_until(lock, next_poll);
                }
                if (!owner) {
                    if (cache_entry->retryable_failure) {
                        retry = true;
                    } else if (cache_entry->result) {
                        return cache_entry->result;
                    } else {
                        return std::unexpected(cache_entry->error);
                    }
                }
            }
            if (retry) {
                continue;
            }

            SpoolReservation reservation(
                cache_mutex_, reserved_spool_bytes_, completed_spool_bytes_,
                observed_spool_capacity_, cache_entry->reserved_spool_bytes, limits_);
            using ResolveResult =
                std::expected<std::shared_ptr<const ResolvedArtifact>,
                              ArtifactSourceError>;
            static_assert(std::is_nothrow_move_constructible_v<ResolveResult>);
            std::optional<ResolveResult> resolved;
            try {
                resolved.emplace(resolve_uncached(normalized, entry_name, limits_,
                                                  budget, reservation,
                                                  archive_mutex_));
            } catch (...) {
                // CacheEntry owns a fully constructed allocation-failure-safe
                // fallback. Publication below never exposes a partial error.
            }

            bool retryable_failure = false;
            {
                std::lock_guard lock(cache_mutex_);
                if (resolved && *resolved) {
                    cache_entry->result = **resolved;
                    reservation.complete_locked();
                } else {
                    if (resolved) {
                        cache_entry->error = std::move(resolved->error());
                    }
                    retryable_failure =
                        cache_entry->error.kind == ArtifactSourceErrorKind::Cancelled ||
                        cache_entry->error.kind == ArtifactSourceErrorKind::TimedOut;
                    cache_entry->retryable_failure = retryable_failure;
                    reservation.release_locked();
                    if (retryable_failure) {
                        const auto position = cache_.find(key);
                        if (position != cache_.end() &&
                            position->second == cache_entry) {
                            cache_.erase(position);
                        }
                    }
                }
                // done is the publication barrier and must be the last mutated
                // state observed by waiters.
                cache_entry->done = true;
            }
            cache_entry->ready.notify_all();
            if (cache_entry->result) {
                return cache_entry->result;
            }
            return std::unexpected(cache_entry->error);
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::Io,
                       "memory allocation failed while resolving artifact source"));
    } catch (const std::filesystem::filesystem_error& error) {
        return std::unexpected(
            make_error(ArtifactSourceErrorKind::Io,
                       "filesystem failure while resolving artifact source",
                       error.code().value()));
    }
}

std::expected<PreflightFlashArtifact, ArtifactSourceError>
preflight_flash_artifact(ArtifactSourceResolver& resolver,
                         const std::filesystem::path& archive_directory_or_file,
                         const std::string_view entry_name,
                         const std::stop_token cancellation) {
    auto resolved =
        resolver.resolve(archive_directory_or_file, entry_name, cancellation);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    auto inspected = FlashArtifact::inspect((*resolved)->source, cancellation);
    if (!inspected) {
        return std::unexpected(make_error(
            inspected.error().kind == SparseErrorKind::Cancelled
                ? ArtifactSourceErrorKind::Cancelled
                : ArtifactSourceErrorKind::InvalidImage,
            "flash artifact inspection failed: " + inspected.error().message));
    }
    return PreflightFlashArtifact{
        .resolved = std::move(*resolved),
        .artifact = std::move(*inspected),
    };
}

}  // namespace kairosboot::image
