// SPDX-License-Identifier: MIT
#include "src/image/file_source.hpp"
#include "src/transport/image_transfer_source.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            throw CheckFailure(std::string("check failed: ") + #condition + " at line " + \
                               std::to_string(__LINE__));                                     \
        }                                                                                    \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("kairosboot-file-source-" + std::to_string(stamp));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw CheckFailure("unable to write the file source fixture");
    }
}

[[nodiscard]] std::filesystem::path with_embedded_nul(
    const std::filesystem::path& prefix) {
    auto native = prefix.native();
    native.push_back(std::filesystem::path::value_type{});
    native.push_back(static_cast<std::filesystem::path::value_type>('x'));
    return std::filesystem::path(std::move(native));
}

[[nodiscard]] std::string as_string(const std::span<const std::byte> bytes) {
    std::string result;
    result.reserve(bytes.size());
    for (const auto byte : bytes) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return result;
}

void open_rejects_invalid_paths() {
    TemporaryDirectory temporary;
    const auto empty = kairosboot::image::FileImageSource::open({});
    CHECK(!empty);
    CHECK(empty.error().kind == kairosboot::image::FileSourceErrorKind::InvalidArgument);

    const auto missing = kairosboot::image::FileImageSource::open(
        temporary.path() / "missing.img");
    CHECK(!missing);
    CHECK(missing.error().kind == kairosboot::image::FileSourceErrorKind::NotFound);

    const auto directory = kairosboot::image::FileImageSource::open(temporary.path());
    CHECK(!directory);
    CHECK(directory.error().kind ==
          kairosboot::image::FileSourceErrorKind::NotRegularFile);
}

void filesystem_entrypoints_reject_embedded_nul() {
    TemporaryDirectory temporary;
    const auto image = temporary.path() / "boot.img";
    write_bytes(image, "payload");
    const auto direct = kairosboot::image::FileImageSource::open(
        with_embedded_nul(image));
    CHECK(!direct);
    CHECK(direct.error().kind ==
          kairosboot::image::FileSourceErrorKind::UnsafePath);

    const auto relative = kairosboot::image::FileImageSource::open_beneath(
        temporary.path(), with_embedded_nul(std::filesystem::path{"boot.img"}));
    CHECK(!relative);
    CHECK(relative.error().kind ==
          kairosboot::image::FileSourceErrorKind::UnsafePath);

    const auto base = kairosboot::image::FileImageSource::open_beneath(
        with_embedded_nul(temporary.path()), "boot.img");
    CHECK(!base);
    CHECK(base.error().kind ==
          kairosboot::image::FileSourceErrorKind::UnsafePath);
}

#if defined(_WIN32)
void windows_normalized_aliases_are_rejected_before_open() {
    TemporaryDirectory temporary;
    const auto image = temporary.path() / "boot.img";
    write_bytes(image, "payload");
    const std::array aliases{
        temporary.path() / "boot.img.", temporary.path() / "boot.img ",
        temporary.path() / "NUL.img", temporary.path() / "CONOUT$.txt",
        temporary.path() / "boot.img:stream",
    };
    for (const auto& alias : aliases) {
        const auto opened = kairosboot::image::FileImageSource::open(alias);
        CHECK(!opened);
        CHECK(opened.error().kind ==
              kairosboot::image::FileSourceErrorKind::UnsafePath);
    }
    const std::array relative_aliases{
        std::filesystem::path{"boot.img."}, std::filesystem::path{"boot.img "},
        std::filesystem::path{"CON"}, std::filesystem::path{"CONIN$.txt"},
        std::filesystem::path{"boot.img:stream"},
    };
    for (const auto& alias : relative_aliases) {
        const auto opened = kairosboot::image::FileImageSource::open_beneath(
            temporary.path(), alias);
        CHECK(!opened);
        CHECK(opened.error().kind ==
              kairosboot::image::FileSourceErrorKind::UnsafePath);
    }
}
#endif

void random_access_reads_are_bounded_by_the_open_size() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "system.img";
    write_bytes(path, "0123456789abcdef");
    auto source = kairosboot::image::FileImageSource::open(path);
    CHECK(source);
    CHECK((*source)->size() == 16);

    std::array<std::byte, 5> middle{};
    const auto middle_read = (*source)->read_at(4, middle);
    CHECK(middle_read && *middle_read == middle.size());
    CHECK(as_string(middle) == "45678");

    std::array<std::byte, 8> tail{};
    const auto tail_read = (*source)->read_at(13, tail);
    CHECK(tail_read && *tail_read == 3);
    CHECK(as_string(std::span(tail).first(3)) == "def");

    write_bytes(path, "0123456789abcdefghijklmnop");
    std::array<std::byte, 8> appended{};
    const auto appended_read = (*source)->read_at(16, appended);
    CHECK(appended_read && *appended_read == 0);
    CHECK((*source)->size() == 16);
}

