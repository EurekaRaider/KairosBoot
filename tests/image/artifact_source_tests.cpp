// SPDX-License-Identifier: MIT
#include "src/image/artifact_source.hpp"
#include "src/image/sha256.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
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
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using kairosboot::image::ArtifactSourceErrorKind;
using kairosboot::image::ArtifactSourceLimits;
using kairosboot::image::ArtifactSourceOrigin;
using kairosboot::image::ArtifactSourceResolver;
using kairosboot::image::IImageSource;
using kairosboot::image::preflight_flash_artifact;
using kairosboot::image::sha256_hex;

#if !defined(_WIN32)
std::filesystem::path executable_path;

[[nodiscard]] std::set<int> open_descriptors() {
    std::set<int> descriptors;
    for (int descriptor = 3; descriptor < 1'024; ++descriptor) {
        errno = 0;
        if (::fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF) {
            descriptors.insert(descriptor);
        }
    }
    return descriptors;
}
#endif

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
#if defined(_WIN32)
        const auto process_id = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
        const auto process_id = static_cast<std::uint64_t>(::getpid());
#endif
        path_ = std::filesystem::temp_directory_path() /
                ("kairosboot-artifact-source-test-" +
                 std::to_string(process_id) + "-" + std::to_string(suffix));
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
    std::optional<std::uint64_t> zip64_uncompressed_size;
    bool entry_zip64{};
    std::uint16_t method{};
    std::uint16_t flags{};
    std::uint16_t version_made_by{static_cast<std::uint16_t>((3U << 8U) | 20U)};
    std::uint32_t external_attributes{0100000U << 16U};
};

[[nodiscard]] ZipFixtureEntry zip_fixture_entry(std::string name,
                                                std::string payload) {
    ZipFixtureEntry entry{};
    entry.name = std::move(name);
    entry.payload = std::move(payload);
    return entry;
}

template <typename Configure>
[[nodiscard]] ZipFixtureEntry zip_fixture_entry(std::string name,
                                                std::string payload,
                                                Configure&& configure) {
    auto entry = zip_fixture_entry(std::move(name), std::move(payload));
    std::forward<Configure>(configure)(entry);
    return entry;
}

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
        const auto entry_zip64_size = entry.zip64_uncompressed_size.value_or(
            static_cast<std::uint64_t>(declared_size));

        central.push_back(CentralRecord{
            .entry = &entry,
            .local_offset = static_cast<std::uint32_t>(output.size()),
            .crc = declared_crc,
            .compressed_size = static_cast<std::uint32_t>(compressed.size()),
            .uncompressed_size = declared_size,
        });
        append_u32(output, 0x04034b50U);
        append_u16(output, entry.entry_zip64 ? 45U : 20U);
        append_u16(output, entry.flags);
        append_u16(output, entry.method);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, declared_crc);
        append_u32(output, entry.entry_zip64
                               ? UINT32_MAX
                               : static_cast<std::uint32_t>(compressed.size()));
        append_u32(output, entry.entry_zip64 ? UINT32_MAX : declared_size);
        append_u16(output, static_cast<std::uint16_t>(local_name.size()));
        append_u16(output, entry.entry_zip64 ? 20U : 0U);
        append_string(output, local_name);
        if (entry.entry_zip64) {
            append_u16(output, 0x0001U);
            append_u16(output, 16U);
            append_u64(output, entry_zip64_size);
            append_u64(output, compressed.size());
        }
        output.insert(output.end(), compressed.begin(), compressed.end());
    }

    CHECK(output.size() <= UINT32_MAX);
    const auto central_offset = static_cast<std::uint32_t>(output.size());
    for (const auto& record : central) {
        const auto& entry = *record.entry;
        append_u32(output, 0x02014b50U);
        append_u16(output, entry.version_made_by);
        append_u16(output, entry.entry_zip64 ? 45U : 20U);
        append_u16(output, entry.flags);
        append_u16(output, entry.method);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, record.crc);
        append_u32(output, entry.entry_zip64 ? UINT32_MAX : record.compressed_size);
        append_u32(output, entry.entry_zip64 ? UINT32_MAX : record.uncompressed_size);
        append_u16(output, static_cast<std::uint16_t>(entry.name.size()));
        append_u16(output, entry.entry_zip64 ? 28U : 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, entry.external_attributes);
        append_u32(output, entry.entry_zip64 ? UINT32_MAX : record.local_offset);
        append_string(output, entry.name);
        if (entry.entry_zip64) {
            append_u16(output, 0x0001U);
            append_u16(output, 24U);
            append_u64(output, entry.zip64_uncompressed_size.value_or(
                                     record.uncompressed_size));
            append_u64(output, record.compressed_size);
            append_u64(output, record.local_offset);
        }
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

