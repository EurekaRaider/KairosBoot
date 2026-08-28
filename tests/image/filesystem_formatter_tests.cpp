// SPDX-License-Identifier: MIT
#include "src/image/filesystem_formatter.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "check failed at line " << __LINE__ << ": " << #condition   \
                << '\n';                                                       \
      return __LINE__;                                                         \
    }                                                                          \
  } while (false)

int main() {
  using namespace kairosboot::image;
  const auto arguments = detail::make_f2fs_arguments(
      std::filesystem::path{"image.tmp"}, 4096U,
      FilesystemCasefold | FilesystemProjid | FilesystemCompress);
  CHECK(arguments ==
        std::vector<std::string>({"-S", "4096", "-g", "android", "-O",
                                  "project_quota,extra_attr", "-O", "casefold",
                                  "-C", "utf8", "-O", "compression", "-O",
                                  "extra_attr", "image.tmp"}));

  const auto unsupported = generate_empty_filesystem_image(
      "f2fs", 4096U, 0U, 0U, kAllFilesystemFeatures | (1U << 9));
  CHECK(!unsupported.has_value());
  CHECK(unsupported.error().kind == FilesystemFormatErrorKind::InvalidArgument);
  return 0;
}