void eof_and_out_of_bounds_reads_are_empty() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "userdata.img";
    write_bytes(path, "01234567");
    auto source = kairosboot::image::FileImageSource::open(path);
    CHECK(source);

    std::array<std::byte, 4> bytes{};
    const auto at_eof = (*source)->read_at(8, bytes);
    CHECK(at_eof && *at_eof == 0);
    const auto beyond_eof = (*source)->read_at(80, bytes);
    CHECK(beyond_eof && *beyond_eof == 0);
    const auto empty_before_eof = (*source)->read_at(3, {});
    CHECK(empty_before_eof && *empty_before_eof == 0);
    const auto empty_beyond_eof = (*source)->read_at(80, {});
    CHECK(empty_beyond_eof && *empty_beyond_eof == 0);
}

void native_unicode_paths_are_supported() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() /
                      std::filesystem::path(u8"\u7cfb\u7edf\u955c\u50cf.img");
    write_bytes(path, "kairos");
    auto source = kairosboot::image::FileImageSource::open(path);
    CHECK(source);

    std::array<std::byte, 6> bytes{};
    const auto read = (*source)->read_at(0, bytes);
    CHECK(read && *read == bytes.size());
    CHECK(as_string(bytes) == "kairos");
}

void handle_based_open_rejects_leaf_and_intermediate_links() {
    TemporaryDirectory temporary;
    const auto outside = temporary.path() / "outside.img";
    write_bytes(outside, "outside");

    const auto direct_link = temporary.path() / "direct-link.img";
    std::error_code link_error;
    std::filesystem::create_symlink(outside, direct_link, link_error);
    if (link_error) {
        return;
    }
    const auto direct = kairosboot::image::FileImageSource::open(direct_link);
    CHECK(!direct);
    CHECK(direct.error().kind ==
          kairosboot::image::FileSourceErrorKind::UnsafePath);

    const auto package = temporary.path() / "package";
    const auto outside_directory = temporary.path() / "outside-directory";
    std::filesystem::create_directory(package);
    std::filesystem::create_directory(outside_directory);
    write_bytes(outside_directory / "secret.img", "secret");

    const auto leaf_link = package / "leaf.img";
    std::filesystem::create_symlink(outside, leaf_link);
    const auto leaf = kairosboot::image::FileImageSource::open_beneath(
        package, "leaf.img");
    CHECK(!leaf);
    CHECK(leaf.error().kind ==
          kairosboot::image::FileSourceErrorKind::UnsafePath);

    const auto intermediate_link = package / "jump";
    std::filesystem::create_directory_symlink(
        outside_directory, intermediate_link);
    const auto intermediate = kairosboot::image::FileImageSource::open_beneath(
        package, std::filesystem::path("jump") / "secret.img");
    CHECK(!intermediate);
    CHECK(intermediate.error().kind ==
          kairosboot::image::FileSourceErrorKind::UnsafePath);

    const auto linked_root = temporary.path() / "linked-root";
    std::filesystem::create_directory_symlink(package, linked_root);
    const auto strict_root = kairosboot::image::FileImageSource::open_beneath(
        linked_root, "leaf.img");
    CHECK(!strict_root);
    CHECK(strict_root.error().kind ==
          kairosboot::image::FileSourceErrorKind::UnsafePath);
}

void directory_boundary_survives_root_and_ancestor_replacement() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directories(root / "images");
    std::filesystem::create_directories(outside / "images");
    write_bytes(root / "images/system.img", "inside");
    write_bytes(outside / "images/system.img", "outside");

    auto boundary = kairosboot::image::FileDirectoryBoundary::capture(root);
    CHECK(boundary);
    const auto retained_root = temporary.path() / "retained-root";
    std::filesystem::rename(root, retained_root);
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, root, link_error);
    if (link_error) {
        return;
    }
    auto opened = kairosboot::image::FileImageSource::open_beneath(
        *boundary, std::filesystem::path("images") / "system.img");
    CHECK(opened);
    std::array<std::byte, 6U> contents{};
    auto read = (*opened)->read_at(0U, contents);
    CHECK(read && *read == contents.size());
    CHECK(as_string(contents) == "inside");

    // Windows can rename the retained root itself but not an open handle's
    // ancestor; the root and alias-retarget cases cover its native guarantee.