[[nodiscard]] std::filesystem::path with_embedded_nul(
    const std::filesystem::path& prefix) {
    auto native = prefix.native();
    native.push_back(std::filesystem::path::value_type{});
    native.push_back(static_cast<std::filesystem::path::value_type>('x'));
    return std::filesystem::path(std::move(native));
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

void root_relative_direct_files_are_confined_to_the_selected_boundary() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directories(root / "images");
    std::filesystem::create_directory(outside);
    write_text(root / "images/system.img", "inside");
    write_text(outside / "system.img", "outside");

    ArtifactSourceLimits direct_limits;
    direct_limits.max_spool_bytes = 6U;
    ArtifactSourceResolver resolver(direct_limits);
    auto direct = resolver.resolve_file_beneath(
        root, std::filesystem::path("images") / "system.img");
    CHECK(direct);
    CHECK((*direct)->origin == ArtifactSourceOrigin::DirectFile);
    CHECK(read_source(*(*direct)->source) == "inside");
    auto repeated = resolver.resolve_file_beneath(
        root, std::filesystem::path("images") / "system.img");
    CHECK(repeated);
    CHECK(*repeated == *direct);

    const auto intermediate = root / "jump";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(
        outside, intermediate, link_error);
    if (link_error) {
        return;
    }
    ArtifactSourceResolver escape_resolver;
    auto escaped = escape_resolver.resolve_file_beneath(
        root, std::filesystem::path("jump") / "system.img");
    CHECK(!escaped);
    CHECK(escaped.error().kind == ArtifactSourceErrorKind::UnsafePath);

    const auto root_alias = temporary.path() / "root-alias";
    std::filesystem::create_directory_symlink(root, root_alias, link_error);
    CHECK(!link_error);
    ArtifactSourceResolver alias_resolver;
    auto through_root_alias = alias_resolver.resolve_file_beneath(
        root_alias, std::filesystem::path("images") / "system.img");
    CHECK(through_root_alias);
    CHECK(read_source(*(*through_root_alias)->source) == "inside");
}

void root_and_ancestor_replacement_after_capture_cannot_redirect_resolution() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directories(root / "images");
    std::filesystem::create_directories(outside / "images");
    write_text(root / "images/system.img", "inside");
    write_text(outside / "images/system.img", "outside");

    const auto probe = temporary.path() / "link-probe";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, probe, link_error);
    if (link_error) {
        return;
    }
    std::filesystem::remove(probe);

    ArtifactSourceLimits root_limits;
    root_limits.package_entry_observer = [&](std::string_view) {
        std::filesystem::rename(root, temporary.path() / "retained-root");
        std::filesystem::create_directory_symlink(outside, root);
    };
    ArtifactSourceResolver root_resolver(root_limits);
    auto after_root_replacement = root_resolver.resolve_file_beneath(
        root, std::filesystem::path("images") / "system.img");
    CHECK(after_root_replacement);
    CHECK(read_source(*(*after_root_replacement)->source) == "inside");
    auto cached_after_replacement = root_resolver.resolve_file_beneath(
        root, std::filesystem::path("images") / "system.img");
    CHECK(cached_after_replacement);
    CHECK(*cached_after_replacement == *after_root_replacement);

    const auto trusted_anchor = temporary.path() / "trusted-anchor";
    const auto outside_anchor = temporary.path() / "outside-anchor";
    std::filesystem::create_directories(trusted_anchor / "root/images");
    std::filesystem::create_directories(outside_anchor / "root/images");
    write_text(trusted_anchor / "root/images/system.img", "inside");
    write_text(outside_anchor / "root/images/system.img", "outside");
    ArtifactSourceLimits ancestor_limits;
    ancestor_limits.package_entry_observer = [&](std::string_view) {
        std::filesystem::rename(
            trusted_anchor, temporary.path() / "retained-anchor");
        std::filesystem::create_directory_symlink(
            outside_anchor, trusted_anchor);
    };
    ArtifactSourceResolver ancestor_resolver(ancestor_limits);
    auto after_ancestor_replacement =
        ancestor_resolver.resolve_file_beneath(
            trusted_anchor / "root",
            std::filesystem::path("images") / "system.img");
    CHECK(after_ancestor_replacement);
    CHECK(read_source(*(*after_ancestor_replacement)->source) == "inside");
}

void concurrent_root_relative_resolves_share_the_captured_boundary() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directory(root);
    std::filesystem::create_directory(outside);
    write_text(root / "system.img", "inside");
    write_text(outside / "system.img", "outside");
    const auto probe = temporary.path() / "link-probe";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, probe, link_error);
    if (link_error) {
        return;
    }
    std::filesystem::remove(probe);

    ArtifactSourceLimits limits;
    limits.package_entry_observer = [&](std::string_view) {
        std::filesystem::rename(root, temporary.path() / "retained-root");
        std::filesystem::create_directory_symlink(outside, root);
    };
    ArtifactSourceResolver resolver(limits);
    constexpr std::size_t thread_count = 8U;
    std::array<std::shared_ptr<const kairosboot::image::ResolvedArtifact>,
               thread_count> results{};
    std::array<std::exception_ptr, thread_count> failures{};
    std::atomic<std::size_t> ready{};
    std::atomic<bool> start{};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0U; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            try {
                ready.fetch_add(1U, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                auto result = resolver.resolve_file_beneath(root, "system.img");
                CHECK(result);
                results[index] = std::move(*result);
            } catch (...) {
                failures[index] = std::current_exception();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
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
        CHECK(read_source(*result->source) == "inside");
    }
}

