// SPDX-License-Identifier: MIT
#include "src/fastboot/update_package_preflight.hpp"
#include "src/image/sha256.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::preflight_update_package;
using kairosboot::fastboot::UpdatePackagePreflightErrorKind;
using kairosboot::fastboot::UpdatePackagePreflightLimits;
using kairosboot::fastboot::UpdateTaskKind;
using kairosboot::image::ArtifactSourceErrorKind;
using kairosboot::image::ArtifactSourceResolver;
using kairosboot::image::sha256_hex;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                               \
    do {                                                                               \
        if (!(condition)) {                                                            \
            throw CheckFailure(std::string("check failed at line ") +                  \
                               std::to_string(__LINE__) + ": " #condition);            \
        }                                                                              \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{};
        const auto suffix = sequence.fetch_add(1U, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                ("kairosboot-update-preflight-test-" + std::to_string(suffix));
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

struct ZipEntry final {
    std::string name;
    std::string payload;
    std::optional<std::uint32_t> declared_crc;
};

[[nodiscard]] std::vector<std::byte>
make_stored_zip(const std::span<const ZipEntry> entries) {
    struct CentralRecord final {
        const ZipEntry* entry{};
        std::uint32_t local_offset{};
        std::uint32_t crc{};
    };

    std::vector<std::byte> output;
    std::vector<CentralRecord> central;
    for (const auto& entry : entries) {
        CHECK(output.size() <= UINT32_MAX);
        CHECK(entry.payload.size() <= UINT32_MAX);
        const auto declared_crc = entry.declared_crc.value_or(crc32(entry.payload));
        central.push_back(CentralRecord{
            .entry = &entry,
            .local_offset = static_cast<std::uint32_t>(output.size()),
            .crc = declared_crc,
        });

        append_u32(output, 0x04034b50U);
        append_u16(output, 20U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, declared_crc);
        append_u32(output, static_cast<std::uint32_t>(entry.payload.size()));
        append_u32(output, static_cast<std::uint32_t>(entry.payload.size()));
        append_u16(output, static_cast<std::uint16_t>(entry.name.size()));
        append_u16(output, 0U);
        append_string(output, entry.name);
        append_string(output, entry.payload);
    }

    CHECK(output.size() <= UINT32_MAX);
    const auto central_offset = static_cast<std::uint32_t>(output.size());
    for (const auto& record : central) {
        append_u32(output, 0x02014b50U);
        append_u16(output, static_cast<std::uint16_t>((3U << 8U) | 20U));
        append_u16(output, 20U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, record.crc);
        append_u32(output, static_cast<std::uint32_t>(record.entry->payload.size()));
        append_u32(output, static_cast<std::uint32_t>(record.entry->payload.size()));
        append_u16(output, static_cast<std::uint16_t>(record.entry->name.size()));
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, 0100000U << 16U);
        append_u32(output, record.local_offset);
        append_string(output, record.entry->name);
    }
    CHECK(output.size() - central_offset <= UINT32_MAX);
    CHECK(entries.size() <= UINT16_MAX);
    const auto central_size =
        static_cast<std::uint32_t>(output.size() - central_offset);

    append_u32(output, 0x06054b50U);
    append_u16(output, 0U);
    append_u16(output, 0U);
    append_u16(output, static_cast<std::uint16_t>(entries.size()));
    append_u16(output, static_cast<std::uint16_t>(entries.size()));
    append_u32(output, central_size);
    append_u32(output, central_offset);
    append_u16(output, 0U);
    return output;
}

void write_bytes(const std::filesystem::path& path,
                 const std::span<const std::byte> bytes) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw CheckFailure("unable to write update preflight fixture");
    }
}

void write_text(const std::filesystem::path& path, const std::string_view contents) {
    const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
    write_bytes(path, bytes);
}

[[nodiscard]] std::filesystem::path
write_zip(const TemporaryDirectory& temporary, const std::span<const ZipEntry> entries,
          const std::string_view name = "package.zip") {
    const auto path = temporary.path() / name;
    const auto bytes = make_stored_zip(entries);
    write_bytes(path, bytes);
    return path;
}