#if !defined(_WIN32)
    const auto trusted_anchor = temporary.path() / "trusted-anchor";
    const auto outside_anchor = temporary.path() / "outside-anchor";
    std::filesystem::create_directories(trusted_anchor / "root/images");
    std::filesystem::create_directories(outside_anchor / "root/images");
    write_bytes(trusted_anchor / "root/images/system.img", "inside");
    write_bytes(outside_anchor / "root/images/system.img", "outside");
    auto ancestor_boundary =
        kairosboot::image::FileDirectoryBoundary::capture(
            trusted_anchor / "root");
    CHECK(ancestor_boundary);
    const auto retained_anchor = temporary.path() / "retained-anchor";
    std::filesystem::rename(trusted_anchor, retained_anchor);
    std::filesystem::create_directory_symlink(
        outside_anchor, trusted_anchor, link_error);
    CHECK(!link_error);
    auto after_ancestor_replacement =
        kairosboot::image::FileImageSource::open_beneath(
            *ancestor_boundary,
            std::filesystem::path("images") / "system.img");
    CHECK(after_ancestor_replacement);
    read = (*after_ancestor_replacement)->read_at(0U, contents);
    CHECK(read && *read == contents.size());
    CHECK(as_string(contents) == "inside");
#endif

    const auto root_alias = temporary.path() / "root-alias";
    std::filesystem::create_directory_symlink(
        retained_root, root_alias, link_error);
    CHECK(!link_error);
    auto alias_boundary =
        kairosboot::image::FileDirectoryBoundary::capture(root_alias);
    CHECK(alias_boundary);
    std::filesystem::remove(root_alias);
    std::filesystem::create_directory_symlink(outside, root_alias, link_error);
    CHECK(!link_error);
    auto after_alias_retarget =
        kairosboot::image::FileImageSource::open_beneath(
            *alias_boundary,
            std::filesystem::path("images") / "system.img");
    CHECK(after_alias_retarget);
    read = (*after_alias_retarget)->read_at(0U, contents);
    CHECK(read && *read == contents.size());
    CHECK(as_string(contents) == "inside");
}

void concurrent_root_swaps_cannot_redirect_a_directory_boundary() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    const auto outside = temporary.path() / "outside";
    const auto retained = temporary.path() / "retained";
    std::filesystem::create_directories(root / "images");
    std::filesystem::create_directories(outside / "images");
    write_bytes(root / "images/system.img", "inside");
    write_bytes(outside / "images/system.img", "outside");

    const auto probe = temporary.path() / "link-probe";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, probe, link_error);
    if (link_error) {
        return;
    }
    std::filesystem::remove(probe);

    auto boundary = kairosboot::image::FileDirectoryBoundary::capture(root);
    CHECK(boundary);
    std::atomic<bool> finished{};
    std::atomic<std::size_t> completed_swaps{};
    std::thread attacker([&] {
        for (std::size_t iteration = 0U; iteration < 1'000U; ++iteration) {
            std::error_code ignored;
            std::filesystem::rename(root, retained, ignored);
            if (ignored) {
                continue;
            }
            std::filesystem::create_directory_symlink(outside, root, ignored);
            if (!ignored) {
                completed_swaps.fetch_add(1U, std::memory_order_relaxed);
                std::this_thread::yield();
                std::filesystem::remove(root, ignored);
            }
            std::filesystem::rename(retained, root, ignored);
        }
        finished.store(true, std::memory_order_release);
    });

    std::size_t successful_opens = 0U;
    std::exception_ptr reader_error;
    try {
        while (!finished.load(std::memory_order_acquire)) {
            auto opened = kairosboot::image::FileImageSource::open_beneath(
                *boundary, std::filesystem::path("images") / "system.img");
            CHECK(opened);
            std::array<std::byte, 6U> contents{};
            const auto read = (*opened)->read_at(0U, contents);
            CHECK(read && *read == contents.size());
            CHECK(as_string(contents) == "inside");
            ++successful_opens;
        }
    } catch (...) {
        reader_error = std::current_exception();
    }
    attacker.join();
    if (reader_error) {
        std::rethrow_exception(reader_error);
    }
    CHECK(completed_swaps.load(std::memory_order_relaxed) != 0U);
    CHECK(successful_opens != 0U);
}

