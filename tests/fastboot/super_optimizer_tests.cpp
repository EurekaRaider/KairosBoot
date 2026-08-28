// SPDX-License-Identifier: MIT
#include "src/fastboot/super_optimizer.hpp"

#include "src/image/flash_artifact.hpp"
#include "src/image/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::has_super_optimization_candidate;
using kairosboot::fastboot::optimize_prepared_super;
using kairosboot::fastboot::PlannedRebootTarget;
using kairosboot::fastboot::PlannedUpdateTask;
using kairosboot::fastboot::PreparedSuperArtifact;
using kairosboot::fastboot::PreparedUpdateArtifact;
using kairosboot::fastboot::PreparedUpdatePackage;
using kairosboot::fastboot::SuperOptimizationDeviceInfo;
using kairosboot::fastboot::SuperOptimizationErrorKind;
using kairosboot::fastboot::UpdateSuperPreparationState;
using kairosboot::fastboot::UpdateTaskKind;
using kairosboot::image::ArtifactSourceOrigin;
using kairosboot::image::FlashArtifact;
using kairosboot::image::FlashArtifactKind;
using kairosboot::image::IImageSource;
using kairosboot::image::ImageSourceError;
using kairosboot::image::ResolvedArtifact;
using kairosboot::image::Sha256Accumulator;

class Failure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw Failure(std::string{"check failed: "} + #condition);        \
        }                                                                       \
    } while (false)

class MemorySource final : public IImageSource {
public:
    explicit MemorySource(std::vector<std::byte> bytes) noexcept
        : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }

    [[nodiscard]] std::expected<std::size_t, ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset > bytes_.size()) {
            return std::unexpected(ImageSourceError{"offset out of range"});
        }
        const auto amount = std::min<std::uint64_t>(
            destination.size(), bytes_.size() - offset);
        std::copy_n(bytes_.data() + offset, static_cast<std::size_t>(amount),
                    destination.data());
        return static_cast<std::size_t>(amount);
    }

private:
    std::vector<std::byte> bytes_;
};