void create_directory_package(
    const std::filesystem::path& path, const std::string_view android_info,
    const std::optional<std::string_view> fastboot_info = std::nullopt) {
    std::filesystem::create_directory(path);
    write_text(path / "android-info.txt", android_info);
    if (fastboot_info) {
        write_text(path / "fastboot-info.txt", *fastboot_info);
    }
}

void required_manifest_missing_duplicate_and_bounds_fail_closed() {
    TemporaryDirectory temporary;
    const auto missing = temporary.path() / "missing";
    std::filesystem::create_directory(missing);
    ArtifactSourceResolver missing_resolver;
    auto missing_result = preflight_update_package(missing_resolver, missing, false);
    CHECK(!missing_result);
    CHECK(missing_result.error().kind ==
          UpdatePackagePreflightErrorKind::MissingAndroidInfo);

    const auto android_only = temporary.path() / "android-only";
    create_directory_package(android_only, "");
    ArtifactSourceResolver android_only_resolver;
    auto without_optional_manifest =
        preflight_update_package(android_only_resolver, android_only, false);
    CHECK(without_optional_manifest);
    CHECK(without_optional_manifest->plan.tasks.empty());
    CHECK(without_optional_manifest->artifacts.empty());

    const std::array duplicate_entries{
        ZipEntry{.name = "android-info.txt", .payload = ""},
        ZipEntry{.name = "android-info.txt", .payload = "product=atlas\n"},
    };
    const auto duplicate_zip =
        write_zip(temporary, duplicate_entries, "duplicate-manifest.zip");
    ArtifactSourceResolver duplicate_resolver;
    auto duplicate = preflight_update_package(duplicate_resolver, duplicate_zip, false);
    CHECK(!duplicate);
    CHECK(duplicate.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(duplicate.error().artifact_error.has_value());
    CHECK(duplicate.error().artifact_error->kind ==
          ArtifactSourceErrorKind::UnsafePath);

    const auto oversized = temporary.path() / "oversized";
    create_directory_package(oversized, "product=atlas\n");
    UpdatePackagePreflightLimits small_limits;
    small_limits.manifest.maximum_file_bytes = 4U;
    ArtifactSourceResolver oversized_resolver;
    auto too_large =
        preflight_update_package(oversized_resolver, oversized, false, small_limits);
    CHECK(!too_large);
    CHECK(too_large.error().kind == UpdatePackagePreflightErrorKind::LimitExceeded);

    const auto embedded_nul = temporary.path() / "embedded-nul";
    std::string nul_manifest{"product=atl"};
    nul_manifest.push_back('\0');
    nul_manifest += "as\n";
    create_directory_package(embedded_nul, nul_manifest);
    ArtifactSourceResolver nul_resolver;
    auto nul = preflight_update_package(nul_resolver, embedded_nul, false);
    CHECK(!nul);
    CHECK(nul.error().kind == UpdatePackagePreflightErrorKind::Manifest);
    CHECK(nul.error().manifest_error.has_value());
    CHECK(nul.error().manifest_error->code ==
          kairosboot::fastboot::UpdatePlanErrorCode::EmbeddedNul);
}

void inactive_wipe_paths_are_validated_but_unselected_artifacts_are_not_opened() {
    TemporaryDirectory temporary;
    const auto unsafe = temporary.path() / "unsafe";
    create_directory_package(unsafe, "", "if-wipe flash boot ../outside.img\n");
    ArtifactSourceResolver unsafe_resolver;
    auto unsafe_result = preflight_update_package(unsafe_resolver, unsafe, false);
    CHECK(!unsafe_result);
    CHECK(unsafe_result.error().kind == UpdatePackagePreflightErrorKind::Manifest);
    CHECK(unsafe_result.error().manifest_error->code ==
          kairosboot::fastboot::UpdatePlanErrorCode::UnsafeArtifactPath);

    const auto inactive = temporary.path() / "inactive";
    create_directory_package(inactive, "",
                             "if-wipe flash boot intentionally-missing.img\n");
    ArtifactSourceResolver inactive_resolver;
    auto without_wipe = preflight_update_package(inactive_resolver, inactive, false);
    CHECK(without_wipe);
    CHECK(without_wipe->plan.tasks.empty());
    CHECK(without_wipe->artifacts.empty());

    auto with_wipe = preflight_update_package(inactive_resolver, inactive, true);
    CHECK(!with_wipe);
    CHECK(with_wipe.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(with_wipe.error().artifact == "intentionally-missing.img");
}

void duplicate_references_share_one_immutable_materialization() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "shared";
    create_directory_package(package, "",
                             "flash boot images/shared.img\n"
                             "flash vendor images/shared.img\n");
    write_text(package / "images/shared.img", "shared-payload");

    ArtifactSourceResolver resolver;
    auto prewarmed = resolver.resolve(package, "images/shared.img");
    CHECK(prewarmed);
    auto prepared = preflight_update_package(resolver, package, false);
    CHECK(prepared);
    CHECK(prepared->plan.tasks.size() == 2U);
    CHECK(prepared->artifacts.size() == 1U);
    CHECK(prepared->artifacts[0].resolved == *prewarmed);
    CHECK(prepared->artifacts[0].artifact.metadata().transfer_size == 14U);
    CHECK(sha256_hex(prepared->artifacts[0].resolved->sha256) ==
          sha256_hex((*prewarmed)->sha256));

    std::filesystem::remove(package / "images/shared.img");
    auto repeated = preflight_update_package(resolver, package, false);
    CHECK(repeated);
    CHECK(repeated->artifacts[0].resolved == prepared->artifacts[0].resolved);
}

void transport_free_tasks_and_device_requirements_remain_explicit() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "no-artifacts";
    create_directory_package(package, "product=atlas\n",
                             "reboot fastboot\nerase cache\nupdate-super\n");
    ArtifactSourceResolver resolver;
    auto prepared = preflight_update_package(resolver, package, false);
    CHECK(prepared);
    CHECK(prepared->plan.tasks.size() == 3U);
    CHECK(prepared->artifacts.empty());
    CHECK(prepared->requires_device_validation);
    CHECK(prepared->plan.requirements.size() == 1U);
}

