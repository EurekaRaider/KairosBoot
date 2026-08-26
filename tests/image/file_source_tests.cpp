// SPDX-License-Identifier: MIT
#include "src/image/file_source.hpp"
#include "src/transport/image_transfer_source.hpp"

#include <algorithm>
#include <array>
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
        Test{"bounded random access", random_access_reads_are_bounded_by_the_open_size},
        Test{"EOF and out-of-bounds reads", eof_and_out_of_bounds_reads_are_empty},
        Test{"native Unicode paths", native_unicode_paths_are_supported},
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