void root_relative_parent_replacement_race_never_publishes_outside_bytes() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directory(root);
    std::filesystem::create_directory(outside);
    write_text(outside / "system.img", "outside");

    const auto probe = root / "link-probe";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, probe, link_error);
    if (link_error) {
        return;
    }
    std::filesystem::remove(probe);

    const auto parent = root / "images";
    std::filesystem::create_directory(parent);
    write_text(parent / "system.img", "inside");

    std::atomic<bool> finished{};
    std::exception_ptr attacker_error;
    std::thread attacker([&] {
        try {
            for (std::size_t iteration = 0U; iteration < 1'000U; ++iteration) {
                std::error_code ignored;
                std::filesystem::remove_all(parent, ignored);
                std::filesystem::create_directory_symlink(
                    outside, parent, ignored);
                std::this_thread::yield();
                std::filesystem::remove(parent, ignored);

                const auto candidate =
                    root / ("candidate-" + std::to_string(iteration));
                std::filesystem::create_directory(candidate, ignored);
                if (!ignored) {
                    write_text(candidate / "system.img", "inside");
                    std::filesystem::rename(candidate, parent, ignored);
                }
                std::filesystem::remove_all(candidate, ignored);
            }
        } catch (...) {
            attacker_error = std::current_exception();
        }
        finished.store(true, std::memory_order_release);
    });

    std::size_t successful_resolutions = 0U;
    std::exception_ptr reader_error;
    try {
        while (!finished.load(std::memory_order_acquire)) {
            ArtifactSourceResolver current;
            auto resolved = current.resolve_file_beneath(
                root, std::filesystem::path("images") / "system.img");
            if (!resolved) {
                continue;
            }
            ++successful_resolutions;
            CHECK(read_source(*(*resolved)->source) == "inside");
        }
    } catch (...) {
        reader_error = std::current_exception();
    }
    attacker.join();
    if (attacker_error) {
        std::rethrow_exception(attacker_error);
    }
    if (reader_error) {
        std::rethrow_exception(reader_error);
    }
    CHECK(successful_resolutions != 0U);
}

void stored_non_first_entry_is_materialized_and_hashed() {
    TemporaryDirectory temporary;
    const std::array entries{
        zip_fixture_entry("ignored.txt", "ignore"),
        zip_fixture_entry("images/system.img", "system-payload"),
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
        zip_fixture_entry("system.img", payload, [&](ZipFixtureEntry& entry) {
            entry.compressed = std::move(compressed);
            entry.method = 8U;
        }),
    };
    const auto archive = write_zip(temporary, entries, true);
    ArtifactSourceResolver resolver;
    auto resolved = resolver.resolve(archive, "system.img");
    CHECK(resolved);
    CHECK(read_source(*(*resolved)->source) == payload);
}

void per_entry_zip64_with_classic_eocd_is_supported_and_validated() {
    TemporaryDirectory valid_temporary;
    const std::array valid_entries{
        zip_fixture_entry("system.img", "hybrid-zip64-payload",
                          [](ZipFixtureEntry& entry) { entry.entry_zip64 = true; }),
    };
    const auto valid_archive = write_zip(valid_temporary, valid_entries, false);
    ArtifactSourceResolver valid_resolver;
    const auto valid = valid_resolver.resolve(valid_archive, "system.img");
    CHECK(valid);
    CHECK(read_source(*(*valid)->source) == "hybrid-zip64-payload");

    TemporaryDirectory inconsistent_temporary;
    const std::array inconsistent_entries{
        zip_fixture_entry("system.img", "payload", [](ZipFixtureEntry& entry) {
            entry.zip64_uncompressed_size = 8U;
            entry.entry_zip64 = true;
        }),
    };
    const auto inconsistent_archive =
        write_zip(inconsistent_temporary, inconsistent_entries, false);
    ArtifactSourceResolver inconsistent_resolver;
    const auto inconsistent =
        inconsistent_resolver.resolve(inconsistent_archive, "system.img");
    CHECK(!inconsistent);
    CHECK(inconsistent.error().kind == ArtifactSourceErrorKind::InvalidArchive ||
          inconsistent.error().kind == ArtifactSourceErrorKind::Integrity);
}