void directory_and_zip_packages_produce_equivalent_prepared_plans() {
    TemporaryDirectory temporary;
    constexpr std::string_view android_info = "product=atlas|boreal\n";
    constexpr std::string_view fastboot_info =
        "version 1\nflash boot images/boot.img\nreboot fastboot\n";
    constexpr std::string_view image = "boot-image-payload";

    const auto directory = temporary.path() / "directory";
    create_directory_package(directory, android_info, fastboot_info);
    write_text(directory / "images/boot.img", image);

    const std::array entries{
        ZipEntry{.name = "ignored.txt", .payload = "first"},
        ZipEntry{.name = "android-info.txt", .payload = std::string(android_info)},
        ZipEntry{.name = "fastboot-info.txt", .payload = std::string(fastboot_info)},
        ZipEntry{.name = "images/boot.img", .payload = std::string(image)},
    };
    const auto archive = write_zip(temporary, entries, "parity.zip");

    ArtifactSourceResolver directory_resolver;
    ArtifactSourceResolver zip_resolver;
    auto from_directory =
        preflight_update_package(directory_resolver, directory, false);
    auto from_zip = preflight_update_package(zip_resolver, archive, false);
    CHECK(from_directory && from_zip);
    CHECK(from_directory->plan.tasks.size() == from_zip->plan.tasks.size());
    CHECK(from_directory->plan.requirements.size() ==
          from_zip->plan.requirements.size());
    CHECK(from_directory->artifacts.size() == 1U);
    CHECK(from_zip->artifacts.size() == 1U);
    CHECK(from_directory->plan.tasks[0].kind == UpdateTaskKind::Flash);
    CHECK(from_directory->plan.tasks[0].artifact == from_zip->plan.tasks[0].artifact);
    CHECK(from_directory->plan.tasks[1].kind == UpdateTaskKind::Reboot);
    CHECK(from_directory->artifacts[0].resolved->sha256 ==
          from_zip->artifacts[0].resolved->sha256);
}