void put_u16(std::span<std::byte> output, const std::size_t offset,
             const std::uint16_t value) {
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void put_u32(std::span<std::byte> output, const std::size_t offset,
             const std::uint32_t value) {
    put_u16(output, offset, static_cast<std::uint16_t>(value));
    put_u16(output, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

void put_u64(std::span<std::byte> output, const std::size_t offset,
             const std::uint64_t value) {
    put_u32(output, offset, static_cast<std::uint32_t>(value));
    put_u32(output, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

void put_name(std::span<std::byte> output, const std::size_t offset,
              const std::string_view name) {
    std::transform(name.begin(), name.end(), output.begin() + offset,
                   [](const char value) {
                       return static_cast<std::byte>(
                           static_cast<unsigned char>(value));
                   });
}

[[nodiscard]] kairosboot::image::Sha256Digest hash(
    const std::span<const std::byte> bytes) {
    Sha256Accumulator accumulator;
    accumulator.update(bytes);
    return accumulator.finish();
}

struct MetadataOptions final {
    std::uint32_t partition_attributes{1U};
    bool preallocated{};
    std::uint32_t group_flags{};
    std::uint32_t block_device_flags{};
    std::uint32_t block_device_count{1U};
};

[[nodiscard]] std::vector<std::byte> metadata_blob(
    const MetadataOptions options = {}) {
    constexpr std::uint32_t header_size = 128U;
    constexpr std::uint32_t partition_bytes = 2U * 52U;
    const std::uint32_t extent_count = options.preallocated ? 1U : 0U;
    const std::uint32_t extent_bytes = extent_count * 24U;
    const std::uint32_t group_offset = partition_bytes + extent_bytes;
    const std::uint32_t device_offset = group_offset + 48U;
    const std::uint32_t tables_size =
        device_offset + options.block_device_count * 64U;
    std::vector<std::byte> bytes(header_size + tables_size, std::byte{0});
    put_u32(bytes, 0U, 0x414c5030U);
    put_u16(bytes, 4U, 10U);
    put_u16(bytes, 6U, 0U);
    put_u32(bytes, 8U, header_size);
    put_u32(bytes, 44U, tables_size);
    put_u32(bytes, 80U, 0U);
    put_u32(bytes, 84U, 2U);
    put_u32(bytes, 88U, 52U);
    put_u32(bytes, 92U, partition_bytes);
    put_u32(bytes, 96U, extent_count);
    put_u32(bytes, 100U, 24U);
    put_u32(bytes, 104U, group_offset);
    put_u32(bytes, 108U, 1U);
    put_u32(bytes, 112U, 48U);
    put_u32(bytes, 116U, device_offset);
    put_u32(bytes, 120U, options.block_device_count);
    put_u32(bytes, 124U, 64U);

    auto cursor = static_cast<std::size_t>(header_size);
    std::size_t partition_index = 0U;
    for (const auto name : {std::string_view{"system_a"},
                            std::string_view{"system_b"}}) {
        put_name(bytes, cursor, name);
        put_u32(bytes, cursor + 36U, options.partition_attributes);
        put_u32(bytes, cursor + 40U, 0U);
        put_u32(bytes, cursor + 44U,
                options.preallocated && partition_index == 0U ? 1U : 0U);
        put_u32(bytes, cursor + 48U, 0U);
        cursor += 52U;
        ++partition_index;
    }
    if (options.preallocated) {
        put_u64(bytes, cursor, 8U);
        put_u32(bytes, cursor + 8U, 0U);
        put_u64(bytes, cursor + 12U, 64U);
        put_u32(bytes, cursor + 20U, 0U);
        cursor += 24U;
    }
    put_name(bytes, cursor, "dynamic");
    put_u32(bytes, cursor + 36U, options.group_flags);
    put_u64(bytes, cursor + 40U, 512U * 1024U);
    cursor += 48U;
    for (std::uint32_t index = 0U; index < options.block_device_count; ++index) {
        put_u64(bytes, cursor, 64U);
        put_u32(bytes, cursor + 8U, 4096U);
        put_u32(bytes, cursor + 12U, 0U);
        put_u64(bytes, cursor + 16U, 1024U * 1024U);
        put_name(bytes, cursor + 24U,
                 index == 0U ? "super" : "super_extra");
        put_u32(bytes, cursor + 60U, options.block_device_flags);
        cursor += 64U;
    }

    const auto tables = std::span(bytes).subspan(header_size);
    const auto tables_hash = hash(tables);
    std::copy(tables_hash.begin(), tables_hash.end(), bytes.begin() + 48);
    auto header = std::vector<std::byte>(bytes.begin(),
                                         bytes.begin() + header_size);
    const auto header_hash = hash(header);
    std::copy(header_hash.begin(), header_hash.end(), bytes.begin() + 12);
    return bytes;
}

[[nodiscard]] std::vector<std::byte> valid_super_empty(
    const MetadataOptions options = {}) {
    constexpr std::size_t super_size = 1024U * 1024U;
    constexpr std::size_t metadata_size = 4096U;
    constexpr std::size_t primary_start = 12U * 1024U;
    constexpr std::size_t backup_start = primary_start + metadata_size * 2U;
    std::vector<std::byte> output(super_size, std::byte{0});
    std::vector<std::byte> geometry(4096U, std::byte{0});
    put_u32(geometry, 0U, 0x616c4467U);
    put_u32(geometry, 4U, 52U);
    put_u32(geometry, 40U, metadata_size);
    put_u32(geometry, 44U, 2U);
    put_u32(geometry, 48U, 4096U);
    auto geometry_struct = std::vector<std::byte>(geometry.begin(),
                                                  geometry.begin() + 52U);
    const auto geometry_hash = hash(geometry_struct);
    std::copy(geometry_hash.begin(), geometry_hash.end(), geometry.begin() + 8);
    std::copy(geometry.begin(), geometry.end(), output.begin() + 4096U);
    std::copy(geometry.begin(), geometry.end(), output.begin() + 8192U);

    const auto metadata = metadata_blob(options);
    for (const auto offset : {primary_start, primary_start + metadata_size,
                              backup_start, backup_start + metadata_size}) {
        std::copy(metadata.begin(), metadata.end(), output.begin() + offset);
    }
    return output;
}

[[nodiscard]] std::shared_ptr<const ResolvedArtifact> resolved(
    std::string name, std::shared_ptr<const IImageSource> source) {
    auto digest = kairosboot::image::compute_sha256(*source);
    CHECK(digest);
    return std::make_shared<const ResolvedArtifact>(ResolvedArtifact{
        .source = std::move(source),
        .sha256 = *digest,
        .origin = ArtifactSourceOrigin::DirectFile,
        .logical_name = std::move(name),
    });
}

[[nodiscard]] std::shared_ptr<const FlashArtifact> inspected(
    const std::shared_ptr<const IImageSource>& source) {
    auto artifact = FlashArtifact::inspect(source);
    CHECK(artifact);
    return std::make_shared<const FlashArtifact>(std::move(*artifact));
}

[[nodiscard]] PreparedUpdatePackage package(
    std::vector<std::byte> super_bytes = valid_super_empty(),
    std::vector<std::byte> system_bytes =
        std::vector<std::byte>(6000U, std::byte{0x5a})) {
    auto super_source = std::make_shared<const MemorySource>(
        std::move(super_bytes));
    auto super_resolved = resolved("super_empty.img", super_source);
    auto super_artifact = inspected(super_source);
    auto system_source = std::make_shared<const MemorySource>(
        std::move(system_bytes));
    auto system_resolved = resolved("system.img", system_source);
    auto system_artifact = inspected(system_source);
    PreparedUpdatePackage result;
    result.plan.tasks = {
        PlannedUpdateTask{.kind = UpdateTaskKind::Reboot,
                          .reboot_target = PlannedRebootTarget::Fastboot},
        PlannedUpdateTask{.kind = UpdateTaskKind::UpdateSuper},
        PlannedUpdateTask{.kind = UpdateTaskKind::Flash,
                          .partition = "system",
                          .artifact = "system.img"},
    };
    result.artifacts.push_back(PreparedUpdateArtifact{
        .name = "system.img",
        .resolved = std::move(system_resolved),
        .artifact = std::move(system_artifact),
    });
    result.update_super_state = UpdateSuperPreparationState::Prepared;
    result.prepared_super_artifact = std::make_shared<const PreparedSuperArtifact>(
        std::move(super_resolved), std::move(super_artifact));
    return result;
}

void valid_a_b_layout_becomes_one_sparse_super_flash() {
    auto prepared = package();
    CHECK(has_super_optimization_candidate(prepared));
    auto result = optimize_prepared_super(
        prepared,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "a",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(result);
    CHECK(result->optimized);
    CHECK(result->absorbed_partitions == std::vector<std::string>{"system_a"});
    CHECK(prepared.plan.tasks.size() == 1U);
    CHECK(prepared.plan.tasks.front().kind == UpdateTaskKind::Flash);
    CHECK(prepared.plan.tasks.front().partition == "super");
    CHECK(prepared.artifacts.size() == 1U);
    CHECK(prepared.artifacts.front().artifact->metadata().kind ==
          FlashArtifactKind::AndroidSparse);
    CHECK(prepared.artifacts.front().artifact->metadata().expanded_size ==
          1024U * 1024U);
    CHECK(prepared.update_super_state == UpdateSuperPreparationState::NotRequired);
    CHECK(!prepared.prepared_super_artifact);

    const auto* sparse = prepared.artifacts.front().artifact->sparse_image();
    CHECK(sparse != nullptr);
    std::array<std::byte, 6000U> payload{};
    auto read = sparse->read_at(64U * 512U, payload);
    CHECK(read && *read == payload.size());
    CHECK(std::ranges::all_of(payload,
                              [](const std::byte value) {
                                  return value == std::byte{0x5a};
                              }));
}

void static_slotted_flash_is_retained_and_bad_slot_fails_closed() {
    auto prepared = package();
    prepared.plan.tasks.insert(
        prepared.plan.tasks.begin(),
        PlannedUpdateTask{.kind = UpdateTaskKind::Flash,
                          .partition = "boot_a",
                          .artifact = "boot.img"});
    auto boot_source = std::make_shared<const MemorySource>(
        std::vector<std::byte>(4096U, std::byte{0x33}));
    prepared.artifacts.push_back(PreparedUpdateArtifact{
        .name = "boot.img",
        .resolved = resolved("boot.img", boot_source),
        .artifact = inspected(boot_source),
    });
    auto optimized = optimize_prepared_super(
        prepared,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "a",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(optimized && optimized->optimized);
    CHECK(prepared.plan.tasks.size() == 2U);
    CHECK(prepared.plan.tasks[0].partition == "boot_a");
    CHECK(prepared.plan.tasks[1].partition == "super");

    auto missing_slot = package();
    auto failed = optimize_prepared_super(
        missing_slot,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "c",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(!failed);
    CHECK(failed.error().kind == SuperOptimizationErrorKind::SlotMismatch);
    CHECK(missing_slot.plan.tasks.size() == 3U);
}

void size_and_geometry_mismatch_fail_before_plan_mutation() {
    auto wrong_size = package();
    auto size = optimize_prepared_super(
        wrong_size,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "a",
                                    .super_partition_size = 2U * 1024U * 1024U});
    CHECK(!size);
    CHECK(size.error().kind == SuperOptimizationErrorKind::SizeMismatch);
    CHECK(wrong_size.plan.tasks.size() == 3U);

    auto wrong_name = package();
    auto name = optimize_prepared_super(
        wrong_name,
        SuperOptimizationDeviceInfo{.super_partition = "super_other",
                                    .current_slot = "a",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(!name);
    CHECK(name.error().kind == SuperOptimizationErrorKind::SizeMismatch);
    CHECK(wrong_name.plan.tasks.size() == 3U);

    auto corrupted = valid_super_empty();
    corrupted[4096U + 8U] ^= std::byte{0x01};
    auto bad = package(std::move(corrupted));
    auto geometry = optimize_prepared_super(
        bad,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "a",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(!geometry);
    CHECK(geometry.error().kind == SuperOptimizationErrorKind::InvalidMetadata);
    CHECK(bad.plan.tasks.size() == 3U);

    auto extent = package(valid_super_empty(MetadataOptions{.preallocated = true}));
    auto extent_result = optimize_prepared_super(
        extent,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "a",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(extent_result && !extent_result->optimized);
    CHECK(extent.plan.tasks.size() == 3U);

    auto inconsistent_slot = valid_super_empty();
    inconsistent_slot[12U * 1024U + 4096U + 100U] ^= std::byte{0x01};
    auto slot = package(std::move(inconsistent_slot));
    auto slot_result = optimize_prepared_super(
        slot,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "a",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(!slot_result);
    CHECK(slot_result.error().kind == SuperOptimizationErrorKind::SlotMismatch);
    CHECK(slot.plan.tasks.size() == 3U);
}

void legal_unsupported_layouts_keep_the_frozen_plan() {
    const auto check_fallback = [](const MetadataOptions options) {
        auto prepared = package(valid_super_empty(options));
        auto result = optimize_prepared_super(
            prepared,
            SuperOptimizationDeviceInfo{.super_partition = "super",
                                        .current_slot = "a",
                                        .super_partition_size =
                                            1024U * 1024U});
        CHECK(result && !result->optimized);
        CHECK(prepared.plan.tasks.size() == 3U);
        CHECK(prepared.artifacts.size() == 1U);
        CHECK(prepared.update_super_state ==
              UpdateSuperPreparationState::Prepared);
        CHECK(prepared.prepared_super_artifact);
    };

    check_fallback(MetadataOptions{.partition_attributes = 0U});
    check_fallback(MetadataOptions{.partition_attributes = 1U | (1U << 1U)});
    check_fallback(MetadataOptions{.partition_attributes = 1U | (1U << 2U)});
    check_fallback(MetadataOptions{.partition_attributes = 1U | (1U << 3U)});
    check_fallback(MetadataOptions{.partition_attributes = 1U | (1U << 31U)});
    check_fallback(MetadataOptions{.preallocated = true});
    check_fallback(MetadataOptions{.group_flags = 1U});
    check_fallback(MetadataOptions{.block_device_flags = 1U});
    check_fallback(MetadataOptions{.block_device_count = 2U});
}

void only_the_exact_window_is_rewritten_and_artifact_ids_are_unique() {
    auto prepared = package();
    prepared.plan.tasks.insert(
        prepared.plan.tasks.begin(),
        PlannedUpdateTask{.kind = UpdateTaskKind::Reboot,
                          .reboot_target = PlannedRebootTarget::Fastboot});
    prepared.plan.tasks.push_back(
        PlannedUpdateTask{.kind = UpdateTaskKind::UpdateSuper});
    prepared.plan.tasks.push_back(
        PlannedUpdateTask{.kind = UpdateTaskKind::Reboot,
                          .reboot_target = PlannedRebootTarget::Fastboot});
    prepared.plan.tasks.push_back(
        PlannedUpdateTask{.kind = UpdateTaskKind::Flash,
                          .partition = "boot_a",
                          .artifact = "kairosboot-optimized-super.img"});
    auto collision_source = std::make_shared<const MemorySource>(
        std::vector<std::byte>(4096U, std::byte{0x33}));
    prepared.artifacts.push_back(PreparedUpdateArtifact{
        .name = "kairosboot-optimized-super.img",
        .resolved = resolved("kairosboot-optimized-super.img", collision_source),
        .artifact = inspected(collision_source),
    });

    auto result = optimize_prepared_super(
        prepared,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "a",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(result && result->optimized);
    CHECK(prepared.plan.tasks.size() == 5U);
    CHECK(prepared.plan.tasks[0].kind == UpdateTaskKind::Reboot);
    CHECK(prepared.plan.tasks[0].reboot_target ==
          PlannedRebootTarget::Fastboot);
    CHECK(prepared.plan.tasks[1].kind == UpdateTaskKind::Flash);
    CHECK(prepared.plan.tasks[1].partition == "super");
    CHECK(prepared.plan.tasks[1].artifact ==
          "kairosboot-optimized-super-1.img");
    CHECK(prepared.plan.tasks[2].kind == UpdateTaskKind::UpdateSuper);
    CHECK(prepared.plan.tasks[3].kind == UpdateTaskKind::Reboot);
    CHECK(prepared.plan.tasks[3].reboot_target ==
          PlannedRebootTarget::Fastboot);
    CHECK(prepared.plan.tasks[4].partition == "boot_a");
    CHECK(prepared.plan.tasks[4].artifact ==
          "kairosboot-optimized-super.img");
    CHECK(prepared.update_super_state == UpdateSuperPreparationState::Prepared);
    CHECK(prepared.prepared_super_artifact);
    CHECK(prepared.artifacts.size() == 2U);
    CHECK(std::ranges::any_of(
        prepared.artifacts, [](const PreparedUpdateArtifact& artifact) {
            return artifact.name == "kairosboot-optimized-super.img";
        }));
    CHECK(std::ranges::any_of(
        prepared.artifacts, [](const PreparedUpdateArtifact& artifact) {
            return artifact.name == "kairosboot-optimized-super-1.img";
        }));
}

void sparse_logical_image_keeps_frozen_unoptimized_path() {
    std::vector<std::byte> sparse;
    const auto append16 = [&sparse](const std::uint16_t value) {
        const auto offset = sparse.size();
        sparse.resize(offset + 2U);
        put_u16(sparse, offset, value);
    };
    const auto append32 = [&sparse](const std::uint32_t value) {
        const auto offset = sparse.size();
        sparse.resize(offset + 4U);
        put_u32(sparse, offset, value);
    };
    append32(kairosboot::image::kAndroidSparseMagic);
    append16(1U);
    append16(0U);
    append16(28U);
    append16(12U);
    append32(4096U);
    append32(1U);
    append32(1U);
    append32(0U);
    append16(kairosboot::image::kSparseChunkDontCare);
    append16(0U);
    append32(1U);
    append32(12U);

    auto prepared = package(valid_super_empty(), std::move(sparse));
    auto result = optimize_prepared_super(
        prepared,
        SuperOptimizationDeviceInfo{.super_partition = "super",
                                    .current_slot = "a",
                                    .super_partition_size = 1024U * 1024U});
    CHECK(result);
    CHECK(!result->optimized);
    CHECK(prepared.plan.tasks.size() == 3U);
    CHECK(prepared.update_super_state == UpdateSuperPreparationState::Prepared);
}

void unavailable_optional_getvars_fall_back_only_after_metadata_validation() {
    auto valid = package();
    auto fallback = optimize_prepared_super(
        valid, SuperOptimizationDeviceInfo{.super_partition = "super"});
    CHECK(fallback && !fallback->optimized);
    CHECK(valid.plan.tasks.size() == 3U);

    auto corrupted = valid_super_empty();
    corrupted[4096U + 8U] ^= std::byte{0x01};
    auto invalid = package(std::move(corrupted));
    auto rejected = optimize_prepared_super(
        invalid, SuperOptimizationDeviceInfo{.super_partition = "super"});
    CHECK(!rejected);
    CHECK(rejected.error().kind == SuperOptimizationErrorKind::InvalidMetadata);
    CHECK(invalid.plan.tasks.size() == 3U);
}

}  // namespace

int main() {
    try {
        valid_a_b_layout_becomes_one_sparse_super_flash();
        size_and_geometry_mismatch_fail_before_plan_mutation();
        static_slotted_flash_is_retained_and_bad_slot_fails_closed();
        legal_unsupported_layouts_keep_the_frozen_plan();
        only_the_exact_window_is_rewritten_and_artifact_ids_are_unique();
        sparse_logical_image_keeps_frozen_unoptimized_path();
        unavailable_optional_getvars_fall_back_only_after_metadata_validation();
        std::cout << "super optimizer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
