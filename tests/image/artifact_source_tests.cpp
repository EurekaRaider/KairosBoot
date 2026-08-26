// SPDX-License-Identifier: MIT
#include "src/image/artifact_source.hpp"
#include "src/image/sha256.hpp"

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
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using kairosboot::image::ArtifactSourceErrorKind;
using kairosboot::image::ArtifactSourceLimits;
using kairosboot::image::ArtifactSourceOrigin;
using kairosboot::image::ArtifactSourceResolver;
using kairosboot::image::IImageSource;
using kairosboot::image::preflight_flash_artifact;
using kairosboot::image::sha256_hex;

class CheckFailure final : public std::runtime_error {
    public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                               \
    do {                                                                               \
        if (!(condition)) {                                                            \
            throw CheckFailure(std::string("check failed: ") + #condition +            \
                               " at line " + std::to_string(__LINE__));                \
        }                                                                              \
    } while (false)

class TemporaryDirectory final {
    public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        const auto suffix = sequence.fetch_add(1U, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                ("kairosboot-artifact-source-test-" + std::to_string(suffix));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    private:
    std::filesystem::path path_;
};

void append_u16(std::vector<std::byte>& output, const std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    append_u16(output, static_cast<std::uint16_t>(value & 0xffffU));
    append_u16(output, static_cast<std::uint16_t>(value >> 16U));
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value & 0xffffffffULL));
    append_u32(output, static_cast<std::uint32_t>(value >> 32U));
}

void append_string(std::vector<std::byte>& output, const std::string_view value) {
    for (const auto character : value) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

[[nodiscard]] std::uint32_t crc32(const std::string_view bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto character : bytes) {
        crc ^= static_cast<unsigned char>(character);
        for (std::uint32_t bit = 0; bit < 8U; ++bit) {
            const auto mask =
                static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

struct ZipFixtureEntry final {
    std::string name;
    std::string payload;
    std::vector<std::byte> compressed;
    std::optional<std::string> local_name;
    std::optional<std::uint32_t> declared_crc;
    std::optional<std::uint32_t> declared_uncompressed_size;
    std::uint16_t method{};
    std::uint16_t flags{};
    std::uint16_t version_made_by{static_cast<std::uint16_t>((3U << 8U) | 20U)};
    std::uint32_t external_attributes{0100000U << 16U};
};

[[nodiscard]] std::vector<std::byte> as_bytes(const std::string_view value) {
    std::vector<std::byte> result;
    result.reserve(value.size());
    append_string(result, value);
    return result;
}

[[nodiscard]] std::vector<std::byte>
make_zip(const std::span<const ZipFixtureEntry> entries, const bool zip64 = false) {
    struct CentralRecord final {
        const ZipFixtureEntry* entry{};
        std::uint32_t local_offset{};
        std::uint32_t crc{};
        std::uint32_t compressed_size{};
        std::uint32_t uncompressed_size{};
    };

    std::vector<std::byte> output;
    std::vector<CentralRecord> central;
    for (const auto& entry : entries) {
        const auto compressed =
            entry.compressed.empty() ? as_bytes(entry.payload) : entry.compressed;
        CHECK(compressed.size() <= UINT32_MAX);
        CHECK(entry.payload.size() <= UINT32_MAX);
        CHECK(output.size() <= UINT32_MAX);
        const auto actual_crc = crc32(entry.payload);
        const auto declared_crc = entry.declared_crc.value_or(actual_crc);
        const auto declared_size = entry.declared_uncompressed_size.value_or(
            static_cast<std::uint32_t>(entry.payload.size()));
        const auto local_name = entry.local_name.value_or(entry.name);

        central.push_back(CentralRecord{
            .entry = &entry,
            .local_offset = static_cast<std::uint32_t>(output.size()),
            .crc = declared_crc,
            .compressed_size = static_cast<std::uint32_t>(compressed.size()),
            .uncompressed_size = declared_size,
        });
        append_u32(output, 0x04034b50U);
        append_u16(output, 20U);
        append_u16(output, entry.flags);
        append_u16(output, entry.method);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, declared_crc);
        append_u32(output, static_cast<std::uint32_t>(compressed.size()));
        append_u32(output, declared_size);
        append_u16(output, static_cast<std::uint16_t>(local_name.size()));
        append_u16(output, 0U);
        append_string(output, local_name);
        output.insert(output.end(), compressed.begin(), compressed.end());
    }

    CHECK(output.size() <= UINT32_MAX);
    const auto central_offset = static_cast<std::uint32_t>(output.size());
    for (const auto& record : central) {
        const auto& entry = *record.entry;
        append_u32(output, 0x02014b50U);
        append_u16(output, entry.version_made_by);
        append_u16(output, 20U);
        append_u16(output, entry.flags);
        append_u16(output, entry.method);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, record.crc);
        append_u32(output, record.compressed_size);
        append_u32(output, record.uncompressed_size);
        append_u16(output, static_cast<std::uint16_t>(entry.name.size()));
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, entry.external_attributes);
        append_u32(output, record.local_offset);
        append_string(output, entry.name);
    }
    CHECK(output.size() - central_offset <= UINT32_MAX);
    const auto central_size =
        static_cast<std::uint32_t>(output.size() - central_offset);
    CHECK(entries.size() <= UINT16_MAX);

    if (zip64) {
        const auto zip64_offset = static_cast<std::uint64_t>(output.size());
        append_u32(output, 0x06064b50U);
        append_u64(output, 44U);
        append_u16(output, 45U);
        append_u16(output, 45U);
        append_u32(output, 0U);
        append_u32(output, 0U);
        append_u64(output, entries.size());
        append_u64(output, entries.size());
        append_u64(output, central_size);
        append_u64(output, central_offset);
        append_u32(output, 0x07064b50U);
        append_u32(output, 0U);
        append_u64(output, zip64_offset);
        append_u32(output, 1U);
    }

    append_u32(output, 0x06054b50U);
    append_u16(output, 0U);
    append_u16(output, 0U);
    append_u16(output, zip64 ? UINT16_MAX : static_cast<std::uint16_t>(entries.size()));
    append_u16(output, zip64 ? UINT16_MAX : static_cast<std::uint16_t>(entries.size()));
    append_u32(output, zip64 ? UINT32_MAX : central_size);
    append_u32(output, zip64 ? UINT32_MAX : central_offset);
    append_u16(output, 0U);
    return output;
}