void crc_missing_and_budget_failures_publish_no_prepared_plan() {
    TemporaryDirectory temporary;
    const std::array crc_entries{
        ZipEntry{.name = "android-info.txt", .payload = ""},
        ZipEntry{
            .name = "fastboot-info.txt",
            .payload = "flash boot boot.img\n",
        },
        ZipEntry{
            .name = "boot.img",
            .payload = "corrupt",
            .declared_crc = 0x12345678U,
        },
    };
    const auto corrupt = write_zip(temporary, crc_entries, "corrupt.zip");
    ArtifactSourceResolver crc_resolver;
    auto crc = preflight_update_package(crc_resolver, corrupt, false);
    CHECK(!crc);
    CHECK(crc.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(crc.error().artifact == "boot.img");
    CHECK(crc.error().artifact_error.has_value());
    CHECK(crc.error().artifact_error->kind == ArtifactSourceErrorKind::Integrity ||
          crc.error().artifact_error->kind == ArtifactSourceErrorKind::InvalidArchive);

    const auto invalid_image = temporary.path() / "invalid-image";
    create_directory_package(invalid_image, "", "flash boot malformed-sparse.img\n");
    const std::array malformed_sparse{
        std::byte{0x3a},
        std::byte{0xff},
        std::byte{0x26},
        std::byte{0xed},
    };
    write_bytes(invalid_image / "malformed-sparse.img", malformed_sparse);
    ArtifactSourceResolver invalid_image_resolver;
    auto invalid =
        preflight_update_package(invalid_image_resolver, invalid_image, false);
    CHECK(!invalid);
    CHECK(invalid.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(invalid.error().artifact_error.has_value());
    CHECK(invalid.error().artifact_error->kind ==
          ArtifactSourceErrorKind::InvalidImage);

    const auto missing = temporary.path() / "missing-second";
    create_directory_package(missing, "",
                             "flash boot first.img\nflash vendor missing.img\n");
    write_text(missing / "first.img", "first");
    ArtifactSourceResolver missing_resolver;
    auto partial = preflight_update_package(missing_resolver, missing, false);
    CHECK(!partial);
    CHECK(partial.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(partial.error().artifact == "missing.img");

    const auto limited = temporary.path() / "limited";
    create_directory_package(limited, "",
                             "flash boot first.img\nflash vendor second.img\n");
    write_text(limited / "first.img", "first");
    write_text(limited / "second.img", "second");

    UpdatePackagePreflightLimits count_limits;
    count_limits.maximum_unique_artifacts = 1U;
    ArtifactSourceResolver count_resolver;
    auto count = preflight_update_package(count_resolver, limited, false, count_limits);
    CHECK(!count);
    CHECK(count.error().kind == UpdatePackagePreflightErrorKind::LimitExceeded);

    UpdatePackagePreflightLimits byte_limits;
    byte_limits.maximum_total_artifact_bytes = 10U;
    ArtifactSourceResolver byte_resolver;
    auto bytes = preflight_update_package(byte_resolver, limited, false, byte_limits);
    CHECK(!bytes);
    CHECK(bytes.error().kind == UpdatePackagePreflightErrorKind::LimitExceeded);
    CHECK(bytes.error().artifact == "second.img");
}

void cancellation_precedes_manifest_and_artifact_work() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "cancelled";
    create_directory_package(package, "", "flash boot boot.img\n");
    write_text(package / "boot.img", "boot");
    std::stop_source cancelled;
    cancelled.request_stop();
    ArtifactSourceResolver resolver;
    auto result =
        preflight_update_package(resolver, package, false, {}, cancelled.get_token());
    CHECK(!result);
    CHECK(result.error().kind == UpdatePackagePreflightErrorKind::Cancelled);
}

struct Test final {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main() {
    const std::array tests{
        Test{"required manifest validation",
             required_manifest_missing_duplicate_and_bounds_fail_closed},
        Test{"wipe path validation",
             inactive_wipe_paths_are_validated_but_unselected_artifacts_are_not_opened},
        Test{"single materialization for duplicate references",
             duplicate_references_share_one_immutable_materialization},
        Test{"transport-free tasks and device requirements",
             transport_free_tasks_and_device_requirements_remain_explicit},
        Test{"directory and ZIP parity",
             directory_and_zip_packages_produce_equivalent_prepared_plans},
        Test{"artifact and budget failures",
             crc_missing_and_budget_failures_publish_no_prepared_plan},
        Test{"preflight cancellation",
             cancellation_precedes_manifest_and_artifact_work},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
