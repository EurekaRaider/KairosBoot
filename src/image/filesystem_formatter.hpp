// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
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
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kairosboot::image {

enum class FilesystemFormatErrorKind : std::uint8_t {
  InvalidArgument,
  Unsupported,
  Io,
};

struct FilesystemFormatError final {
  FilesystemFormatErrorKind kind{FilesystemFormatErrorKind::Io};
  int native_code{};
  std::string message;
};

enum FilesystemFeature : std::uint32_t {
  FilesystemCasefold = 1U << 0,
  FilesystemProjid = 1U << 1,
  FilesystemCompress = 1U << 2,
};
inline constexpr std::uint32_t kAllFilesystemFeatures =
    FilesystemCasefold | FilesystemProjid | FilesystemCompress;

class TemporaryFilesystemImage final {
public:
  TemporaryFilesystemImage(const TemporaryFilesystemImage &) = delete;
  TemporaryFilesystemImage &
  operator=(const TemporaryFilesystemImage &) = delete;

  TemporaryFilesystemImage(TemporaryFilesystemImage &&other) noexcept
      : path_(std::move(other.path_)) {
    other.path_.clear();
  }

  TemporaryFilesystemImage &
  operator=(TemporaryFilesystemImage &&other) noexcept {
    if (this != &other) {
      remove();
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }

  ~TemporaryFilesystemImage() { remove(); }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  explicit TemporaryFilesystemImage(std::filesystem::path path) noexcept
      : path_(std::move(path)) {}

  void remove() noexcept {
    if (!path_.empty()) {
      std::error_code ignored;
      static_cast<void>(std::filesystem::remove(path_, ignored));
      path_.clear();
    }
  }

  std::filesystem::path path_;

  friend std::expected<TemporaryFilesystemImage, FilesystemFormatError>
  generate_empty_filesystem_image(std::string_view, std::uint64_t,
                                  std::uint32_t, std::uint32_t, std::uint32_t);
};

namespace detail {

[[nodiscard]] inline std::vector<std::string>
make_f2fs_arguments(const std::filesystem::path &image,
                    const std::uint64_t partition_size,
                    const std::uint32_t features) {
  std::vector<std::string> arguments{"-S", std::to_string(partition_size), "-g",
                                     "android"};
  // Keep the frozen AOSP fs.cpp order. Repeated -O arguments are intentional.
  if ((features & FilesystemProjid) != 0U) {
    arguments.insert(arguments.end(), {"-O", "project_quota,extra_attr"});
  }
  if ((features & FilesystemCasefold) != 0U) {
    arguments.insert(arguments.end(), {"-O", "casefold", "-C", "utf8"});
  }
  if ((features & FilesystemCompress) != 0U) {
    arguments.insert(arguments.end(),
                     {"-O", "compression", "-O", "extra_attr"});
  }
  arguments.push_back(image.string());
  return arguments;
}

[[nodiscard]] inline FilesystemFormatError
format_error(const FilesystemFormatErrorKind kind, std::string message,
             const int native_code = 0) {
  return {
      .kind = kind, .native_code = native_code, .message = std::move(message)};
}

[[nodiscard]] inline std::optional<std::filesystem::path>
executable_directory() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(1024U);
  for (;;) {
    const DWORD length = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U) {
      return std::nullopt;
    }
    if (length < buffer.size() - 1U) {
      return std::filesystem::path(std::wstring_view{buffer.data(), length})
          .parent_path();
    }
    if (buffer.size() > 32768U) {
      return std::nullopt;
    }
    buffer.resize(buffer.size() * 2U);
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0U;
  static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
  if (size == 0U) {
    return std::nullopt;
  }
  std::vector<char> buffer(size);
  if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return std::nullopt;
  }
  return std::filesystem::path(buffer.data()).parent_path();
#else
  std::vector<char> buffer(1024U);
  for (;;) {
    const auto length =
        ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) {
      return std::nullopt;
    }
    if (static_cast<std::size_t>(length) < buffer.size()) {
      return std::filesystem::path(
                 std::string_view{buffer.data(),
                                  static_cast<std::size_t>(length)})
          .parent_path();
    }
    buffer.resize(buffer.size() * 2U);
  }
#endif
}

[[nodiscard]] inline std::filesystem::path
resolve_tool(const char *environment_name, const std::string_view basename) {
  if (const char *configured = std::getenv(environment_name);
      configured != nullptr && configured[0] != '\0') {
    return std::filesystem::path(configured);
  }
  if (const auto directory = executable_directory(); directory.has_value()) {
    auto candidate = *directory / std::string(basename);
#if defined(_WIN32)
    candidate += L".exe";
#endif
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error) && !error) {
      return candidate;
    }
  }
  return std::filesystem::path(std::string(basename));
}