void crc_corruption_and_truncation_fail_closed() {
    TemporaryDirectory temporary;
    const std::array corrupt_entries{
        zip_fixture_entry("system.img", "payload", [](ZipFixtureEntry& entry) {
            entry.declared_crc = 0x12345678U;
        }),
    };
    const auto corrupt = write_zip(temporary, corrupt_entries);
    ArtifactSourceResolver resolver;
    const auto crc_result = resolver.resolve(corrupt, "system.img");
    CHECK(!crc_result);
    CHECK(crc_result.error().kind == ArtifactSourceErrorKind::Integrity ||
          crc_result.error().kind == ArtifactSourceErrorKind::InvalidArchive);

    const std::array valid_entries{
        zip_fixture_entry("system.img", "payload"),
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
            zip_fixture_entry(name, "payload"),
        };
        const auto archive = write_zip(temporary, entries);
        ArtifactSourceResolver resolver;
        const auto result = resolver.resolve(archive, "system.img");
        CHECK(!result);
        CHECK(result.error().kind == ArtifactSourceErrorKind::UnsafePath ||
              result.error().kind == ArtifactSourceErrorKind::InvalidArchive);
    }

    const std::array collision_entries{
        zip_fixture_entry("Images/system.img", "a"),
        zip_fixture_entry("images/system.img", "b"),
    };
    TemporaryDirectory collision_temp;
    const auto collision = write_zip(collision_temp, collision_entries);
    ArtifactSourceResolver collision_resolver;
    const auto collision_result =
        collision_resolver.resolve(collision, "images/system.img");
    CHECK(!collision_result);
    CHECK(collision_result.error().kind == ArtifactSourceErrorKind::UnsafePath);

    const std::array duplicate_entries{
        zip_fixture_entry("system.img", "a"),
        zip_fixture_entry("system.img", "b"),
    };
    TemporaryDirectory duplicate_temp;
    const auto duplicate = write_zip(duplicate_temp, duplicate_entries);
    ArtifactSourceResolver duplicate_resolver;
    const auto duplicate_result = duplicate_resolver.resolve(duplicate, "system.img");
    CHECK(!duplicate_result);
    CHECK(duplicate_result.error().kind == ArtifactSourceErrorKind::UnsafePath);

    const std::array conflict_entries{
        zip_fixture_entry("images", "file"),
        zip_fixture_entry("images/system.img", "payload"),
    };
    TemporaryDirectory conflict_temp;
    const auto conflict = write_zip(conflict_temp, conflict_entries);
    ArtifactSourceResolver conflict_resolver;
    const auto conflict_result =
        conflict_resolver.resolve(conflict, "images/system.img");
    CHECK(!conflict_result);
    CHECK(conflict_result.error().kind == ArtifactSourceErrorKind::UnsafePath);
}

void source_paths_reject_nul_and_win32_aliases_before_cache_keys() {
    TemporaryDirectory temporary;
    const auto image = temporary.path() / "boot.img";
    write_text(image, "payload");

    ArtifactSourceResolver container_resolver;
    const auto nul_container =
        container_resolver.resolve(with_embedded_nul(image));
    CHECK(!nul_container);
    CHECK(nul_container.error().kind == ArtifactSourceErrorKind::UnsafePath);

    ArtifactSourceLimits temporary_limits;
    temporary_limits.temporary_directory =
        with_embedded_nul(temporary.path());
    ArtifactSourceResolver temporary_resolver(temporary_limits);
    const auto nul_temporary = temporary_resolver.resolve(image);
    CHECK(!nul_temporary);
    CHECK(nul_temporary.error().kind == ArtifactSourceErrorKind::UnsafePath);

    const auto directory = temporary.path() / "images";
    std::filesystem::create_directory(directory);
    write_text(directory / "system.img", "system");

    const std::string nul_entry{"system.img\0alias", 16U};
    ArtifactSourceResolver entry_resolver;
    const auto embedded = entry_resolver.resolve(directory, nul_entry);
    CHECK(!embedded);
    CHECK(embedded.error().kind == ArtifactSourceErrorKind::UnsafePath);

    ArtifactSourceLimits short_name_limits;
    short_name_limits.max_name_bytes = 4U;
    ArtifactSourceResolver short_name_resolver(short_name_limits);
    const auto oversized = short_name_resolver.resolve(directory, "12345");
    CHECK(!oversized);
    CHECK(oversized.error().kind == ArtifactSourceErrorKind::LimitExceeded);

    constexpr std::array win32_aliases{
        std::string_view{"system.img."}, std::string_view{"system.img "},
        std::string_view{"NUL.img"}, std::string_view{"con"},
        std::string_view{"CONOUT$.txt"},
        std::string_view{"system.img:stream"},
    };
    for (const auto alias : win32_aliases) {
        ArtifactSourceResolver alias_resolver;
        const auto result = alias_resolver.resolve(directory, alias);
        CHECK(!result);
        CHECK(result.error().kind == ArtifactSourceErrorKind::UnsafePath);
    }
}