#if !defined(_WIN32)
void concurrent_leaf_swaps_never_escape_the_directory_handle() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "package";
    std::filesystem::create_directory(package);
    const auto outside = temporary.path() / "outside.img";
    write_bytes(outside, "outside");
    const auto leaf = package / "system.img";
    write_bytes(leaf, "inside");

    std::atomic<bool> finished{};
    std::exception_ptr attacker_error;
    std::thread attacker([&] {
        try {
            for (std::size_t iteration = 0; iteration < 2'000U; ++iteration) {
                std::error_code ignored;
                std::filesystem::remove(leaf, ignored);
                std::filesystem::create_symlink(outside, leaf);
                std::this_thread::yield();
                std::filesystem::remove(leaf, ignored);
                const auto candidate = package / "candidate.img";
                write_bytes(candidate, "inside");
                std::filesystem::rename(candidate, leaf);
            }
        } catch (...) {
            attacker_error = std::current_exception();
        }
        finished.store(true, std::memory_order_release);
    });

    std::size_t successful_opens = 0;
    while (!finished.load(std::memory_order_acquire)) {
        auto opened = kairosboot::image::FileImageSource::open_beneath(
            package, "system.img");
        if (!opened) {
            continue;
        }
        ++successful_opens;
        CHECK((*opened)->size() == 6U);
        std::array<std::byte, 6> contents{};
        const auto read = (*opened)->read_at(0, contents);
        CHECK(read && *read == contents.size());
        CHECK(as_string(contents) == "inside");
    }
    attacker.join();
    if (attacker_error) {
        std::rethrow_exception(attacker_error);
    }
    CHECK(successful_opens != 0U);
}
#endif

void concurrent_parent_swaps_never_escape_the_directory_handle() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "package";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directory(package);
    std::filesystem::create_directory(outside);
    write_bytes(outside / "system.img", "outside");

    const auto probe = package / "link-probe";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, probe, link_error);
    if (link_error) {
        return;
    }
    std::filesystem::remove(probe);

    const auto parent = package / "images";
    std::filesystem::create_directory(parent);
    write_bytes(parent / "system.img", "inside");

    std::atomic<bool> finished{};
    std::exception_ptr attacker_error;
    std::thread attacker([&] {
        try {
            for (std::size_t iteration = 0; iteration < 2'000U; ++iteration) {
                std::error_code ignored;
                std::filesystem::remove_all(parent, ignored);
                std::filesystem::create_directory_symlink(outside, parent, ignored);
                std::this_thread::yield();
                std::filesystem::remove(parent, ignored);

                const auto candidate =
                    package / ("candidate-" + std::to_string(iteration));
                std::filesystem::create_directory(candidate, ignored);
                if (!ignored) {
                    write_bytes(candidate / "system.img", "inside");
                    std::filesystem::rename(candidate, parent, ignored);
                }
                std::filesystem::remove_all(candidate, ignored);
            }
        } catch (...) {
            attacker_error = std::current_exception();
        }
        finished.store(true, std::memory_order_release);
    });

    std::size_t successful_opens = 0;
    while (!finished.load(std::memory_order_acquire)) {
        auto opened = kairosboot::image::FileImageSource::open_beneath(
            package, std::filesystem::path("images") / "system.img");
        if (!opened) {
            continue;
        }
        ++successful_opens;
        CHECK((*opened)->size() == 6U);
        std::array<std::byte, 6> contents{};
        const auto read = (*opened)->read_at(0, contents);
        CHECK(read && *read == contents.size());
        CHECK(as_string(contents) == "inside");
    }
    attacker.join();
    if (attacker_error) {
        std::rethrow_exception(attacker_error);
    }
    CHECK(successful_opens != 0U);
}

void transfer_adapter_completes_short_source_reads() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "boot.img";
    write_bytes(path, "abcdefghij");
    auto file = kairosboot::image::FileImageSource::open(path);
    CHECK(file);
    auto transfer = kairosboot::transport::ImageTransferSource::create(*file);
    CHECK(transfer);

    std::array<std::byte, 6> bytes{};
    CHECK((*transfer)->read_exact(2, bytes));
    CHECK(as_string(bytes) == "cdefgh");
    CHECK(!(*transfer)->read_exact(8, bytes));
    CHECK((*transfer)->read_exact(10, {}));
    CHECK(!(*transfer)->read_exact(11, {}));
}

void truncation_after_open_fails_exact_transfer_reads() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "vendor.img";
    write_bytes(path, "abcdefghijklmnop");
    auto file = kairosboot::image::FileImageSource::open(path);
    CHECK(file);
    auto transfer = kairosboot::transport::ImageTransferSource::create(*file);
    CHECK(transfer);

    write_bytes(path, "abcd");
    std::array<std::byte, 8> bytes{};
    const auto short_read = (*file)->read_at(0, bytes);
    CHECK(short_read && *short_read == 4);
    CHECK(as_string(std::span(bytes).first(4)) == "abcd");
    const auto truncated_eof = (*file)->read_at(4, bytes);
    CHECK(truncated_eof && *truncated_eof == 0);
    CHECK(!(*transfer)->read_exact(0, bytes));
}