void write_bytes(const std::filesystem::path& path,
                 const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw CheckFailure("unable to write artifact test fixture");
    }
}

void write_text(const std::filesystem::path& path, const std::string_view text) {
    const auto bytes = as_bytes(text);
    write_bytes(path, bytes);
}

[[nodiscard]] std::string read_source(const IImageSource& source) {
    CHECK(source.size() <= SIZE_MAX);
    std::vector<std::byte> bytes(static_cast<std::size_t>(source.size()));
    std::size_t completed = 0;
    while (completed < bytes.size()) {
        auto read = source.read_at(completed, std::span(bytes).subspan(completed));
        CHECK(read);
        CHECK(*read != 0U);
        completed += *read;
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] std::filesystem::path
write_zip(const TemporaryDirectory& temporary,
          const std::span<const ZipFixtureEntry> entries, const bool zip64 = false) {
    const auto path = temporary.path() / "package.zip";
    const auto archive = make_zip(entries, zip64);
    write_bytes(path, archive);
    return path;
}

void direct_and_directory_sources_are_immutable_snapshots() {
    TemporaryDirectory temporary;
    const auto direct = temporary.path() / "boot.img";
    write_text(direct, "original-direct");
    ArtifactSourceResolver resolver;
    auto resolved_direct = resolver.resolve(direct);
    CHECK(resolved_direct);
    write_text(direct, "changed-direct!");
    CHECK(read_source(*(*resolved_direct)->source) == "original-direct");

    const auto images = temporary.path() / "images";
    std::filesystem::create_directory(images);
    const auto system = images / "system.img";
    write_text(system, "original-directory");
    auto resolved_directory = resolver.resolve(images, "system.img");
    CHECK(resolved_directory);
    std::filesystem::remove(system);
    CHECK((*resolved_directory)->origin == ArtifactSourceOrigin::DirectoryEntry);
    CHECK(read_source(*(*resolved_directory)->source) == "original-directory");
}

void stored_non_first_entry_is_materialized_and_hashed() {
    TemporaryDirectory temporary;
    const std::array entries{
        ZipFixtureEntry{.name = "ignored.txt", .payload = "ignore"},
        ZipFixtureEntry{.name = "images/system.img", .payload = "system-payload"},
    };
    const auto archive = write_zip(temporary, entries);
    ArtifactSourceResolver resolver;
    auto resolved = resolver.resolve(archive, "images/system.img");
    CHECK(resolved);
    CHECK((*resolved)->origin == ArtifactSourceOrigin::ZipEntry);
    CHECK(read_source(*(*resolved)->source) == "system-payload");
    CHECK(sha256_hex((*resolved)->sha256) ==
          "fa5762c2547b1db66f4c98686e7431d58e0f551891501c8c608fdc93a6f77735");
}

void deflate_and_zip64_entries_are_supported() {
    TemporaryDirectory temporary;
    std::string payload;
    for (std::size_t index = 0; index < 64U; ++index) {
        payload += "deflated payload";
    }
    constexpr std::array compressed_values{
        0x4bU, 0x49U, 0x4dU, 0xcbU, 0x49U, 0x2cU, 0x49U, 0x4dU, 0x51U,
        0x28U, 0x48U, 0xacU, 0xccU, 0xc9U, 0x4fU, 0x4cU, 0x49U, 0x19U,
        0xe5U, 0x8fU, 0xf2U, 0x47U, 0xf9U, 0x23U, 0x86U, 0x0fU, 0x00U,
    };
    std::vector<std::byte> compressed;
    compressed.reserve(compressed_values.size());
    for (const auto value : compressed_values) {
        compressed.push_back(static_cast<std::byte>(value));
    }
    const std::array entries{
        ZipFixtureEntry{
            .name = "system.img",
            .payload = payload,
            .compressed = std::move(compressed),
            .method = 8U,
        },
    };
    const auto archive = write_zip(temporary, entries, true);
    ArtifactSourceResolver resolver;
    auto resolved = resolver.resolve(archive, "system.img");
    CHECK(resolved);
    CHECK(read_source(*(*resolved)->source) == payload);
}

void crc_corruption_and_truncation_fail_closed() {
    TemporaryDirectory temporary;
    const std::array corrupt_entries{
        ZipFixtureEntry{
            .name = "system.img",
            .payload = "payload",
            .declared_crc = 0x12345678U,
        },
    };
    const auto corrupt = write_zip(temporary, corrupt_entries);
    ArtifactSourceResolver resolver;
    const auto crc_result = resolver.resolve(corrupt, "system.img");
    CHECK(!crc_result);
    CHECK(crc_result.error().kind == ArtifactSourceErrorKind::Integrity ||
          crc_result.error().kind == ArtifactSourceErrorKind::InvalidArchive);

    const std::array valid_entries{
        ZipFixtureEntry{.name = "system.img", .payload = "payload"},
    };
    auto bytes = make_zip(valid_entries);
    bytes.pop_back();
    const auto truncated = temporary.path() / "truncated.zip";
    write_bytes(truncated, bytes);
    const auto truncated_result = resolver.resolve(truncated, "system.img");
    CHECK(!truncated_result);
    CHECK(truncated_result.error().kind == ArtifactSourceErrorKind::InvalidArchive);
}

void unsafe_duplicate_and_conflicting_paths_fail_closed() {
    const std::array unsafe_names{
        std::string{"../system.img"},        std::string{"/system.img"},
        std::string{"C:system.img"},         std::string{"images\\system.img"},
        std::string{"images//system.img"},   std::string{"images/./system.img"},
        std::string{"images/../system.img"}, std::string{"bad\0name.img", 12U},
        std::string{"bad\xc0\x80.img", 9U},
    };
    for (const auto& name : unsafe_names) {
        TemporaryDirectory temporary;
        const std::array entries{
            ZipFixtureEntry{.name = name, .payload = "payload"},
        };
        const auto archive = write_zip(temporary, entries);
        ArtifactSourceResolver resolver;
        const auto result = resolver.resolve(archive, "system.img");
        CHECK(!result);
        CHECK(result.error().kind == ArtifactSourceErrorKind::UnsafePath ||
              result.error().kind == ArtifactSourceErrorKind::InvalidArchive);
    }

    const std::array collision_entries{
        ZipFixtureEntry{.name = "Images/system.img", .payload = "a"},
        ZipFixtureEntry{.name = "images/system.img", .payload = "b"},
    };
    TemporaryDirectory collision_temp;
    const auto collision = write_zip(collision_temp, collision_entries);
    ArtifactSourceResolver collision_resolver;
    const auto collision_result =
        collision_resolver.resolve(collision, "images/system.img");
    CHECK(!collision_result);
    CHECK(collision_result.error().kind == ArtifactSourceErrorKind::UnsafePath);

    const std::array duplicate_entries{
        ZipFixtureEntry{.name = "system.img", .payload = "a"},
        ZipFixtureEntry{.name = "system.img", .payload = "b"},
    };
    TemporaryDirectory duplicate_temp;
    const auto duplicate = write_zip(duplicate_temp, duplicate_entries);
    ArtifactSourceResolver duplicate_resolver;
    const auto duplicate_result = duplicate_resolver.resolve(duplicate, "system.img");
    CHECK(!duplicate_result);
    CHECK(duplicate_result.error().kind == ArtifactSourceErrorKind::UnsafePath);

    const std::array conflict_entries{
        ZipFixtureEntry{.name = "images", .payload = "file"},
        ZipFixtureEntry{.name = "images/system.img", .payload = "payload"},
    };
    TemporaryDirectory conflict_temp;
    const auto conflict = write_zip(conflict_temp, conflict_entries);
    ArtifactSourceResolver conflict_resolver;
    const auto conflict_result =
        conflict_resolver.resolve(conflict, "images/system.img");
    CHECK(!conflict_result);
    CHECK(conflict_result.error().kind == ArtifactSourceErrorKind::UnsafePath);
}

void metadata_features_and_local_name_mismatch_fail_closed() {
    TemporaryDirectory special_temp;
    const std::array special_entries{
        ZipFixtureEntry{
            .name = "system.img",
            .payload = "target",
            .external_attributes = 0120000U << 16U,
        },
    };
    const auto special = write_zip(special_temp, special_entries);
    ArtifactSourceResolver special_resolver;
    const auto special_result = special_resolver.resolve(special, "system.img");
    CHECK(!special_result);
    CHECK(special_result.error().kind == ArtifactSourceErrorKind::UnsafePath);

    TemporaryDirectory encrypted_temp;
    const std::array encrypted_entries{
        ZipFixtureEntry{.name = "system.img", .payload = "x", .flags = 1U},
    };
    const auto encrypted = write_zip(encrypted_temp, encrypted_entries);
    ArtifactSourceResolver encrypted_resolver;
    const auto encrypted_result = encrypted_resolver.resolve(encrypted, "system.img");
    CHECK(!encrypted_result);
    CHECK(encrypted_result.error().kind == ArtifactSourceErrorKind::UnsupportedFeature);

    TemporaryDirectory method_temp;
    const std::array method_entries{
        ZipFixtureEntry{.name = "system.img", .payload = "x", .method = 12U},
    };
    const auto unsupported_method = write_zip(method_temp, method_entries);
    ArtifactSourceResolver method_resolver;
    const auto method_result =
        method_resolver.resolve(unsupported_method, "system.img");
    CHECK(!method_result);
    CHECK(method_result.error().kind == ArtifactSourceErrorKind::UnsupportedFeature);

    TemporaryDirectory mismatch_temp;
    const std::array mismatch_entries{
        ZipFixtureEntry{
            .name = "system.img",
            .payload = "payload",
            .local_name = std::string{"vendor.img"},
        },
    };
    const auto mismatch = write_zip(mismatch_temp, mismatch_entries);
    ArtifactSourceResolver mismatch_resolver;
    const auto mismatch_result = mismatch_resolver.resolve(mismatch, "system.img");
    CHECK(!mismatch_result);
    CHECK(mismatch_result.error().kind == ArtifactSourceErrorKind::InvalidArchive);
}

void entry_ratio_aggregate_disk_and_name_limits_are_enforced() {
    TemporaryDirectory temporary;
    const std::array entries{
        ZipFixtureEntry{.name = "one.img", .payload = "aa"},
        ZipFixtureEntry{.name = "two.img", .payload = "bb"},
    };
    const auto archive = write_zip(temporary, entries);

    ArtifactSourceLimits count_limits;
    count_limits.max_entry_count = 1U;
    ArtifactSourceResolver count_resolver(count_limits);
    auto count = count_resolver.resolve(archive, "one.img");
    CHECK(!count);
    CHECK(count.error().kind == ArtifactSourceErrorKind::LimitExceeded);

    ArtifactSourceLimits aggregate_limits;
    aggregate_limits.max_total_uncompressed_size = 3U;
    ArtifactSourceResolver aggregate_resolver(aggregate_limits);
    auto aggregate = aggregate_resolver.resolve(archive, "one.img");
    CHECK(!aggregate);
    CHECK(aggregate.error().kind == ArtifactSourceErrorKind::LimitExceeded);

    ArtifactSourceLimits disk_limits;
    disk_limits.max_spool_bytes = 1U;
    ArtifactSourceResolver disk_resolver(disk_limits);
    auto disk = disk_resolver.resolve(archive, "one.img");
    CHECK(!disk);
    CHECK(disk.error().kind == ArtifactSourceErrorKind::LimitExceeded);

    ArtifactSourceLimits overflow_limits;
    overflow_limits.minimum_free_space_bytes = UINT64_MAX;
    ArtifactSourceResolver overflow_resolver(overflow_limits);
    auto overflow = overflow_resolver.resolve(archive, "one.img");
    CHECK(!overflow);
    CHECK(overflow.error().kind == ArtifactSourceErrorKind::LimitExceeded);

    ArtifactSourceLimits failing_disk_limits;
    failing_disk_limits.temporary_directory = temporary.path() / "missing-temp";
    ArtifactSourceResolver failing_disk_resolver(failing_disk_limits);
    auto failing_disk = failing_disk_resolver.resolve(archive, "one.img");
    CHECK(!failing_disk);
    CHECK(failing_disk.error().kind == ArtifactSourceErrorKind::Io);

    const std::string large_payload(1'001U, 'a');
    const std::array<std::byte, 1> compressed{std::byte{0}};
    const std::array bomb_entries{
        ZipFixtureEntry{
            .name = "bomb.img",
            .payload = large_payload,
            .compressed = std::vector(compressed.begin(), compressed.end()),
            .method = 8U,
        },
    };
    TemporaryDirectory bomb_temp;
    const auto bomb = write_zip(bomb_temp, bomb_entries);
    ArtifactSourceLimits ratio_limits;
    ratio_limits.max_compression_ratio = 1'000U;
    ArtifactSourceResolver ratio_resolver(ratio_limits);
    auto ratio = ratio_resolver.resolve(bomb, "bomb.img");
    CHECK(!ratio);
    CHECK(ratio.error().kind == ArtifactSourceErrorKind::LimitExceeded);

    std::string bounded_payload;
    for (std::size_t index = 0; index < 64U; ++index) {
        bounded_payload += "deflated payload";
    }
    constexpr std::array bounded_compressed_values{
        0x4bU, 0x49U, 0x4dU, 0xcbU, 0x49U, 0x2cU, 0x49U, 0x4dU, 0x51U,
        0x28U, 0x48U, 0xacU, 0xccU, 0xc9U, 0x4fU, 0x4cU, 0x49U, 0x19U,
        0xe5U, 0x8fU, 0xf2U, 0x47U, 0xf9U, 0x23U, 0x86U, 0x0fU, 0x00U,
    };
    std::vector<std::byte> bounded_compressed;
    for (const auto value : bounded_compressed_values) {
        bounded_compressed.push_back(static_cast<std::byte>(value));
    }
    const std::array bounded_entries{
        ZipFixtureEntry{
            .name = "bounded.img",
            .payload = bounded_payload,
            .compressed = std::move(bounded_compressed),
            .declared_uncompressed_size = 10U,
            .method = 8U,
        },
    };
    TemporaryDirectory bounded_temp;
    const auto bounded_zip = write_zip(bounded_temp, bounded_entries);
    ArtifactSourceResolver bounded_resolver;
    auto bounded = bounded_resolver.resolve(bounded_zip, "bounded.img");
    CHECK(!bounded);
    CHECK(bounded.error().kind == ArtifactSourceErrorKind::Integrity ||
          bounded.error().kind == ArtifactSourceErrorKind::InvalidArchive);

    TemporaryDirectory long_temp;
    const std::array long_entries{
        ZipFixtureEntry{.name = std::string(512U, 'a'), .payload = "x"},
    };
    const auto long_zip = write_zip(long_temp, long_entries);
    ArtifactSourceResolver long_resolver;
    auto long_name = long_resolver.resolve(long_zip, "system.img");
    CHECK(!long_name);
    CHECK(long_name.error().kind == ArtifactSourceErrorKind::LimitExceeded);
}

void cancellation_timeout_and_symlink_checks_precede_publication() {
    TemporaryDirectory temporary;
    const auto image = temporary.path() / "system.img";
    write_text(image, "payload");

    std::stop_source cancellation;
    cancellation.request_stop();
    ArtifactSourceResolver cancelled_resolver;
    auto cancelled = cancelled_resolver.resolve(image, {}, cancellation.get_token());
    CHECK(!cancelled);
    CHECK(cancelled.error().kind == ArtifactSourceErrorKind::Cancelled);

    ArtifactSourceLimits timeout_limits;
    timeout_limits.max_elapsed = std::chrono::milliseconds(0);
    ArtifactSourceResolver timeout_resolver(timeout_limits);
    auto timed_out = timeout_resolver.resolve(image);
    CHECK(!timed_out);
    CHECK(timed_out.error().kind == ArtifactSourceErrorKind::TimedOut);

    const auto link = temporary.path() / "linked.img";
    std::error_code link_error;
    std::filesystem::create_symlink(image, link, link_error);
    if (!link_error) {
        ArtifactSourceResolver link_resolver;
        auto linked = link_resolver.resolve(link);
        CHECK(!linked);
        CHECK(linked.error().kind == ArtifactSourceErrorKind::UnsafePath);
    }
}

void concurrent_resolves_publish_one_shared_snapshot() {
    TemporaryDirectory temporary;
    const auto image = temporary.path() / "super.img";
    write_text(image, std::string(2U * 1024U * 1024U, 'k'));
    ArtifactSourceResolver resolver;
    constexpr std::size_t thread_count = 8U;
    std::array<std::shared_ptr<const kairosboot::image::ResolvedArtifact>, thread_count>
        results{};
    std::array<std::exception_ptr, thread_count> failures{};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            try {
                auto result = resolver.resolve(image);
                CHECK(result);
                results[index] = std::move(*result);
            } catch (...) {
                failures[index] = std::current_exception();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    for (const auto& result : results) {
        CHECK(result == results.front());
        CHECK(result->source == results.front()->source);
    }
}

void preflight_inspection_is_transport_free_and_rejects_invalid_sparse() {
    TemporaryDirectory temporary;
    const auto invalid_sparse = temporary.path() / "invalid-sparse.img";
    const std::array sparse_magic{std::byte{0x3a}, std::byte{0xff}, std::byte{0x26},
                                  std::byte{0xed}};
    write_bytes(invalid_sparse, sparse_magic);
    ArtifactSourceResolver resolver;
    const auto result = preflight_flash_artifact(resolver, invalid_sparse);
    CHECK(!result);
    CHECK(result.error().kind == ArtifactSourceErrorKind::InvalidImage);
}

using Test = std::pair<std::string_view, void (*)()>;

}  // namespace

int main() {
    const std::array tests{
        Test{"direct_and_directory_sources_are_immutable_snapshots",
             direct_and_directory_sources_are_immutable_snapshots},
        Test{"stored_non_first_entry_is_materialized_and_hashed",
             stored_non_first_entry_is_materialized_and_hashed},
        Test{"deflate_and_zip64_entries_are_supported",
             deflate_and_zip64_entries_are_supported},
        Test{"crc_corruption_and_truncation_fail_closed",
             crc_corruption_and_truncation_fail_closed},
        Test{"unsafe_duplicate_and_conflicting_paths_fail_closed",
             unsafe_duplicate_and_conflicting_paths_fail_closed},
        Test{"metadata_features_and_local_name_mismatch_fail_closed",
             metadata_features_and_local_name_mismatch_fail_closed},
        Test{"entry_ratio_aggregate_disk_and_name_limits_are_enforced",
             entry_ratio_aggregate_disk_and_name_limits_are_enforced},
        Test{"cancellation_timeout_and_symlink_checks_precede_publication",
             cancellation_timeout_and_symlink_checks_precede_publication},
        Test{"concurrent_resolves_publish_one_shared_snapshot",
             concurrent_resolves_publish_one_shared_snapshot},
        Test{"preflight_inspection_is_transport_free_and_rejects_invalid_sparse",
             preflight_inspection_is_transport_free_and_rejects_invalid_sparse},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