void metadata_features_and_local_name_mismatch_fail_closed() {
    TemporaryDirectory special_temp;
    const std::array special_entries{
        zip_fixture_entry("system.img", "target", [](ZipFixtureEntry& entry) {
            entry.external_attributes = 0120000U << 16U;
        }),
    };
    const auto special = write_zip(special_temp, special_entries);
    ArtifactSourceResolver special_resolver;
    const auto special_result = special_resolver.resolve(special, "system.img");
    CHECK(!special_result);
    CHECK(special_result.error().kind == ArtifactSourceErrorKind::UnsafePath);

    TemporaryDirectory encrypted_temp;
    const std::array encrypted_entries{
        zip_fixture_entry("system.img", "x",
                          [](ZipFixtureEntry& entry) { entry.flags = 1U; }),
    };
    const auto encrypted = write_zip(encrypted_temp, encrypted_entries);
    ArtifactSourceResolver encrypted_resolver;
    const auto encrypted_result = encrypted_resolver.resolve(encrypted, "system.img");
    CHECK(!encrypted_result);
    CHECK(encrypted_result.error().kind == ArtifactSourceErrorKind::UnsupportedFeature);

    TemporaryDirectory method_temp;
    const std::array method_entries{
        zip_fixture_entry("system.img", "x",
                          [](ZipFixtureEntry& entry) { entry.method = 12U; }),
    };
    const auto unsupported_method = write_zip(method_temp, method_entries);
    ArtifactSourceResolver method_resolver;
    const auto method_result =
        method_resolver.resolve(unsupported_method, "system.img");
    CHECK(!method_result);
    CHECK(method_result.error().kind == ArtifactSourceErrorKind::UnsupportedFeature);

    TemporaryDirectory mismatch_temp;
    const std::array mismatch_entries{
        zip_fixture_entry("system.img", "payload", [](ZipFixtureEntry& entry) {
            entry.local_name = std::string{"vendor.img"};
        }),
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
        zip_fixture_entry("one.img", "aa"),
        zip_fixture_entry("two.img", "bb"),
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
        zip_fixture_entry("bomb.img", large_payload, [&](ZipFixtureEntry& entry) {
            entry.compressed = std::vector(compressed.begin(), compressed.end());
            entry.method = 8U;
        }),
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
        zip_fixture_entry("bounded.img", bounded_payload,
                          [&](ZipFixtureEntry& entry) {
                              entry.compressed = std::move(bounded_compressed);
                              entry.declared_uncompressed_size = 10U;
                              entry.method = 8U;
                          }),
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
        zip_fixture_entry(std::string(512U, 'a'), "x"),
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

    const auto root = temporary.path() / "root";
    std::filesystem::create_directory(root);
    write_text(root / "system.img", "payload");
    ArtifactSourceResolver cancelled_beneath_resolver;
    auto cancelled_beneath = cancelled_beneath_resolver.resolve_file_beneath(
        root, "system.img", cancellation.get_token());
    CHECK(!cancelled_beneath);
    CHECK(cancelled_beneath.error().kind == ArtifactSourceErrorKind::Cancelled);

    ArtifactSourceLimits timeout_limits;
    timeout_limits.max_elapsed = std::chrono::milliseconds(0);
    ArtifactSourceResolver timeout_resolver(timeout_limits);
    auto timed_out = timeout_resolver.resolve(image);
    CHECK(!timed_out);
    CHECK(timed_out.error().kind == ArtifactSourceErrorKind::TimedOut);
    ArtifactSourceResolver timeout_beneath_resolver(timeout_limits);
    auto timed_out_beneath =
        timeout_beneath_resolver.resolve_file_beneath(root, "system.img");
    CHECK(!timed_out_beneath);
    CHECK(timed_out_beneath.error().kind == ArtifactSourceErrorKind::TimedOut);

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

void cancelled_and_timed_out_owners_do_not_poison_waiters() {
    TemporaryDirectory temporary;
    const auto image = temporary.path() / "super.img";
    write_text(image, std::string(256U * 1024U, 'k'));

    std::mutex gate_mutex;
    std::condition_variable gate_ready;
    bool first_provider_entered = false;
    bool release_first_provider = false;
    std::atomic<std::uint32_t> provider_calls{};
    ArtifactSourceLimits cancellation_limits;
    cancellation_limits.max_spool_bytes = 256U * 1024U;
    cancellation_limits.available_space_provider =
        [&](const std::filesystem::path&)
        -> std::expected<std::uint64_t, std::error_code> {
        const auto call = provider_calls.fetch_add(1U, std::memory_order_relaxed);
        if (call == 0U) {
            std::unique_lock lock(gate_mutex);
            first_provider_entered = true;
            gate_ready.notify_all();
            gate_ready.wait(lock, [&] { return release_first_provider; });
        }
        return 256U * 1024U;
    };
    ArtifactSourceResolver cancellation_resolver(cancellation_limits);
    std::stop_source cancellation;
    using ResolveResult = std::expected<
        std::shared_ptr<const kairosboot::image::ResolvedArtifact>,
        kairosboot::image::ArtifactSourceError>;
    std::optional<ResolveResult> owner_result;
    std::optional<ResolveResult> waiter_result;
    std::thread owner([&] {
        owner_result.emplace(
            cancellation_resolver.resolve(image, {}, cancellation.get_token()));
    });
    {
        std::unique_lock lock(gate_mutex);
        gate_ready.wait(lock, [&] { return first_provider_entered; });
    }
    std::atomic<bool> waiter_started{};
    std::thread waiter([&] {
        waiter_started.store(true, std::memory_order_release);
        waiter_result.emplace(cancellation_resolver.resolve(image));
    });
    while (!waiter_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    cancellation.request_stop();
    {
        std::lock_guard lock(gate_mutex);
        release_first_provider = true;
    }
    gate_ready.notify_all();
    owner.join();
    waiter.join();
    CHECK(owner_result && !*owner_result);
    CHECK(owner_result->error().kind == ArtifactSourceErrorKind::Cancelled);
    CHECK(waiter_result && *waiter_result);
    CHECK(provider_calls.load(std::memory_order_relaxed) == 2U);

    const auto timeout_image = temporary.path() / "timeout.img";
    write_text(timeout_image, std::string(4U * 1024U, 't'));
    std::atomic<std::uint32_t> timeout_provider_calls{};
    ArtifactSourceLimits timeout_limits;
    timeout_limits.max_elapsed = std::chrono::milliseconds(500);
    timeout_limits.max_spool_bytes = 256U * 1024U;
    timeout_limits.available_space_provider =
        [&](const std::filesystem::path&)
        -> std::expected<std::uint64_t, std::error_code> {
        const auto call =
            timeout_provider_calls.fetch_add(1U, std::memory_order_relaxed);
        if (call == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
        return 256U * 1024U;
    };
    ArtifactSourceResolver timeout_resolver(timeout_limits);
    const auto timed_out = timeout_resolver.resolve(timeout_image);
    CHECK(!timed_out);
    CHECK(timed_out.error().kind == ArtifactSourceErrorKind::TimedOut);
    const auto retried = timeout_resolver.resolve(timeout_image);
    CHECK(retried);
    CHECK(timeout_provider_calls.load(std::memory_order_relaxed) == 2U);
}

void different_keys_share_one_batch_disk_reservation() {
    TemporaryDirectory temporary;
    const auto first_path = temporary.path() / "first.img";
    const auto second_path = temporary.path() / "second.img";
    write_text(first_path, "12345678");
    write_text(second_path, "abcdefgh");

    std::atomic<std::uint32_t> space_calls{};
    ArtifactSourceLimits limits;
    limits.minimum_free_space_bytes = 5U;
    limits.available_space_provider =
        [&](const std::filesystem::path&)
        -> std::expected<std::uint64_t, std::error_code> {
        space_calls.fetch_add(1U, std::memory_order_relaxed);
        return 20U;
    };
    ArtifactSourceResolver resolver(limits);
    using ResolveResult = std::expected<
        std::shared_ptr<const kairosboot::image::ResolvedArtifact>,
        kairosboot::image::ArtifactSourceError>;
    std::array<std::optional<ResolveResult>, 2> results;
    std::atomic<std::uint32_t> ready{};
    std::atomic<bool> go{};
    std::array<std::thread, 2> workers{
        std::thread([&] {
            ready.fetch_add(1U, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[0].emplace(resolver.resolve(first_path));
        }),
        std::thread([&] {
            ready.fetch_add(1U, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[1].emplace(resolver.resolve(second_path));
        }),
    };
    while (ready.load(std::memory_order_acquire) != workers.size()) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(results[0] && results[1]);
    const auto first_succeeded = static_cast<bool>(*results[0]);
    const auto second_succeeded = static_cast<bool>(*results[1]);
    CHECK(first_succeeded != second_succeeded);
    const auto& rejected = first_succeeded ? *results[1] : *results[0];
    CHECK(rejected.error().kind == ArtifactSourceErrorKind::LimitExceeded);
    CHECK(space_calls.load(std::memory_order_relaxed) == 2U);

    const auto cached = resolver.resolve(first_succeeded ? first_path : second_path);
    CHECK(cached);
    const auto cached_rejection =
        resolver.resolve(first_succeeded ? second_path : first_path);
    CHECK(!cached_rejection);
    CHECK(cached_rejection.error().kind == ArtifactSourceErrorKind::LimitExceeded);
    CHECK(space_calls.load(std::memory_order_relaxed) == 2U);
}

void different_zip_keys_share_one_metadata_concurrency_ceiling() {
    TemporaryDirectory first_temporary;
    TemporaryDirectory second_temporary;
    const std::array first_entries{
        zip_fixture_entry("system.img", "first"),
    };
    const std::array second_entries{
        zip_fixture_entry("vendor.img", "second"),
    };
    const auto first_archive = write_zip(first_temporary, first_entries);
    const auto second_archive = write_zip(second_temporary, second_entries);

    std::mutex observer_mutex;
    std::condition_variable observer_changed;
    std::uint32_t observer_calls = 0U;
    bool release_first = false;
    ArtifactSourceLimits limits;
    limits.archive_reader_observer = [&] {
        std::unique_lock lock(observer_mutex);
        ++observer_calls;
        observer_changed.notify_all();
        if (observer_calls == 1U) {
            observer_changed.wait(lock, [&] { return release_first; });
        }
    };
    ArtifactSourceResolver resolver(limits);
    using ResolveResult = std::expected<
        std::shared_ptr<const kairosboot::image::ResolvedArtifact>,
        kairosboot::image::ArtifactSourceError>;
    std::optional<ResolveResult> first_result;
    std::optional<ResolveResult> second_result;
    std::thread first([&] {
        first_result.emplace(resolver.resolve(first_archive, "system.img"));
    });
    {
        std::unique_lock lock(observer_mutex);
        observer_changed.wait(lock, [&] { return observer_calls == 1U; });
    }
    std::atomic<bool> second_started{};
    std::thread second([&] {
        second_started.store(true, std::memory_order_release);
        second_result.emplace(resolver.resolve(second_archive, "vendor.img"));
    });
    while (!second_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    bool concurrent_reader = false;
    {
        std::unique_lock lock(observer_mutex);
        concurrent_reader = observer_changed.wait_for(
            lock, std::chrono::milliseconds(50),
            [&] { return observer_calls > 1U; });
        release_first = true;
    }
    observer_changed.notify_all();
    first.join();
    second.join();
    CHECK(!concurrent_reader);
    CHECK(first_result && *first_result);
    CHECK(second_result && *second_result);
    CHECK(observer_calls == 2U);
}

void allocation_failure_publishes_one_complete_cached_error() {
    TemporaryDirectory temporary;
    const auto image = temporary.path() / "vendor.img";
    write_text(image, "payload");
    std::atomic<std::uint32_t> provider_calls{};
    ArtifactSourceLimits limits;
    limits.available_space_provider =
        [&](const std::filesystem::path&)
        -> std::expected<std::uint64_t, std::error_code> {
        provider_calls.fetch_add(1U, std::memory_order_relaxed);
        throw std::bad_alloc();
    };
    ArtifactSourceResolver resolver(limits);
    constexpr std::size_t thread_count = 8U;
    std::array<std::optional<kairosboot::image::ArtifactSourceError>, thread_count>
        errors;
    std::array<std::thread, thread_count> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            const auto result = resolver.resolve(image);
            if (!result) {
                errors[index] = result.error();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(provider_calls.load(std::memory_order_relaxed) == 1U);
    for (const auto& error : errors) {
        CHECK(error);
        CHECK(error->kind == ArtifactSourceErrorKind::Io);
        CHECK(!error->message.empty());
        CHECK(error->message == errors.front()->message);
    }
}

#if !defined(_WIN32)
void spool_descriptor_is_closed_across_exec() {
    TemporaryDirectory temporary;
    const auto image = temporary.path() / "boot.img";
    write_text(image, "payload");
    const auto before = open_descriptors();
    ArtifactSourceResolver resolver;
    const auto resolved = resolver.resolve(image);
    CHECK(resolved);
    const auto after = open_descriptors();

    std::vector<int> candidates;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(candidates));
    CHECK(candidates.size() == 1U);
    const int descriptor = candidates.front();
    const int flags = ::fcntl(descriptor, F_GETFD);
    CHECK(flags >= 0);
    CHECK((flags & FD_CLOEXEC) != 0);
    struct stat identity {};
    CHECK(::fstat(descriptor, &identity) == 0);

    const auto descriptor_text = std::to_string(descriptor);
    const auto device_text =
        std::to_string(static_cast<std::uint64_t>(identity.st_dev));
    const auto inode_text =
        std::to_string(static_cast<std::uint64_t>(identity.st_ino));
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const auto executable = executable_path.native();
        ::execl(executable.c_str(), executable.c_str(), "--probe-closed-fd",
                descriptor_text.c_str(), device_text.c_str(), inode_text.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
#else
void windows_spools_use_unpredictable_private_create_new_files() {
    TemporaryDirectory temporary;
    const auto first = temporary.path() / "boot.img";
    const auto second = temporary.path() / "vendor.img";
    write_text(first, "first");
    write_text(second, "second");
    ArtifactSourceLimits limits;
    limits.temporary_directory = temporary.path();
    ArtifactSourceResolver resolver(limits);
    CHECK(resolver.resolve(first));
    CHECK(resolver.resolve(second));

    const auto pattern = temporary.path() / L"kairosboot-spool-*.tmp";
    WIN32_FIND_DATAW data{};
    const auto search = ::FindFirstFileW(pattern.c_str(), &data);
    CHECK(search != INVALID_HANDLE_VALUE);
    std::set<std::wstring> names;
    constexpr std::wstring_view prefix = L"kairosboot-spool-";
    constexpr std::wstring_view suffix = L".tmp";
    do {
        const std::wstring_view name(data.cFileName);
        CHECK(name.starts_with(prefix));
        CHECK(name.ends_with(suffix));
        const auto random = name.substr(
            prefix.size(), name.size() - prefix.size() - suffix.size());
        CHECK(random.size() == 32U);
        CHECK(std::ranges::all_of(random, [](const wchar_t character) {
            return (character >= L'0' && character <= L'9') ||
                   (character >= L'a' && character <= L'f');
        }));
        names.emplace(name);

        const auto candidate = temporary.path() / name;
        const auto bypass = ::CreateFileW(
            candidate.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(bypass == INVALID_HANDLE_VALUE);
        CHECK(::GetLastError() == ERROR_SHARING_VIOLATION ||
              ::GetLastError() == ERROR_ACCESS_DENIED ||
              ::GetLastError() == ERROR_FILE_NOT_FOUND);
    } while (::FindNextFileW(search, &data) != FALSE);
    (void)::FindClose(search);
    CHECK(names.size() == 2U);
}
#endif

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

int main(const int argument_count, char** arguments) {
#if !defined(_WIN32)
    if (argument_count == 5 &&
        std::string_view(arguments[1]) == "--probe-closed-fd") {
        const int descriptor = static_cast<int>(std::strtol(arguments[2], nullptr, 10));
        const auto expected_device = std::strtoull(arguments[3], nullptr, 10);
        const auto expected_inode = std::strtoull(arguments[4], nullptr, 10);
        struct stat identity {};
        if (::fstat(descriptor, &identity) != 0) {
            return errno == EBADF ? 0 : 2;
        }
        const bool inherited_same_spool =
            static_cast<std::uint64_t>(identity.st_dev) == expected_device &&
            static_cast<std::uint64_t>(identity.st_ino) == expected_inode;
        return inherited_same_spool ? 3 : 0;
    }
    executable_path = std::filesystem::absolute(arguments[0]);
#else
    (void)argument_count;
    (void)arguments;
#endif
    const std::array tests{
        Test{"direct_and_directory_sources_are_immutable_snapshots",
             direct_and_directory_sources_are_immutable_snapshots},
        Test{"root_relative_direct_files_are_confined_to_the_selected_boundary",
             root_relative_direct_files_are_confined_to_the_selected_boundary},
        Test{"root_and_ancestor_replacement_after_capture_cannot_redirect_resolution",
             root_and_ancestor_replacement_after_capture_cannot_redirect_resolution},
        Test{"concurrent_root_relative_resolves_share_the_captured_boundary",
             concurrent_root_relative_resolves_share_the_captured_boundary},
        Test{"root_relative_parent_replacement_race_never_publishes_outside_bytes",
             root_relative_parent_replacement_race_never_publishes_outside_bytes},
        Test{"stored_non_first_entry_is_materialized_and_hashed",
             stored_non_first_entry_is_materialized_and_hashed},
        Test{"deflate_and_zip64_entries_are_supported",
             deflate_and_zip64_entries_are_supported},
        Test{"per_entry_zip64_with_classic_eocd_is_supported_and_validated",
             per_entry_zip64_with_classic_eocd_is_supported_and_validated},
        Test{"crc_corruption_and_truncation_fail_closed",
             crc_corruption_and_truncation_fail_closed},
        Test{"unsafe_duplicate_and_conflicting_paths_fail_closed",
             unsafe_duplicate_and_conflicting_paths_fail_closed},
        Test{"source_paths_reject_nul_and_win32_aliases_before_cache_keys",
             source_paths_reject_nul_and_win32_aliases_before_cache_keys},
        Test{"metadata_features_and_local_name_mismatch_fail_closed",
             metadata_features_and_local_name_mismatch_fail_closed},
        Test{"entry_ratio_aggregate_disk_and_name_limits_are_enforced",
             entry_ratio_aggregate_disk_and_name_limits_are_enforced},
        Test{"cancellation_timeout_and_symlink_checks_precede_publication",
             cancellation_timeout_and_symlink_checks_precede_publication},
        Test{"concurrent_resolves_publish_one_shared_snapshot",
             concurrent_resolves_publish_one_shared_snapshot},
        Test{"cancelled_and_timed_out_owners_do_not_poison_waiters",
             cancelled_and_timed_out_owners_do_not_poison_waiters},
        Test{"different_keys_share_one_batch_disk_reservation",
             different_keys_share_one_batch_disk_reservation},
        Test{"different_zip_keys_share_one_metadata_concurrency_ceiling",
             different_zip_keys_share_one_metadata_concurrency_ceiling},
        Test{"allocation_failure_publishes_one_complete_cached_error",
             allocation_failure_publishes_one_complete_cached_error},
#if !defined(_WIN32)
        Test{"spool_descriptor_is_closed_across_exec",
             spool_descriptor_is_closed_across_exec},
#else
        Test{"Windows spools use unpredictable private create-new files",
             windows_spools_use_unpredictable_private_create_new_files},
#endif
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