[[nodiscard]] inline std::optional<std::filesystem::path>
mke2fs_config(const std::filesystem::path &tool) {
  if (const char *configured = std::getenv("KAIROSBOOT_MKE2FS_CONFIG");
      configured != nullptr && configured[0] != '\0') {
    return std::filesystem::path(configured);
  }
  if (!tool.has_parent_path()) {
    return std::nullopt;
  }
  const auto candidate = tool.parent_path() / "mke2fs.conf";
  std::error_code error;
  if (std::filesystem::is_regular_file(candidate, error) && !error) {
    return candidate;
  }
  return std::nullopt;
}

[[nodiscard]] inline std::expected<std::filesystem::path, FilesystemFormatError>
create_temporary_path() {
#if defined(_WIN32)
  std::vector<wchar_t> directory(MAX_PATH + 1U);
  const DWORD directory_length =
      ::GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
  if (directory_length == 0U || directory_length >= directory.size()) {
    return std::unexpected(
        format_error(FilesystemFormatErrorKind::Io,
                     "unable to locate the temporary directory",
                     static_cast<int>(::GetLastError())));
  }
  std::vector<wchar_t> file(MAX_PATH + 1U);
  if (::GetTempFileNameW(directory.data(), L"kbf", 0U, file.data()) == 0U) {
    return std::unexpected(
        format_error(FilesystemFormatErrorKind::Io,
                     "unable to create a temporary filesystem image",
                     static_cast<int>(::GetLastError())));
  }
  return std::filesystem::path(file.data());
#else
  std::error_code filesystem_error;
  const auto directory = std::filesystem::temp_directory_path(filesystem_error);
  if (filesystem_error) {
    return std::unexpected(format_error(
        FilesystemFormatErrorKind::Io,
        "unable to locate the temporary directory", filesystem_error.value()));
  }
  auto pattern = (directory / "kairosboot-format-XXXXXX.img").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const int descriptor = ::mkstemps(writable.data(), 4);
  if (descriptor < 0) {
    return std::unexpected(
        format_error(FilesystemFormatErrorKind::Io,
                     "unable to create a temporary filesystem image", errno));
  }
  static_cast<void>(::close(descriptor));
  return std::filesystem::path(writable.data());
#endif
}

#if defined(_WIN32)
[[nodiscard]] inline std::wstring
quote_windows_argument(const std::wstring_view value) {
  if (value.find_first_of(L" \t\"") == std::wstring_view::npos) {
    return std::wstring(value);
  }
  std::wstring quoted{L'\"'};
  std::size_t backslashes = 0U;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      quoted.append(backslashes * 2U + 1U, L'\\');
      quoted.push_back(L'\"');
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(character);
    }
    backslashes = 0U;
  }
  quoted.append(backslashes * 2U, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}
#endif

[[nodiscard]] inline std::expected<void, FilesystemFormatError>
run_tool(const std::filesystem::path &tool,
         const std::vector<std::string> &arguments,
         const std::optional<std::filesystem::path> &config) {
#if defined(_WIN32)
  std::wstring command = quote_windows_argument(tool.wstring());
  for (const auto &argument : arguments) {
    command.push_back(L' ');
    command +=
        quote_windows_argument(std::filesystem::path(argument).wstring());
  }
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(L'\0');
  static std::mutex environment_mutex;
  std::scoped_lock environment_lock(environment_mutex);
  std::wstring previous;
  bool had_previous = false;
  if (config.has_value()) {
    const DWORD required =
        ::GetEnvironmentVariableW(L"MKE2FS_CONFIG", nullptr, 0U);
    if (required != 0U) {
      std::vector<wchar_t> buffer(required);
      const DWORD length =
          ::GetEnvironmentVariableW(L"MKE2FS_CONFIG", buffer.data(), required);
      if (length != 0U && length < required) {
        previous.assign(buffer.data(), length);
        had_previous = true;
      }
    }
    if (::SetEnvironmentVariableW(L"MKE2FS_CONFIG", config->c_str()) == FALSE) {
      return std::unexpected(format_error(FilesystemFormatErrorKind::Io,
                                          "unable to configure mke2fs",
                                          ::GetLastError()));
    }
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created = ::CreateProcessW(
      tool.has_parent_path() ? tool.c_str() : nullptr, writable.data(), nullptr,
      nullptr, FALSE, 0U, nullptr, nullptr, &startup, &process);
  const DWORD create_error = created == FALSE ? ::GetLastError() : 0U;
  if (config.has_value()) {
    static_cast<void>(::SetEnvironmentVariableW(
        L"MKE2FS_CONFIG", had_previous ? previous.c_str() : nullptr));
  }
  if (created == FALSE) {
    return std::unexpected(
        format_error(create_error == ERROR_FILE_NOT_FOUND
                         ? FilesystemFormatErrorKind::Unsupported
                         : FilesystemFormatErrorKind::Io,
                     "unable to start filesystem image generator",
                     static_cast<int>(create_error)));
  }
  static_cast<void>(::WaitForSingleObject(process.hProcess, INFINITE));
  DWORD exit_code = 1U;
  static_cast<void>(::GetExitCodeProcess(process.hProcess, &exit_code));
  static_cast<void>(::CloseHandle(process.hThread));
  static_cast<void>(::CloseHandle(process.hProcess));
  if (exit_code != 0U) {
    return std::unexpected(
        format_error(FilesystemFormatErrorKind::Io,
                     "filesystem image generator failed with exit code " +
                         std::to_string(exit_code),
                     static_cast<int>(exit_code)));
  }
#else
  const pid_t child = ::fork();
  if (child < 0) {
    return std::unexpected(
        format_error(FilesystemFormatErrorKind::Io,
                     "unable to fork the filesystem image generator", errno));
  }
  if (child == 0) {
    if (config.has_value()) {
      static_cast<void>(::setenv("MKE2FS_CONFIG", config->c_str(), 1));
    }
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1U);
    storage.push_back(tool.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1U);
    for (auto &argument : storage) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    if (tool.has_parent_path()) {
      ::execv(tool.c_str(), argv.data());
    } else {
      ::execvp(tool.c_str(), argv.data());
    }
    ::_exit(errno == ENOENT ? 127 : 126);
  }
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    return std::unexpected(format_error(
        FilesystemFormatErrorKind::Io,
        "unable to wait for the filesystem image generator", errno));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return std::unexpected(format_error(
        exit_code == 127 ? FilesystemFormatErrorKind::Unsupported
                         : FilesystemFormatErrorKind::Io,
        exit_code == 127 ? "filesystem image generator is not installed"
                         : "filesystem image generator failed with exit code " +
                               std::to_string(exit_code),
        exit_code));
  }