void concurrent_positioned_reads_do_not_share_a_cursor() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "super.img";
    write_bytes(path, "0123456789abcdef");
    auto source = kairosboot::image::FileImageSource::open(path);
    CHECK(source);

    std::array<std::byte, 4> first{};
    std::array<std::byte, 4> second{};
    std::exception_ptr left_error;
    std::exception_ptr right_error;
    std::thread left([&] {
        try {
            const auto result = (*source)->read_at(1, first);
            CHECK(result && *result == first.size());
        } catch (...) {
            left_error = std::current_exception();
        }
    });
    std::thread right([&] {
        try {
            const auto result = (*source)->read_at(9, second);
            CHECK(result && *result == second.size());
        } catch (...) {
            right_error = std::current_exception();
        }
    });
    left.join();
    right.join();
    if (left_error) {
        std::rethrow_exception(left_error);
    }
    if (right_error) {
        std::rethrow_exception(right_error);
    }
    CHECK(as_string(first) == "1234");
    CHECK(as_string(second) == "9abc");
}

void concurrent_random_reads_are_consistent() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "product.img";
    std::string payload(2U * 1024U * 1024U, '\0');
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>((index * 131U + index / 251U) & 0xffU);
    }
    write_bytes(path, payload);
    auto opened = kairosboot::image::FileImageSource::open(path);
    CHECK(opened);
    const auto source = *opened;

    constexpr std::size_t thread_count = 8;
    constexpr std::size_t reads_per_thread = 256;
    std::vector<std::exception_ptr> errors(thread_count);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        workers.emplace_back([&, thread_index, source] {
            try {
                std::uint64_t state = 0x9e3779b97f4a7c15ULL ^ thread_index;
                for (std::size_t iteration = 0; iteration < reads_per_thread;
                     ++iteration) {
                    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                    const auto offset = static_cast<std::size_t>(state % payload.size());
                    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                    const auto requested =
                        1U + static_cast<std::size_t>(state % (32U * 1024U));
                    const auto amount = std::min(requested, payload.size() - offset);

                    std::vector<std::byte> actual(amount);
                    const auto result = source->read_at(offset, actual);
                    CHECK(result && *result == amount);
                    for (std::size_t byte_index = 0; byte_index < amount; ++byte_index) {
                        CHECK(std::to_integer<unsigned char>(actual[byte_index]) ==
                              static_cast<unsigned char>(payload[offset + byte_index]));
                    }
                }
            } catch (...) {
                errors[thread_index] = std::current_exception();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
}

using Test = std::pair<std::string_view, void (*)()>;

}  // namespace

int main() {
    const std::array tests{
        Test{"open rejects invalid paths", open_rejects_invalid_paths},
        Test{"filesystem entrypoints reject embedded NUL",
             filesystem_entrypoints_reject_embedded_nul},
#if defined(_WIN32)
        Test{"Windows normalized aliases are rejected",
             windows_normalized_aliases_are_rejected_before_open},
#endif
        Test{"bounded random access", random_access_reads_are_bounded_by_the_open_size},
        Test{"EOF and out-of-bounds reads", eof_and_out_of_bounds_reads_are_empty},
        Test{"native Unicode paths", native_unicode_paths_are_supported},
        Test{"handle-based symlink rejection",
             handle_based_open_rejects_leaf_and_intermediate_links},
        Test{"directory boundary root and ancestor replacement",
             directory_boundary_survives_root_and_ancestor_replacement},
        Test{"directory boundary concurrent root replacement",
             concurrent_root_swaps_cannot_redirect_a_directory_boundary},
#if !defined(_WIN32)
        Test{"handle-based race confinement",
             concurrent_leaf_swaps_never_escape_the_directory_handle},
#endif
        Test{"handle-based parent race confinement",
             concurrent_parent_swaps_never_escape_the_directory_handle},
        Test{"transfer adapter exact reads", transfer_adapter_completes_short_source_reads},
        Test{"truncation fails exact reads", truncation_after_open_fails_exact_transfer_reads},
        Test{"concurrent positioned reads", concurrent_positioned_reads_do_not_share_a_cursor},
        Test{"concurrent random reads", concurrent_random_reads_are_consistent},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " file source test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " file source tests passed\n";
    return 0;
}
