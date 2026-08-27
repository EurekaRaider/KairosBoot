// SPDX-License-Identifier: MIT
#include "src/fastboot/update_package_preflight.hpp"
#include "src/image/sha256.hpp"
#include "tests/fastboot/aosp_hardcoded_image_inventory_37_0_1.hpp"

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

namespace {

using kairosboot::fastboot::preflight_update_package;
using kairosboot::fastboot::PlannedSlot;
using kairosboot::fastboot::PreparedUpdatePackage;
using kairosboot::fastboot::UpdatePackagePreflightErrorKind;
using kairosboot::fastboot::UpdatePackagePreflightLimits;
using kairosboot::fastboot::UpdateSuperPreparationState;
using kairosboot::fastboot::UpdateTaskKind;
using kairosboot::image::ArtifactSourceErrorKind;
using kairosboot::image::ArtifactSourceLimits;
using kairosboot::image::ArtifactSourceResolver;
using kairosboot::image::FlashArtifactKind;
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

[[nodiscard]] std::vector<std::byte> valid_empty_sparse_image() {
    std::vector<std::byte> bytes;
    append_u32(bytes, kairosboot::image::kAndroidSparseMagic);
    append_u16(bytes, kairosboot::image::kAndroidSparseMajorVersion);
    append_u16(bytes, 0U);
    append_u16(bytes, 28U);
    append_u16(bytes, 12U);
    append_u32(bytes, 4096U);
    append_u32(bytes, 0U);
    append_u32(bytes, 0U);
    append_u32(bytes, 0U);
    CHECK(bytes.size() == 28U);
    return bytes;
}

[[nodiscard]] std::string binary_string(
    const std::span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] std::string hardcoded_payload(const std::string_view image_name) {
    return "frozen-aosp-37.0.1:" + std::string(image_name);
}

[[nodiscard]] bool selected_hardcoded_image(
    const kairosboot::test::aosp_37_0_1::Image& image,
    const bool include_optional) noexcept {
    return image.type != kairosboot::test::aosp_37_0_1::ImageType::Extra &&
           (!image.optional_if_missing || include_optional);
}

void write_hardcoded_images(const std::filesystem::path& package,
                            const bool include_optional,
                            const bool include_extra = true) {
    for (const auto& image : kairosboot::test::aosp_37_0_1::kImages) {
        if ((!image.optional_if_missing || include_optional) &&
            (include_extra ||
             image.type != kairosboot::test::aosp_37_0_1::ImageType::Extra)) {
            write_text(package / image.image_name,
                       hardcoded_payload(image.image_name));
        }
    }
}

[[nodiscard]] std::vector<ZipEntry> hardcoded_zip_entries(
    const std::string_view android_info,
    const bool include_optional,
    const bool include_extra = true) {
    std::vector<ZipEntry> entries;
    entries.push_back(
        ZipEntry{.name = "android-info.txt", .payload = std::string(android_info)});
    for (const auto& image : kairosboot::test::aosp_37_0_1::kImages) {
        if ((!image.optional_if_missing || include_optional) &&
            (include_extra ||
             image.type != kairosboot::test::aosp_37_0_1::ImageType::Extra)) {
            entries.push_back(ZipEntry{
                .name = std::string(image.image_name),
                .payload = hardcoded_payload(image.image_name),
            });
        }
    }
    return entries;
}

[[nodiscard]] bool expected_apply_vbmeta(
    const std::string_view partition) noexcept {
    return partition == "vbmeta" || partition == "vbmeta_system" ||
           partition == "vbmeta_vendor";
}

void check_hardcoded_plan(const PreparedUpdatePackage& prepared,
                          const bool include_optional,
                          const bool includes_update_super) {
    std::vector<const kairosboot::test::aosp_37_0_1::Image*> boot;
    std::vector<const kairosboot::test::aosp_37_0_1::Image*> normal;
    for (const auto& image : kairosboot::test::aosp_37_0_1::kImages) {
        if (!selected_hardcoded_image(image, include_optional)) {
            continue;
        }
        (image.type == kairosboot::test::aosp_37_0_1::ImageType::BootCritical
             ? boot
             : normal)
            .push_back(&image);
    }

    CHECK(prepared.plan.tasks.size() ==
          boot.size() + (includes_update_super ? 1U : 0U) + normal.size());
    CHECK(prepared.artifacts.size() == boot.size() + normal.size());
    std::size_t task_index = 0;
    std::size_t artifact_index = 0;
    const auto check_images = [&](const auto& images) {
        for (const auto* image : images) {
            const auto& task = prepared.plan.tasks[task_index++];
            CHECK(task.kind == UpdateTaskKind::Flash);
            CHECK(task.partition == image->partition);
            CHECK(task.artifact == image->image_name);
            CHECK(task.slot == (image->nickname.empty() ? PlannedSlot::Other
                                                        : PlannedSlot::Default));
            CHECK(task.apply_vbmeta == expected_apply_vbmeta(image->partition));
            CHECK(prepared.artifacts[artifact_index++].name == image->image_name);
        }
    };
    check_images(boot);
    if (includes_update_super) {
        CHECK(prepared.plan.tasks[task_index++].kind ==
              UpdateTaskKind::UpdateSuper);
    }
    check_images(normal);
    CHECK(task_index == prepared.plan.tasks.size());
    CHECK(artifact_index == prepared.artifacts.size());

    std::set<std::string_view> materialized;
    for (const auto& artifact : prepared.artifacts) {
        materialized.insert(artifact.name);
    }
    for (const auto& image : kairosboot::test::aosp_37_0_1::kImages) {
        if (image.type == kairosboot::test::aosp_37_0_1::ImageType::Extra) {
            CHECK(!materialized.contains(image.image_name));
        }
    }
}

void hardcoded_fallback_directory_and_zip_follow_frozen_inventory() {
    TemporaryDirectory temporary;
    constexpr std::string_view android_info = "require product=atlas|boreal\n";
    const auto super_empty = valid_empty_sparse_image();

    const auto directory = temporary.path() / "fallback-directory";
    create_directory_package(directory, android_info);
    write_hardcoded_images(directory, true);
    write_bytes(directory / "super_empty.img", super_empty);

    auto zip_entries = hardcoded_zip_entries(android_info, true);
    zip_entries.push_back(ZipEntry{
        .name = "super_empty.img",
        .payload = binary_string(super_empty),
    });
    const auto archive = write_zip(temporary, zip_entries, "fallback.zip");

    ArtifactSourceResolver directory_resolver;
    ArtifactSourceResolver zip_resolver;
    auto from_directory =
        preflight_update_package(directory_resolver, directory, false);
    auto from_zip = preflight_update_package(zip_resolver, archive, false);
    CHECK(from_directory);
    CHECK(from_zip);
    check_hardcoded_plan(*from_directory, true, true);
    check_hardcoded_plan(*from_zip, true, true);
    CHECK(from_directory->requires_device_validation);
    CHECK(from_zip->requires_device_validation);
    CHECK(from_directory->plan.requirements.size() == 1U);
    CHECK(from_directory->plan.requirements.front().variable == "product");
    CHECK(from_directory->plan.requirements.front().options ==
          from_zip->plan.requirements.front().options);
    CHECK(from_directory->update_super_state ==
          UpdateSuperPreparationState::Prepared);
    CHECK(from_zip->update_super_state == UpdateSuperPreparationState::Prepared);
    CHECK(from_directory->prepared_super_artifact);
    CHECK(from_zip->prepared_super_artifact);
    CHECK(from_directory->prepared_super_artifact->resolved()->logical_name ==
          "super_empty.img");
    CHECK(from_directory->prepared_super_artifact->artifact()->metadata().kind ==
          FlashArtifactKind::AndroidSparse);
    CHECK(from_directory->prepared_super_artifact->artifact()->transfer_source() ==
          from_directory->prepared_super_artifact->resolved()->source);
    CHECK(from_directory->prepared_super_artifact->resolved()->sha256 ==
          from_zip->prepared_super_artifact->resolved()->sha256);
}

void missing_fallback_and_present_empty_manifest_remain_distinct() {
    TemporaryDirectory temporary;

    const auto missing_manifest = temporary.path() / "missing-fastboot-info";
    create_directory_package(missing_manifest, "");
    ArtifactSourceResolver missing_resolver;
    auto missing =
        preflight_update_package(missing_resolver, missing_manifest, false);
    CHECK(!missing);
    CHECK(missing.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(missing.error().artifact == "boot.img");
    CHECK(missing.error().artifact_error.has_value());
    CHECK(missing.error().artifact_error->kind == ArtifactSourceErrorKind::NotFound);

    const auto explicit_empty = temporary.path() / "present-empty-fastboot-info";
    create_directory_package(explicit_empty, "product=atlas\n", "");
    write_text(explicit_empty / "super_empty.img", "not inspected");
    ArtifactSourceResolver explicit_resolver;
    auto empty = preflight_update_package(explicit_resolver, explicit_empty, false);
    CHECK(empty);
    CHECK(empty->plan.tasks.empty());
    CHECK(empty->artifacts.empty());
    CHECK(empty->update_super_state ==
          UpdateSuperPreparationState::NotRequired);
    CHECK(!empty->prepared_super_artifact);
    CHECK(empty->requires_device_validation);
}

void hardcoded_required_optional_and_bounds_fail_closed() {
    TemporaryDirectory temporary;
    const auto minimal = temporary.path() / "fallback-minimal";
    create_directory_package(minimal, "product=atlas\n");
    write_hardcoded_images(minimal, false, false);

    ArtifactSourceResolver minimal_resolver;
    auto prepared = preflight_update_package(minimal_resolver, minimal, false);
    CHECK(prepared);
    check_hardcoded_plan(*prepared, false, false);
    CHECK(prepared->update_super_state ==
          UpdateSuperPreparationState::SkippedNotFound);
    CHECK(!prepared->prepared_super_artifact);

    const auto missing_system = temporary.path() / "fallback-missing-system";
    create_directory_package(missing_system, "");
    write_text(missing_system / "boot.img", hardcoded_payload("boot.img"));
    ArtifactSourceResolver missing_system_resolver;
    auto without_system = preflight_update_package(
        missing_system_resolver, missing_system, false);
    CHECK(!without_system);
    CHECK(without_system.error().artifact == "system.img");
    CHECK(without_system.error().artifact_error.has_value());
    CHECK(without_system.error().artifact_error->kind ==
          ArtifactSourceErrorKind::NotFound);

    const auto invalid_optional = temporary.path() / "fallback-invalid-optional";
    create_directory_package(invalid_optional, "");
    write_hardcoded_images(invalid_optional, false, false);
    const std::array malformed_sparse{
        std::byte{0x3a}, std::byte{0xff}, std::byte{0x26}, std::byte{0xed}};
    write_bytes(invalid_optional / "vendor.img", malformed_sparse);
    ArtifactSourceResolver invalid_optional_resolver;
    auto invalid = preflight_update_package(
        invalid_optional_resolver, invalid_optional, false);
    CHECK(!invalid);
    CHECK(invalid.error().artifact == "vendor.img");
    CHECK(invalid.error().artifact_error.has_value());
    CHECK(invalid.error().artifact_error->kind ==
          ArtifactSourceErrorKind::InvalidImage);

    UpdatePackagePreflightLimits task_limits;
    task_limits.manifest.maximum_tasks = 1U;
    ArtifactSourceResolver task_resolver;
    auto too_many_tasks = preflight_update_package(
        task_resolver, minimal, false, task_limits);
    CHECK(!too_many_tasks);
    CHECK(too_many_tasks.error().kind ==
          UpdatePackagePreflightErrorKind::LimitExceeded);

    UpdatePackagePreflightLimits artifact_limits;
    artifact_limits.maximum_unique_artifacts = 1U;
    ArtifactSourceResolver artifact_resolver;
    auto too_many_artifacts = preflight_update_package(
        artifact_resolver, minimal, false, artifact_limits);
    CHECK(!too_many_artifacts);
    CHECK(too_many_artifacts.error().kind ==
          UpdatePackagePreflightErrorKind::LimitExceeded);
}

void update_super_three_state_contract_and_unique_mapping() {
    TemporaryDirectory temporary;

    const auto absent = temporary.path() / "super-absent";
    create_directory_package(absent, "", "update-super\n");
    ArtifactSourceResolver absent_resolver;
    auto skipped = preflight_update_package(absent_resolver, absent, false);
    CHECK(skipped);
    CHECK(skipped->plan.tasks.empty());
    CHECK(skipped->update_super_state ==
          UpdateSuperPreparationState::SkippedNotFound);
    CHECK(!skipped->prepared_super_artifact);

    const auto super_empty = valid_empty_sparse_image();
    const auto present = temporary.path() / "super-present";
    create_directory_package(
        present, "", "flash metadata super_empty.img\nupdate-super\n");
    write_bytes(present / "super_empty.img", super_empty);
    UpdatePackagePreflightLimits exact_limits;
    exact_limits.maximum_unique_artifacts = 1U;
    exact_limits.maximum_total_artifact_bytes = super_empty.size();
    std::size_t super_resolutions = 0U;
    ArtifactSourceLimits source_limits;
    source_limits.package_entry_observer = [&](const std::string_view name) {
        if (name == "super_empty.img") {
            ++super_resolutions;
        }
    };
    ArtifactSourceResolver present_resolver(source_limits);
    auto prepared = preflight_update_package(
        present_resolver, present, false, exact_limits);
    CHECK(prepared);
    CHECK(prepared->plan.tasks.size() == 2U);
    CHECK(prepared->update_super_state ==
          UpdateSuperPreparationState::Prepared);
    CHECK(prepared->prepared_super_artifact);
    CHECK(prepared->artifacts.size() == 1U);
    CHECK(prepared->artifacts.front().name == "super_empty.img");
    CHECK(prepared->artifacts.front().resolved ==
          prepared->prepared_super_artifact->resolved());
    CHECK(prepared->artifacts.front().artifact ==
          prepared->prepared_super_artifact->artifact());
    CHECK(prepared->artifacts.front().artifact->sparse_image() ==
          prepared->prepared_super_artifact->artifact()->sparse_image());
    CHECK(super_resolutions == 1U);
    CHECK(prepared->prepared_super_artifact->artifact()->metadata().transfer_size ==
          super_empty.size());

    UpdatePackagePreflightLimits no_unique_artifact;
    no_unique_artifact.maximum_unique_artifacts = 0U;
    ArtifactSourceResolver unique_resolver;
    auto unique_failure = preflight_update_package(
        unique_resolver, present, false, no_unique_artifact);
    CHECK(!unique_failure);
    CHECK(unique_failure.error().kind ==
          UpdatePackagePreflightErrorKind::LimitExceeded);

    const auto super_only = temporary.path() / "super-present-only";
    create_directory_package(super_only, "", "update-super\n");
    write_bytes(super_only / "super_empty.img", super_empty);
    UpdatePackagePreflightLimits byte_limit;
    byte_limit.maximum_total_artifact_bytes = super_empty.size() - 1U;
    ArtifactSourceResolver byte_resolver;
    auto byte_failure = preflight_update_package(
        byte_resolver, super_only, false, byte_limit);
    CHECK(!byte_failure);
    CHECK(byte_failure.error().kind ==
          UpdatePackagePreflightErrorKind::LimitExceeded);
    CHECK(byte_failure.error().artifact == "super_empty.img");
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
    CHECK(!without_optional_manifest);
    CHECK(without_optional_manifest.error().kind ==
          UpdatePackagePreflightErrorKind::Artifact);
    CHECK(without_optional_manifest.error().artifact == "boot.img");

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
    auto prepared = preflight_update_package(resolver, package, false);
    CHECK(prepared);
    CHECK(prepared->plan.tasks.size() == 2U);
    CHECK(prepared->artifacts.size() == 1U);
    CHECK(prepared->artifacts[0].artifact->metadata().transfer_size == 14U);
    const auto immutable_hash =
        sha256_hex(prepared->artifacts[0].resolved->sha256);

    std::filesystem::remove(package / "images/shared.img");
    auto repeated = preflight_update_package(resolver, package, false);
    CHECK(!repeated);
    CHECK(repeated.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(sha256_hex(prepared->artifacts[0].resolved->sha256) == immutable_hash);
}

void zip_replacement_between_entries_fails_closed_after_one_inventory() {
    TemporaryDirectory temporary;
    const std::array original_entries{
        ZipEntry{.name = "android-info.txt", .payload = "product=atlas\n"},
        ZipEntry{
            .name = "fastboot-info.txt",
            .payload = "flash boot boot.img\n",
        },
        ZipEntry{.name = "boot.img", .payload = "trusted-image"},
    };
    const auto archive = write_zip(temporary, original_entries, "replace.zip");
    const std::array replacement_entries{
        ZipEntry{.name = "android-info.txt", .payload = "product=atlas\n"},
        ZipEntry{
            .name = "fastboot-info.txt",
            .payload = "flash boot boot.img\n",
        },
        ZipEntry{.name = "boot.img", .payload = "replacement-image-is-different"},
    };
    const auto replacement_archive =
        write_zip(temporary, replacement_entries, "replacement-source.zip");

    std::atomic<std::uint32_t> inventories{};
    std::atomic<bool> replaced{};
    ArtifactSourceLimits limits;
    limits.archive_reader_observer = [&] {
        inventories.fetch_add(1U, std::memory_order_relaxed);
    };
    limits.package_entry_observer = [&](const std::string_view name) {
        if (name == "fastboot-info.txt" &&
            !replaced.exchange(true, std::memory_order_relaxed)) {
            std::error_code filesystem_error;
            if (!std::filesystem::remove(archive, filesystem_error) ||
                filesystem_error) {
                throw CheckFailure("unable to remove ZIP replacement target");
            }
            std::filesystem::rename(
                replacement_archive, archive, filesystem_error);
            if (filesystem_error) {
                throw CheckFailure("unable to install ZIP replacement fixture");
            }
        }
    };
    ArtifactSourceResolver resolver(limits);
    auto prepared = preflight_update_package(resolver, archive, false);
    CHECK(!prepared);
    CHECK(prepared.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(prepared.error().artifact_error.has_value());
    CHECK(prepared.error().artifact_error->kind ==
          ArtifactSourceErrorKind::Integrity);
    CHECK(replaced.load(std::memory_order_relaxed));
    CHECK(inventories.load(std::memory_order_relaxed) == 1U);
}

void directory_names_are_exact_and_case_folded_deterministically() {
    TemporaryDirectory temporary;

    const auto wrong_manifest = temporary.path() / "wrong-manifest";
    std::filesystem::create_directory(wrong_manifest);
    write_text(wrong_manifest / "ANDROID-INFO.TXT", "product=atlas\n");
    ArtifactSourceResolver wrong_manifest_resolver;
    auto manifest =
        preflight_update_package(wrong_manifest_resolver, wrong_manifest, false);
    CHECK(!manifest);
    CHECK(manifest.error().kind ==
          UpdatePackagePreflightErrorKind::MissingAndroidInfo);

    const auto wrong_artifact = temporary.path() / "wrong-artifact";
    create_directory_package(
        wrong_artifact, "", "flash boot images/boot.img\n");
    write_text(wrong_artifact / "images/Boot.img", "boot");
    ArtifactSourceResolver wrong_artifact_resolver;
    auto artifact =
        preflight_update_package(wrong_artifact_resolver, wrong_artifact, false);
    CHECK(!artifact);
    CHECK(artifact.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(artifact.error().artifact_error.has_value());
    CHECK(artifact.error().artifact_error->kind ==
          ArtifactSourceErrorKind::NotFound);

    const auto aliased_plan = temporary.path() / "aliased-plan";
    create_directory_package(
        aliased_plan, "",
        "flash boot images/Boot.img\nflash vendor images/boot.img\n");
    write_text(aliased_plan / "images/Boot.img", "boot");
    ArtifactSourceResolver aliased_resolver;
    auto aliases =
        preflight_update_package(aliased_resolver, aliased_plan, false);
    CHECK(!aliases);
    CHECK(aliases.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(aliases.error().artifact_error.has_value());
    CHECK(aliases.error().artifact_error->kind ==
          ArtifactSourceErrorKind::UnsafePath);

    const auto physical_alias = temporary.path() / "physical-alias";
    create_directory_package(
        physical_alias, "", "flash boot images/Boot.img\n");
    write_text(physical_alias / "images/Boot.img", "upper");
    write_text(physical_alias / "images/boot.img", "lower");
    std::size_t image_entries = 0U;
    for (const auto& ignored :
         std::filesystem::directory_iterator(physical_alias / "images")) {
        (void)ignored;
        ++image_entries;
    }
    if (image_entries == 2U) {
        ArtifactSourceResolver physical_resolver;
        auto physical =
            preflight_update_package(physical_resolver, physical_alias, false);
        CHECK(!physical);
        CHECK(physical.error().kind == UpdatePackagePreflightErrorKind::Artifact);
        CHECK(physical.error().artifact_error.has_value());
        CHECK(physical.error().artifact_error->kind ==
              ArtifactSourceErrorKind::UnsafePath);
    }
}

void one_absolute_deadline_covers_all_snapshot_entries() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "deadline";
    create_directory_package(package, "",
                             "flash boot boot.img\n");
    write_text(package / "boot.img", "boot");

    std::atomic<std::uint32_t> observed{};
    ArtifactSourceLimits limits;
    limits.max_elapsed = std::chrono::hours(1);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    limits.package_entry_observer = [&](const std::string_view) {
        const auto count =
            observed.fetch_add(1U, std::memory_order_relaxed) + 1U;
        if (count == 2U) {
            std::this_thread::sleep_until(deadline);
        }
    };
    ArtifactSourceResolver resolver(limits);
    auto prepared = preflight_update_package(
        resolver, package, false, {}, deadline);
    CHECK(!prepared);
    CHECK(prepared.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(prepared.error().artifact_error.has_value());
    CHECK(prepared.error().artifact_error->kind ==
          ArtifactSourceErrorKind::TimedOut);
    CHECK(observed.load(std::memory_order_relaxed) == 2U);
}

void caller_deadline_preempts_artifact_limit_before_preflight_work() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "caller-deadline";
    create_directory_package(package, "", "flash boot boot.img\n");
    write_text(package / "boot.img", "boot");

    std::atomic<std::uint32_t> observed{};
    ArtifactSourceLimits source_limits;
    source_limits.max_elapsed = std::chrono::hours(1);
    source_limits.package_entry_observer = [&](const std::string_view) {
        observed.fetch_add(1U, std::memory_order_relaxed);
    };
    ArtifactSourceResolver resolver(source_limits);
    const auto deadline = std::chrono::steady_clock::now();
    auto prepared = preflight_update_package(
        resolver, package, false, {}, deadline);

    CHECK(!prepared);
    CHECK(prepared.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(prepared.error().artifact_error.has_value());
    CHECK(prepared.error().artifact_error->kind ==
          ArtifactSourceErrorKind::TimedOut);
    CHECK(observed.load(std::memory_order_relaxed) == 0U);
}

void transport_free_tasks_and_device_requirements_remain_explicit() {
    TemporaryDirectory temporary;
    const auto package = temporary.path() / "no-artifacts";
    create_directory_package(package, "product=atlas\n",
                             "reboot fastboot\nerase cache\nupdate-super\n");
    ArtifactSourceResolver resolver;
    auto prepared = preflight_update_package(resolver, package, false);
    CHECK(prepared);
    CHECK(prepared->plan.tasks.size() == 2U);
    CHECK(prepared->artifacts.empty());
    CHECK(prepared->update_super_state ==
          UpdateSuperPreparationState::SkippedNotFound);
    CHECK(!prepared->prepared_super_artifact);
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

void update_super_present_failures_abort_the_complete_preflight() {
    TemporaryDirectory temporary;
    const auto super_empty = valid_empty_sparse_image();
    const std::array corrupt_entries{
        ZipEntry{.name = "android-info.txt", .payload = ""},
        ZipEntry{.name = "fastboot-info.txt", .payload = "update-super\n"},
        ZipEntry{
            .name = "super_empty.img",
            .payload = binary_string(super_empty),
            .declared_crc = 0x12345678U,
        },
    };
    const auto corrupt =
        write_zip(temporary, corrupt_entries, "corrupt-super.zip");
    ArtifactSourceResolver crc_resolver;
    auto crc = preflight_update_package(crc_resolver, corrupt, false);
    CHECK(!crc);
    CHECK(crc.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(crc.error().artifact == "super_empty.img");
    CHECK(crc.error().artifact_error.has_value());
    CHECK(crc.error().artifact_error->kind == ArtifactSourceErrorKind::Integrity ||
          crc.error().artifact_error->kind ==
              ArtifactSourceErrorKind::InvalidArchive);

    const auto malformed = temporary.path() / "malformed-super";
    create_directory_package(malformed, "", "update-super\n");
    const std::array malformed_sparse{
        std::byte{0x3a}, std::byte{0xff}, std::byte{0x26}, std::byte{0xed}};
    write_bytes(malformed / "super_empty.img", malformed_sparse);
    ArtifactSourceResolver malformed_resolver;
    auto parse = preflight_update_package(malformed_resolver, malformed, false);
    CHECK(!parse);
    CHECK(parse.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(parse.error().artifact == "super_empty.img");
    CHECK(parse.error().artifact_error.has_value());
    CHECK(parse.error().artifact_error->kind ==
          ArtifactSourceErrorKind::InvalidImage);

#if !defined(_WIN32)
    const auto unreadable = temporary.path() / "unreadable-super";
    create_directory_package(unreadable, "", "update-super\n");
    write_bytes(unreadable / "super_empty.img", super_empty);
    std::error_code permission_error;
    std::filesystem::permissions(
        unreadable / "super_empty.img", std::filesystem::perms::none,
        std::filesystem::perm_options::replace, permission_error);
    CHECK(!permission_error);
    ArtifactSourceResolver unreadable_resolver;
    auto io = preflight_update_package(unreadable_resolver, unreadable, false);
    std::filesystem::permissions(
        unreadable / "super_empty.img",
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, permission_error);
    CHECK(!permission_error);
    CHECK(!io);
    CHECK(io.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    // The immutable directory snapshot inventories every entry before parsing
    // the manifest, so an unreadable entry fails at the package boundary.
    CHECK(io.error().artifact == "update package");
    CHECK(io.error().artifact_error.has_value());
    CHECK(io.error().artifact_error->kind == ArtifactSourceErrorKind::Io);
#endif

    const auto cancelled_package = temporary.path() / "cancelled-super";
    create_directory_package(cancelled_package, "", "update-super\n");
    write_bytes(cancelled_package / "super_empty.img", super_empty);
    std::stop_source cancellation;
    std::atomic<std::size_t> cancellation_reservations{};
    ArtifactSourceLimits cancellation_limits;
    cancellation_limits.available_space_provider =
        [&](const std::filesystem::path&)
        -> std::expected<std::uint64_t, std::error_code> {
        if (cancellation_reservations.fetch_add(
                1U, std::memory_order_acq_rel) == 2U) {
            cancellation.request_stop();
        }
        return 1ULL << 50U;
    };
    ArtifactSourceResolver cancellation_resolver(cancellation_limits);
    auto cancelled = preflight_update_package(
        cancellation_resolver, cancelled_package, false, {},
        cancellation.get_token());
    CHECK(!cancelled);
    CHECK(cancelled.error().kind == UpdatePackagePreflightErrorKind::Cancelled);
    CHECK(cancelled.error().artifact == "super_empty.img");
    CHECK(cancelled.error().artifact_error.has_value());
    CHECK(cancelled.error().artifact_error->kind ==
          ArtifactSourceErrorKind::Cancelled);

    const auto timed_package = temporary.path() / "timed-super";
    create_directory_package(timed_package, "", "update-super\n");
    write_bytes(timed_package / "super_empty.img", super_empty);
    std::atomic<std::size_t> timeout_reservations{};
    ArtifactSourceLimits timeout_limits;
    timeout_limits.max_elapsed = std::chrono::milliseconds(500);
    timeout_limits.available_space_provider =
        [&](const std::filesystem::path&)
        -> std::expected<std::uint64_t, std::error_code> {
        if (timeout_reservations.fetch_add(
                1U, std::memory_order_acq_rel) == 2U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(650));
        }
        return 1ULL << 50U;
    };
    ArtifactSourceResolver timeout_resolver(timeout_limits);
    auto timed =
        preflight_update_package(timeout_resolver, timed_package, false);
    CHECK(!timed);
    CHECK(timed.error().kind == UpdatePackagePreflightErrorKind::Artifact);
    CHECK(timed.error().artifact == "super_empty.img");
    CHECK(timed.error().artifact_error.has_value());
    CHECK(timed.error().artifact_error->kind == ArtifactSourceErrorKind::TimedOut);
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
        Test{"hardcoded fallback inventory",
             hardcoded_fallback_directory_and_zip_follow_frozen_inventory},
        Test{"missing and explicitly empty fastboot-info semantics",
             missing_fallback_and_present_empty_manifest_remain_distinct},
        Test{"hardcoded fallback required optional and bounds",
             hardcoded_required_optional_and_bounds_fail_closed},
        Test{"update-super three-state artifact contract",
             update_super_three_state_contract_and_unique_mapping},
        Test{"required manifest validation",
             required_manifest_missing_duplicate_and_bounds_fail_closed},
        Test{"wipe path validation",
             inactive_wipe_paths_are_validated_but_unselected_artifacts_are_not_opened},
        Test{"single materialization for duplicate references",
             duplicate_references_share_one_immutable_materialization},
        Test{"ZIP replacement fails closed",
             zip_replacement_between_entries_fails_closed_after_one_inventory},
        Test{"directory exact and folded names",
             directory_names_are_exact_and_case_folded_deterministically},
        Test{"shared snapshot deadline",
             one_absolute_deadline_covers_all_snapshot_entries},
        Test{"caller deadline preempts artifact limit",
             caller_deadline_preempts_artifact_limit_before_preflight_work},
        Test{"transport-free tasks and device requirements",
             transport_free_tasks_and_device_requirements_remain_explicit},
        Test{"directory and ZIP parity",
             directory_and_zip_packages_produce_equivalent_prepared_plans},
        Test{"update-super present failures",
             update_super_present_failures_abort_the_complete_preflight},
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