#endif
  return {};
}

} // namespace detail

[[nodiscard]] inline std::expected<TemporaryFilesystemImage,
                                   FilesystemFormatError>
generate_empty_filesystem_image(const std::string_view filesystem_type,
                                const std::uint64_t partition_size,
                                const std::uint32_t erase_block_size,
                                const std::uint32_t logical_block_size,
                                const std::uint32_t features = 0U) {
  if (partition_size == 0U ||
      partition_size > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max())) {
    return std::unexpected(detail::format_error(
        FilesystemFormatErrorKind::InvalidArgument,
        "filesystem partition size must be between 1 and INT64_MAX bytes"));
  }
  if (filesystem_type != "ext4" && filesystem_type != "f2fs") {
    return std::unexpected(
        detail::format_error(FilesystemFormatErrorKind::Unsupported,
                             "format supports only ext4 and f2fs filesystems"));
  }
  if ((features & ~kAllFilesystemFeatures) != 0U) {
    return std::unexpected(detail::format_error(
        FilesystemFormatErrorKind::InvalidArgument,
        "filesystem options contain unsupported feature bits"));
  }
  auto path = detail::create_temporary_path();
  if (!path) {
    return std::unexpected(std::move(path.error()));
  }
  TemporaryFilesystemImage image(std::move(*path));

  std::filesystem::path tool;
  std::vector<std::string> arguments;
  std::optional<std::filesystem::path> config;
  if (filesystem_type == "ext4") {
    constexpr std::uint32_t block_size = 4096U;
    if (partition_size < block_size) {
      return std::unexpected(detail::format_error(
          FilesystemFormatErrorKind::InvalidArgument,
          "ext4 partition size must be at least 4096 bytes"));
    }
    tool = detail::resolve_tool("KAIROSBOOT_MKE2FS", "mke2fs");
    config = detail::mke2fs_config(tool);
    std::string extended = "android_sparse";
    if (erase_block_size != 0U && logical_block_size != 0U) {
      auto stride = logical_block_size / block_size;
      auto stripe_width = erase_block_size / block_size;
      if (logical_block_size < 8192U) {
        stride = 8192U / block_size;
      }
      stripe_width = std::max(stripe_width, stride);
      extended += ",stride=" + std::to_string(stride) +
                  ",stripe-width=" + std::to_string(stripe_width);
    }
    arguments = {"-t",
                 "ext4",
                 "-b",
                 "4096",
                 "-E",
                 extended,
                 "-O",
                 "uninit_bg",
                 image.path().string(),
                 std::to_string(partition_size / block_size)};
  } else {
    tool = detail::resolve_tool("KAIROSBOOT_MAKE_F2FS", "make_f2fs");
    arguments =
        detail::make_f2fs_arguments(image.path(), partition_size, features);
  }

  auto generated = detail::run_tool(tool, arguments, config);
  if (!generated) {
    return std::unexpected(std::move(generated.error()));
  }
  std::error_code size_error;
  const auto generated_size =
      std::filesystem::file_size(image.path(), size_error);
  if (size_error || generated_size == 0U) {
    return std::unexpected(detail::format_error(
        FilesystemFormatErrorKind::Io,
        "filesystem image generator produced no data", size_error.value()));
  }
  return image;
}

} // namespace kairosboot::image
