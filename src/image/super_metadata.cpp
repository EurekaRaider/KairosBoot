// SPDX-License-Identifier: MIT
#include "super_metadata.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace kairosboot::image {
namespace {

constexpr std::uint32_t kGeometryMagic = 0x616c4467U;
constexpr std::uint32_t kHeaderMagic = 0x414c5030U;
constexpr std::uint16_t kMetadataMajorVersion = 10U;
constexpr std::uint16_t kMaximumMetadataMinorVersion = 2U;
constexpr std::uint64_t kSectorSize = 512U;
constexpr std::uint64_t kReservedBytes = 4096U;
constexpr std::uint64_t kGeometrySize = 4096U;
constexpr std::uint64_t kPrimaryGeometryOffset = kReservedBytes;
constexpr std::uint64_t kBackupGeometryOffset = kReservedBytes + kGeometrySize;
constexpr std::uint64_t kPrimaryMetadataOffset =
    kReservedBytes + 2U * kGeometrySize;
constexpr std::size_t kGeometryStructSize = 52U;
constexpr std::size_t kHeaderV10Size = 128U;
constexpr std::size_t kHeaderV12Size = 256U;
constexpr std::size_t kPartitionEntrySize = 52U;
constexpr std::size_t kExtentEntrySize = 24U;
constexpr std::size_t kGroupEntrySize = 48U;
constexpr std::size_t kBlockDeviceEntrySize = 64U;
constexpr std::uint32_t kLinearExtent = 0U;
constexpr std::uint32_t kZeroExtent = 1U;
constexpr std::size_t kMaximumMetadataBytes = 1024U * 1024U;
constexpr std::uint32_t kSparseBlockSize = 4096U;
constexpr std::uint32_t kSparseRawChunk = 0xcac1U;
constexpr std::uint32_t kSparseDontCareChunk = 0xcac3U;

struct Geometry final {
    std::uint32_t metadata_max_size{};
    std::uint32_t metadata_slot_count{};
    std::uint32_t logical_block_size{};
};

struct Descriptor final {
    std::uint32_t offset{};
    std::uint32_t count{};
    std::uint32_t entry_size{};
};

struct Partition final {
    std::vector<std::byte> raw;
    std::string name;
    std::uint32_t attributes{};
    std::uint32_t group_index{};
    std::vector<std::size_t> extent_indices;
};

struct Extent final {
    std::vector<std::byte> raw;
    std::uint64_t sectors{};
    std::uint32_t type{};
    std::uint64_t target_data{};
    std::uint32_t target_source{};
};

struct Group final {
    std::vector<std::byte> raw;
    std::uint64_t maximum_size{};
};

struct BlockDevice final {
    std::vector<std::byte> raw;
    std::uint64_t first_sector{};
    std::uint32_t alignment{};
    std::uint32_t alignment_offset{};
    std::uint64_t size{};
};

struct Metadata final {
    Geometry geometry;
    std::uint32_t slot{};
    std::vector<std::byte> header;
    std::uint16_t minor_version{};
    Descriptor partition_descriptor;
    Descriptor extent_descriptor;
    Descriptor group_descriptor;
    Descriptor block_device_descriptor;
    std::vector<Partition> partitions;
    std::vector<Extent> extents;
    std::vector<Group> groups;
    std::vector<BlockDevice> block_devices;
};

[[nodiscard]] SuperMetadataError error(const SuperMetadataErrorKind kind,
                                       std::string message) {
    return SuperMetadataError{kind, std::move(message)};
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> read_le(const std::span<const std::byte> bytes,
                                             const std::size_t offset) noexcept {
    static_assert(std::is_unsigned_v<Integer>);
    if (offset > bytes.size() || sizeof(Integer) > bytes.size() - offset) {
        return std::nullopt;
    }
    Integer value{};
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(std::to_integer<unsigned int>(
                     bytes[offset + index]))
                 << static_cast<unsigned int>(index * 8U);
    }
    return value;
}

template <typename Integer>
bool write_le(const std::span<std::byte> bytes, const std::size_t offset,
              const Integer value) noexcept {
    static_assert(std::is_unsigned_v<Integer>);
    if (offset > bytes.size() || sizeof(Integer) > bytes.size() - offset) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> static_cast<unsigned int>(index * 8U)) &
            static_cast<Integer>(0xffU));
    }
    return true;
}

[[nodiscard]] Sha256Digest digest(const std::span<const std::byte> bytes) noexcept {
    Sha256Accumulator accumulator;
    accumulator.update(bytes);
    return accumulator.finish();
}

[[nodiscard]] bool checksum_matches(const std::span<const std::byte> bytes,
                                    const std::size_t checksum_offset) {
    if (checksum_offset > bytes.size() ||
        kSha256DigestSize > bytes.size() - checksum_offset) {
        return false;
    }
    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    std::array<std::byte, kSha256DigestSize> expected{};
    std::copy_n(copy.begin() + static_cast<std::ptrdiff_t>(checksum_offset),
                expected.size(), expected.begin());
    std::fill_n(copy.begin() + static_cast<std::ptrdiff_t>(checksum_offset),
                expected.size(), std::byte{});
    return digest(copy) == expected;
}

[[nodiscard]] std::optional<Geometry> parse_geometry_at(
    const std::span<const std::byte> bytes, const std::uint64_t offset) {
    if (offset > bytes.size() || kGeometryStructSize > bytes.size() - offset) {
        return std::nullopt;
    }
    const auto geometry_bytes = bytes.subspan(
        static_cast<std::size_t>(offset), kGeometryStructSize);
    const auto magic = read_le<std::uint32_t>(geometry_bytes, 0U);
    const auto structure_size = read_le<std::uint32_t>(geometry_bytes, 4U);
    const auto maximum = read_le<std::uint32_t>(geometry_bytes, 40U);
    const auto slots = read_le<std::uint32_t>(geometry_bytes, 44U);
    const auto logical_block = read_le<std::uint32_t>(geometry_bytes, 48U);
    if (!magic || !structure_size || !maximum || !slots || !logical_block ||
        *magic != kGeometryMagic || *structure_size != kGeometryStructSize ||
        !checksum_matches(geometry_bytes, 8U) || *maximum == 0U ||
        *maximum > kMaximumMetadataBytes || *maximum % kSectorSize != 0U ||
        *slots == 0U || *slots > 32U || *logical_block < kSectorSize ||
        *logical_block % kSectorSize != 0U) {
        return std::nullopt;
    }
    return Geometry{*maximum, *slots, *logical_block};
}

[[nodiscard]] std::expected<Geometry, SuperMetadataError> parse_geometry(
    const std::span<const std::byte> bytes) {
    if (auto primary = parse_geometry_at(bytes, kPrimaryGeometryOffset)) {
        return *primary;
    }
    if (auto backup = parse_geometry_at(bytes, kBackupGeometryOffset)) {
        return *backup;
    }
    return std::unexpected(error(
        SuperMetadataErrorKind::Malformed,
        "fetched super image has no valid primary or backup geometry"));
}

[[nodiscard]] std::optional<std::uint32_t> metadata_slot_for(
    const std::string_view partition, const Geometry& geometry) noexcept {
    if (partition.size() >= 2U && partition[partition.size() - 2U] == '_' &&
        partition.back() >= 'a' && partition.back() <= 'z') {
        const auto slot = static_cast<std::uint32_t>(partition.back() - 'a');
        if (slot < geometry.metadata_slot_count) {
            return slot;
        }
        return std::nullopt;
    }
    return 0U;
}

[[nodiscard]] bool checked_table_bytes(const Descriptor& descriptor,
                                       const std::size_t tables_size,
                                       std::size_t& result) noexcept {
    if (descriptor.entry_size == 0U ||
        descriptor.count > std::numeric_limits<std::size_t>::max() /
                               descriptor.entry_size) {
        return false;
    }
    result = static_cast<std::size_t>(descriptor.count) * descriptor.entry_size;
    return descriptor.offset <= tables_size && result <= tables_size - descriptor.offset;
}

[[nodiscard]] std::optional<Descriptor> read_descriptor(
    const std::span<const std::byte> header, const std::size_t offset) noexcept {
    const auto table_offset = read_le<std::uint32_t>(header, offset);
    const auto count = read_le<std::uint32_t>(header, offset + 4U);
    const auto entry_size = read_le<std::uint32_t>(header, offset + 8U);
    if (!table_offset || !count || !entry_size) {
        return std::nullopt;
    }
    return Descriptor{*table_offset, *count, *entry_size};
}

[[nodiscard]] std::optional<std::string> parse_name(
    const std::span<const std::byte> bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), std::byte{});
    if (end == bytes.begin()) {
        return std::nullopt;
    }
    std::string value;
    value.reserve(static_cast<std::size_t>(end - bytes.begin()));
    for (auto cursor = bytes.begin(); cursor != end; ++cursor) {
        const auto character = static_cast<unsigned char>(
            std::to_integer<unsigned int>(*cursor));
        const bool ascii_alphanumeric =
            (character >= '0' && character <= '9') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z');
        if (!(ascii_alphanumeric || character == '_')) {
            return std::nullopt;
        }
        value.push_back(static_cast<char>(character));
    }
    return value;
}

[[nodiscard]] std::expected<Metadata, SuperMetadataError> parse_metadata_at(
    const std::span<const std::byte> fetched, const Geometry& geometry,
    const std::uint32_t slot, const std::uint64_t offset) {
    if (offset > fetched.size() ||
        geometry.metadata_max_size > fetched.size() - offset) {
        return std::unexpected(error(
            SuperMetadataErrorKind::Malformed,
            "fetched super image does not contain the selected metadata slot"));
    }
    const auto area = fetched.subspan(static_cast<std::size_t>(offset),
                                      geometry.metadata_max_size);
    const auto magic = read_le<std::uint32_t>(area, 0U);
    const auto major = read_le<std::uint16_t>(area, 4U);
    const auto minor = read_le<std::uint16_t>(area, 6U);
    const auto header_size = read_le<std::uint32_t>(area, 8U);
    const auto tables_size = read_le<std::uint32_t>(area, 44U);
    if (!magic || !major || !minor || !header_size || !tables_size ||
        *magic != kHeaderMagic || *major != kMetadataMajorVersion ||
        *minor > kMaximumMetadataMinorVersion ||
        (*header_size != kHeaderV10Size && *header_size != kHeaderV12Size) ||
        *header_size > area.size() || *tables_size > area.size() - *header_size) {
        return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                     "logical partition metadata header is invalid"));
    }
    const auto header = area.first(*header_size);
    const auto tables = area.subspan(*header_size, *tables_size);
    const auto tables_digest = digest(tables);
    if (!checksum_matches(header, 12U) ||
        !std::equal(tables_digest.begin(), tables_digest.end(),
                    area.begin() + 48)) {
        return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                     "logical partition metadata checksum is invalid"));
    }

    auto partition_descriptor = read_descriptor(header, 80U);
    auto extent_descriptor = read_descriptor(header, 92U);
    auto group_descriptor = read_descriptor(header, 104U);
    auto device_descriptor = read_descriptor(header, 116U);
    if (!partition_descriptor || !extent_descriptor || !group_descriptor ||
        !device_descriptor || partition_descriptor->entry_size < kPartitionEntrySize ||
        extent_descriptor->entry_size < kExtentEntrySize ||
        group_descriptor->entry_size < kGroupEntrySize ||
        device_descriptor->entry_size < kBlockDeviceEntrySize) {
        return std::unexpected(error(SuperMetadataErrorKind::Unsupported,
                                     "logical partition table layout is unsupported"));
    }
    std::array<std::pair<std::size_t, std::size_t>, 4> ranges{};
    const std::array descriptors{*partition_descriptor, *extent_descriptor,
                                 *group_descriptor, *device_descriptor};
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        std::size_t bytes{};
        if (!checked_table_bytes(descriptors[index], tables.size(), bytes)) {
            return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                         "logical partition table exceeds metadata bounds"));
        }
        ranges[index] = {descriptors[index].offset,
                         descriptors[index].offset + bytes};
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index) {
        if (ranges[index - 1U].second > ranges[index].first) {
            return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                         "logical partition tables overlap"));
        }
    }

    Metadata result{
        .geometry = geometry,
        .slot = slot,
        .header = std::vector<std::byte>(header.begin(), header.end()),
        .minor_version = *minor,
        .partition_descriptor = *partition_descriptor,
        .extent_descriptor = *extent_descriptor,
        .group_descriptor = *group_descriptor,
        .block_device_descriptor = *device_descriptor,
    };
    result.extents.reserve(extent_descriptor->count);
    for (std::uint32_t index = 0; index < extent_descriptor->count; ++index) {
        const auto entry_offset = static_cast<std::size_t>(extent_descriptor->offset) +
                                  static_cast<std::size_t>(index) *
                                      extent_descriptor->entry_size;
        const auto entry = tables.subspan(entry_offset, extent_descriptor->entry_size);
        const auto sectors = read_le<std::uint64_t>(entry, 0U);
        const auto type = read_le<std::uint32_t>(entry, 8U);
        const auto target_data = read_le<std::uint64_t>(entry, 12U);
        const auto target_source = read_le<std::uint32_t>(entry, 20U);
        if (!sectors || !type || !target_data || !target_source ||
            (*type != kLinearExtent && *type != kZeroExtent)) {
            return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                         "logical partition extent is invalid"));
        }
        result.extents.push_back(Extent{
            std::vector<std::byte>(entry.begin(), entry.end()), *sectors,
            *type, *target_data, *target_source});
    }

    result.groups.reserve(group_descriptor->count);
    for (std::uint32_t index = 0; index < group_descriptor->count; ++index) {
        const auto entry_offset = static_cast<std::size_t>(group_descriptor->offset) +
                                  static_cast<std::size_t>(index) *
                                      group_descriptor->entry_size;
        const auto entry = tables.subspan(entry_offset, group_descriptor->entry_size);
        const auto maximum = read_le<std::uint64_t>(entry, 40U);
        if (!maximum) {
            return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                         "logical partition group is invalid"));
        }
        result.groups.push_back(Group{
            std::vector<std::byte>(entry.begin(), entry.end()), *maximum});
    }

    result.block_devices.reserve(device_descriptor->count);
    for (std::uint32_t index = 0; index < device_descriptor->count; ++index) {
        const auto entry_offset = static_cast<std::size_t>(device_descriptor->offset) +
                                  static_cast<std::size_t>(index) *
                                      device_descriptor->entry_size;
        const auto entry = tables.subspan(entry_offset, device_descriptor->entry_size);
        const auto first = read_le<std::uint64_t>(entry, 0U);
        const auto alignment = read_le<std::uint32_t>(entry, 8U);
        const auto alignment_offset = read_le<std::uint32_t>(entry, 12U);
        const auto size = read_le<std::uint64_t>(entry, 16U);
        if (!first || !alignment || !alignment_offset || !size ||
            *size == 0U || *size % kSectorSize != 0U ||
            *first >= *size / kSectorSize ||
            (*alignment != 0U && *alignment % kSectorSize != 0U)) {
            return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                         "logical partition block device is invalid"));
        }
        result.block_devices.push_back(BlockDevice{
            std::vector<std::byte>(entry.begin(), entry.end()), *first,
            *alignment, *alignment_offset, *size});
    }
    if (result.block_devices.empty() || result.groups.empty()) {
        return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                     "logical partition metadata has no group or block device"));
    }
    for (const auto& extent : result.extents) {
        if (extent.type == kLinearExtent &&
            (extent.target_source >= result.block_devices.size() ||
             extent.target_data <
                 result.block_devices[extent.target_source].first_sector ||
             extent.sectors >
                 result.block_devices[extent.target_source].size / kSectorSize -
                     extent.target_data)) {
            return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                         "logical partition extent exceeds its block device"));
        }
    }

    std::vector<bool> owned(result.extents.size(), false);
    std::set<std::string> names;
    result.partitions.reserve(partition_descriptor->count);
    for (std::uint32_t index = 0; index < partition_descriptor->count; ++index) {
        const auto entry_offset = static_cast<std::size_t>(partition_descriptor->offset) +
                                  static_cast<std::size_t>(index) *
                                      partition_descriptor->entry_size;
        const auto entry = tables.subspan(entry_offset, partition_descriptor->entry_size);
        auto name = parse_name(entry.first(36U));
        const auto attributes = read_le<std::uint32_t>(entry, 36U);
        const auto first = read_le<std::uint32_t>(entry, 40U);
        const auto count = read_le<std::uint32_t>(entry, 44U);
        const auto group = read_le<std::uint32_t>(entry, 48U);
        if (!name || !attributes || !first || !count || !group ||
            !names.insert(*name).second || *group >= result.groups.size() ||
            *first > result.extents.size() ||
            *count > result.extents.size() - *first) {
            return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                         "logical partition entry is invalid"));
        }
        Partition parsed{
            .raw = std::vector<std::byte>(entry.begin(), entry.end()),
            .name = std::move(*name),
            .attributes = *attributes,
            .group_index = *group,
        };
        parsed.extent_indices.reserve(*count);
        for (std::uint32_t extent = 0; extent < *count; ++extent) {
            const auto extent_index = static_cast<std::size_t>(*first + extent);
            if (owned[extent_index]) {
                return std::unexpected(error(
                    SuperMetadataErrorKind::Malformed,
                    "logical partition extents have multiple owners"));
            }
            owned[extent_index] = true;
            parsed.extent_indices.push_back(extent_index);
        }
        result.partitions.push_back(std::move(parsed));
    }
    if (std::find(owned.begin(), owned.end(), false) != owned.end()) {
        return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                     "logical partition metadata has orphan extents"));
    }
    return result;
}

[[nodiscard]] std::expected<Metadata, SuperMetadataError> parse_metadata(
    const std::span<const std::byte> fetched, const Geometry& geometry,
    const std::uint32_t slot) {
    const auto primary = kPrimaryMetadataOffset +
                         static_cast<std::uint64_t>(slot) *
                             geometry.metadata_max_size;
    auto parsed = parse_metadata_at(fetched, geometry, slot, primary);
    if (parsed) {
        return parsed;
    }
    const auto backup = kPrimaryMetadataOffset +
                        static_cast<std::uint64_t>(geometry.metadata_slot_count) *
                            geometry.metadata_max_size +
                        static_cast<std::uint64_t>(slot) *
                            geometry.metadata_max_size;
    auto backup_parsed = parse_metadata_at(fetched, geometry, slot, backup);
    if (backup_parsed) {
        return backup_parsed;
    }
    return std::unexpected(std::move(parsed.error()));
}

[[nodiscard]] std::expected<std::uint64_t, SuperMetadataError> partition_bytes(
    const Metadata& metadata, const Partition& partition) {
    std::uint64_t sectors{};
    for (const auto index : partition.extent_indices) {
        const auto count = metadata.extents[index].sectors;
        if (count > std::numeric_limits<std::uint64_t>::max() - sectors) {
            return std::unexpected(error(SuperMetadataErrorKind::Overflow,
                                         "logical partition size overflows uint64"));
        }
        sectors += count;
    }
    if (sectors > std::numeric_limits<std::uint64_t>::max() / kSectorSize) {
        return std::unexpected(error(SuperMetadataErrorKind::Overflow,
                                     "logical partition byte size overflows uint64"));
    }
    return sectors * kSectorSize;
}

[[nodiscard]] std::uint64_t align_up(const std::uint64_t value,
                                     const std::uint64_t alignment) noexcept {
    if (alignment == 0U || value % alignment == 0U) {
        return value;
    }
    const auto delta = alignment - value % alignment;
    return delta > std::numeric_limits<std::uint64_t>::max() - value
               ? std::numeric_limits<std::uint64_t>::max()
               : value + delta;
}

struct Interval final {
    std::uint64_t begin{};
    std::uint64_t end{};
};

[[nodiscard]] std::vector<Interval> free_intervals(
    const Metadata& metadata, const std::size_t device_index) {
    const auto& device = metadata.block_devices[device_index];
    std::vector<Interval> used;
    for (const auto& extent : metadata.extents) {
        if (extent.type == kLinearExtent && extent.target_source == device_index &&
            extent.sectors != 0U) {
            used.push_back({extent.target_data,
                            extent.target_data + extent.sectors});
        }
    }
    std::sort(used.begin(), used.end(), [](const auto& left, const auto& right) {
        return std::tie(left.begin, left.end) < std::tie(right.begin, right.end);
    });
    std::vector<Interval> merged;
    for (const auto& interval : used) {
        if (!merged.empty() && interval.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, interval.end);
        } else {
            merged.push_back(interval);
        }
    }
    std::vector<Interval> result;
    auto cursor = device.first_sector;
    const auto limit = device.size / kSectorSize;
    for (const auto& interval : merged) {
        if (cursor < interval.begin) {
            result.push_back({cursor, interval.begin});
        }
        cursor = std::max(cursor, interval.end);
    }
    if (cursor < limit) {
        result.push_back({cursor, limit});
    }
    return result;
}

[[nodiscard]] std::uint64_t aligned_sector(const BlockDevice& device,
                                           const std::uint64_t sector,
                                           const std::uint64_t logical_block) {
    const auto alignment = std::max<std::uint64_t>(
        logical_block, device.alignment == 0U ? logical_block : device.alignment);
    const auto bytes = sector * kSectorSize;
    const auto offset = device.alignment_offset % alignment;
    const auto adjusted = bytes >= offset ? bytes - offset : 0U;
    const auto aligned = align_up(adjusted, alignment);
    if (aligned == std::numeric_limits<std::uint64_t>::max() ||
        aligned > std::numeric_limits<std::uint64_t>::max() - offset) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (aligned + offset) / kSectorSize;
}

[[nodiscard]] std::expected<void, SuperMetadataError> resize_partition(
    Metadata& metadata, const std::string_view name,
    const std::uint64_t requested_size) {
    const auto found = std::find_if(
        metadata.partitions.begin(), metadata.partitions.end(),
        [name](const Partition& partition) { return partition.name == name; });
    if (found == metadata.partitions.end()) {
        return std::unexpected(error(SuperMetadataErrorKind::PartitionNotFound,
                                     "logical partition was not found in super metadata"));
    }
    auto& partition = *found;
    const auto aligned_size = align_up(requested_size,
                                       metadata.geometry.logical_block_size);
    if (aligned_size == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(error(SuperMetadataErrorKind::Overflow,
                                     "requested logical partition size overflows alignment"));
    }
    auto current_result = partition_bytes(metadata, partition);
    if (!current_result) {
        return std::unexpected(std::move(current_result.error()));
    }
    const auto current_size = *current_result;
    if (aligned_size == current_size) {
        return {};
    }
    if (partition.group_index >= metadata.groups.size()) {
        return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                     "logical partition group index is invalid"));
    }
    const auto group_limit = metadata.groups[partition.group_index].maximum_size;
    if (group_limit != 0U) {
        std::uint64_t group_other{};
        for (const auto& candidate : metadata.partitions) {
            if (&candidate == &partition ||
                candidate.group_index != partition.group_index) {
                continue;
            }
            auto bytes = partition_bytes(metadata, candidate);
            if (!bytes || *bytes > std::numeric_limits<std::uint64_t>::max() -
                                      group_other) {
                return std::unexpected(error(SuperMetadataErrorKind::Overflow,
                                             "logical partition group size overflows"));
            }
            group_other += *bytes;
        }
        if (group_other > group_limit || aligned_size > group_limit - group_other) {
            return std::unexpected(error(SuperMetadataErrorKind::NoSpace,
                                         "logical partition group has insufficient space"));
        }
    }

    const auto target_sectors = aligned_size / kSectorSize;
    auto partition_sectors = current_size / kSectorSize;
    if (target_sectors < partition_sectors) {
        auto remove = partition_sectors - target_sectors;
        while (remove != 0U && !partition.extent_indices.empty()) {
            auto& extent = metadata.extents[partition.extent_indices.back()];
            if (remove < extent.sectors) {
                extent.sectors -= remove;
                remove = 0U;
            } else {
                remove -= extent.sectors;
                extent.sectors = 0U;
                partition.extent_indices.pop_back();
            }
        }
        return remove == 0U
                   ? std::expected<void, SuperMetadataError>{}
                   : std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                           "logical partition shrink exceeded its extents"));
    }

    auto remaining = target_sectors - partition_sectors;
    const auto logical_sectors =
        static_cast<std::uint64_t>(metadata.geometry.logical_block_size) /
        kSectorSize;

    // liblp first extends the final linear extent when the immediately
    // following sectors are free. This preserves the existing allocation and
    // avoids introducing a second extent solely because its start would not
    // satisfy the block-device alignment used for new allocations.
    if (!partition.extent_indices.empty()) {
        auto& tail = metadata.extents[partition.extent_indices.back()];
        if (tail.type == kLinearExtent &&
            tail.target_source < metadata.block_devices.size()) {
            const auto tail_end = tail.target_data + tail.sectors;
            auto free_end = metadata.block_devices[tail.target_source].size /
                            kSectorSize;
            for (std::size_t index = 0; index < metadata.extents.size(); ++index) {
                if (index == partition.extent_indices.back()) {
                    continue;
                }
                const auto& candidate = metadata.extents[index];
                if (candidate.type == kLinearExtent &&
                    candidate.target_source == tail.target_source &&
                    candidate.target_data >= tail_end) {
                    free_end = std::min(free_end, candidate.target_data);
                }
            }
            if (free_end > tail_end) {
                auto available = free_end - tail_end;
                available -= available % logical_sectors;
                const auto amount = std::min(remaining, available);
                tail.sectors += amount;
                remaining -= amount;
            }
        }
    }

    const auto allocate_on_device = [&](const std::size_t device_index) {
        if (remaining == 0U) {
            return;
        }
        const auto& device = metadata.block_devices[device_index];
        auto free = free_intervals(metadata, device_index);
        for (const auto interval : free) {
            if (remaining == 0U) {
                break;
            }
            auto begin = aligned_sector(device, interval.begin,
                                        metadata.geometry.logical_block_size);
            if (begin == std::numeric_limits<std::uint64_t>::max() ||
                begin >= interval.end) {
                continue;
            }
            auto available = interval.end - begin;
            available -= available % logical_sectors;
            auto amount = std::min(remaining, available);
            amount -= amount % logical_sectors;
            if (amount == 0U) {
                continue;
            }
            Extent extent{
                .raw = std::vector<std::byte>(
                    metadata.extent_descriptor.entry_size),
                .sectors = amount,
                .type = kLinearExtent,
                .target_data = begin,
                .target_source = static_cast<std::uint32_t>(device_index),
            };
            metadata.extents.push_back(std::move(extent));
            partition.extent_indices.push_back(metadata.extents.size() - 1U);
            remaining -= amount;
        }
    };

    std::vector<std::size_t> device_order;
    if (!partition.extent_indices.empty()) {
        const auto source = metadata.extents[partition.extent_indices.back()].target_source;
        if (source < metadata.block_devices.size()) {
            device_order.push_back(source);
        }
    }
    for (std::size_t index = 0; index < metadata.block_devices.size(); ++index) {
        if (std::find(device_order.begin(), device_order.end(), index) ==
            device_order.end()) {
            device_order.push_back(index);
        }
    }
    for (const auto index : device_order) {
        allocate_on_device(index);
    }
    if (remaining != 0U) {
        return std::unexpected(error(SuperMetadataErrorKind::NoSpace,
                                     "super partition has insufficient free space"));
    }
    return {};
}

[[nodiscard]] std::expected<std::vector<std::byte>, SuperMetadataError>
serialize_metadata(Metadata& metadata) {
    std::vector<Extent> ordered_extents;
    ordered_extents.reserve(metadata.extents.size());
    for (auto& partition : metadata.partitions) {
        std::vector<std::size_t> new_indices;
        new_indices.reserve(partition.extent_indices.size());
        for (const auto old_index : partition.extent_indices) {
            if (old_index >= metadata.extents.size() ||
                metadata.extents[old_index].sectors == 0U) {
                continue;
            }
            new_indices.push_back(ordered_extents.size());
            ordered_extents.push_back(metadata.extents[old_index]);
        }
        partition.extent_indices = std::move(new_indices);
    }
    metadata.extents = std::move(ordered_extents);

    const auto checked_size = [](const std::size_t count,
                                 const std::uint32_t entry_size)
        -> std::optional<std::size_t> {
        if (entry_size == 0U ||
            count > std::numeric_limits<std::size_t>::max() / entry_size) {
            return std::nullopt;
        }
        return count * entry_size;
    };
    const auto partition_bytes = checked_size(
        metadata.partitions.size(), metadata.partition_descriptor.entry_size);
    const auto extent_bytes = checked_size(
        metadata.extents.size(), metadata.extent_descriptor.entry_size);
    const auto group_bytes = checked_size(
        metadata.groups.size(), metadata.group_descriptor.entry_size);
    const auto device_bytes = checked_size(
        metadata.block_devices.size(), metadata.block_device_descriptor.entry_size);
    if (!partition_bytes || !extent_bytes || !group_bytes || !device_bytes ||
        *partition_bytes > std::numeric_limits<std::size_t>::max() - *extent_bytes ||
        *partition_bytes + *extent_bytes >
            std::numeric_limits<std::size_t>::max() - *group_bytes ||
        *partition_bytes + *extent_bytes + *group_bytes >
            std::numeric_limits<std::size_t>::max() - *device_bytes) {
        return std::unexpected(error(SuperMetadataErrorKind::Overflow,
                                     "serialized logical partition tables overflow"));
    }
    const auto tables_size =
        *partition_bytes + *extent_bytes + *group_bytes + *device_bytes;
    if (metadata.header.size() > metadata.geometry.metadata_max_size ||
        tables_size > metadata.geometry.metadata_max_size - metadata.header.size() ||
        tables_size > std::numeric_limits<std::uint32_t>::max() ||
        metadata.partitions.size() > std::numeric_limits<std::uint32_t>::max() ||
        metadata.extents.size() > std::numeric_limits<std::uint32_t>::max() ||
        metadata.groups.size() > std::numeric_limits<std::uint32_t>::max() ||
        metadata.block_devices.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(error(SuperMetadataErrorKind::NoSpace,
                                     "resized logical partition metadata exceeds its slot"));
    }

    metadata.partition_descriptor = {
        0U, static_cast<std::uint32_t>(metadata.partitions.size()),
        metadata.partition_descriptor.entry_size};
    metadata.extent_descriptor = {
        static_cast<std::uint32_t>(*partition_bytes),
        static_cast<std::uint32_t>(metadata.extents.size()),
        metadata.extent_descriptor.entry_size};
    metadata.group_descriptor = {
        static_cast<std::uint32_t>(*partition_bytes + *extent_bytes),
        static_cast<std::uint32_t>(metadata.groups.size()),
        metadata.group_descriptor.entry_size};
    metadata.block_device_descriptor = {
        static_cast<std::uint32_t>(*partition_bytes + *extent_bytes + *group_bytes),
        static_cast<std::uint32_t>(metadata.block_devices.size()),
        metadata.block_device_descriptor.entry_size};

    std::vector<std::byte> tables(tables_size);
    std::size_t cursor{};
    std::uint32_t next_extent{};
    for (auto& partition : metadata.partitions) {
        auto entry = std::span<std::byte>{tables}.subspan(
            cursor, metadata.partition_descriptor.entry_size);
        std::copy(partition.raw.begin(), partition.raw.end(), entry.begin());
        write_le<std::uint32_t>(entry, 36U, partition.attributes);
        write_le<std::uint32_t>(entry, 40U, next_extent);
        write_le<std::uint32_t>(
            entry, 44U,
            static_cast<std::uint32_t>(partition.extent_indices.size()));
        write_le<std::uint32_t>(entry, 48U, partition.group_index);
        next_extent += static_cast<std::uint32_t>(partition.extent_indices.size());
        cursor += entry.size();
    }
    for (auto& extent : metadata.extents) {
        auto entry = std::span<std::byte>{tables}.subspan(
            cursor, metadata.extent_descriptor.entry_size);
        std::copy(extent.raw.begin(), extent.raw.end(), entry.begin());
        write_le<std::uint64_t>(entry, 0U, extent.sectors);
        write_le<std::uint32_t>(entry, 8U, extent.type);
        write_le<std::uint64_t>(entry, 12U, extent.target_data);
        write_le<std::uint32_t>(entry, 20U, extent.target_source);
        cursor += entry.size();
    }
    for (const auto& group : metadata.groups) {
        std::copy(group.raw.begin(), group.raw.end(),
                  tables.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += group.raw.size();
    }
    for (const auto& device : metadata.block_devices) {
        std::copy(device.raw.begin(), device.raw.end(),
                  tables.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor += device.raw.size();
    }

    auto header = metadata.header;
    auto header_span = std::span<std::byte>{header};
    write_le<std::uint32_t>(header_span, 44U,
                            static_cast<std::uint32_t>(tables.size()));
    const auto write_descriptor = [&header_span](const std::size_t offset,
                                                 const Descriptor value) {
        return write_le<std::uint32_t>(header_span, offset, value.offset) &&
               write_le<std::uint32_t>(header_span, offset + 4U, value.count) &&
               write_le<std::uint32_t>(header_span, offset + 8U, value.entry_size);
    };
    if (!write_descriptor(80U, metadata.partition_descriptor) ||
        !write_descriptor(92U, metadata.extent_descriptor) ||
        !write_descriptor(104U, metadata.group_descriptor) ||
        !write_descriptor(116U, metadata.block_device_descriptor)) {
        return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                     "logical partition metadata header is truncated"));
    }
    const auto tables_digest = digest(tables);
    std::copy(tables_digest.begin(), tables_digest.end(), header.begin() + 48);
    std::fill_n(header.begin() + 12, kSha256DigestSize, std::byte{});
    const auto header_digest = digest(header);
    std::copy(header_digest.begin(), header_digest.end(), header.begin() + 12);

    std::vector<std::byte> area(metadata.geometry.metadata_max_size);
    std::copy(header.begin(), header.end(), area.begin());
    std::copy(tables.begin(), tables.end(),
              area.begin() + static_cast<std::ptrdiff_t>(header.size()));
    return area;
}

void append_u16(std::vector<std::byte>& output, const std::uint16_t value) {
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        output.push_back(static_cast<std::byte>(
            (value >> static_cast<unsigned int>(index * 8U)) & 0xffU));
    }
}

[[nodiscard]] std::expected<std::vector<std::byte>, SuperMetadataError>
make_sparse_metadata_image(const Metadata& metadata,
                           std::vector<std::byte> area) {
    const auto metadata_offset =
        kPrimaryMetadataOffset + static_cast<std::uint64_t>(metadata.slot) *
                                     metadata.geometry.metadata_max_size;
    if (metadata_offset % kSparseBlockSize != 0U ||
        area.size() % kSparseBlockSize != 0U) {
        return std::unexpected(error(
            SuperMetadataErrorKind::Unsupported,
            "logical partition metadata is not aligned for Android sparse output"));
    }
    const auto leading_blocks = metadata_offset / kSparseBlockSize;
    const auto raw_blocks = area.size() / kSparseBlockSize;
    if (leading_blocks == 0U || leading_blocks > std::numeric_limits<std::uint32_t>::max() ||
        raw_blocks == 0U || raw_blocks > std::numeric_limits<std::uint32_t>::max() ||
        leading_blocks + raw_blocks > std::numeric_limits<std::uint32_t>::max() ||
        area.size() > std::numeric_limits<std::uint32_t>::max() - 12U) {
        return std::unexpected(error(SuperMetadataErrorKind::Overflow,
                                     "Android sparse metadata image exceeds format limits"));
    }
    std::vector<std::byte> output;
    output.reserve(28U + 12U + 12U + area.size());
    append_u32(output, kAndroidSparseMagic);
    append_u16(output, kAndroidSparseMajorVersion);
    append_u16(output, 0U);
    append_u16(output, 28U);
    append_u16(output, 12U);
    append_u32(output, kSparseBlockSize);
    append_u32(output, static_cast<std::uint32_t>(leading_blocks + raw_blocks));
    append_u32(output, 2U);
    append_u32(output, 0U);
    append_u16(output, static_cast<std::uint16_t>(kSparseDontCareChunk));
    append_u16(output, 0U);
    append_u32(output, static_cast<std::uint32_t>(leading_blocks));
    append_u32(output, 12U);
    append_u16(output, static_cast<std::uint16_t>(kSparseRawChunk));
    append_u16(output, 0U);
    append_u32(output, static_cast<std::uint32_t>(raw_blocks));
    append_u32(output, static_cast<std::uint32_t>(12U + area.size()));
    output.insert(output.end(), area.begin(), area.end());
    return output;
}

}  // namespace

std::expected<std::vector<std::byte>, SuperMetadataError>
resize_fetched_super_metadata(const std::span<const std::byte> fetched_super,
                              const std::string_view partition,
                              const std::uint64_t requested_size) {
    if (partition.empty()) {
        return std::unexpected(error(SuperMetadataErrorKind::Malformed,
                                     "logical partition name is empty"));
    }
    auto geometry = parse_geometry(fetched_super);
    if (!geometry) {
        return std::unexpected(std::move(geometry.error()));
    }
    const auto slot = metadata_slot_for(partition, *geometry);
    if (!slot) {
        return std::unexpected(error(SuperMetadataErrorKind::Unsupported,
                                     "logical partition slot is unsupported"));
    }
    auto metadata = parse_metadata(fetched_super, *geometry, *slot);
    if (!metadata) {
        return std::unexpected(std::move(metadata.error()));
    }
    if (auto resized = resize_partition(*metadata, partition, requested_size);
        !resized) {
        return std::unexpected(std::move(resized.error()));
    }
    auto serialized = serialize_metadata(*metadata);
    if (!serialized) {
        return std::unexpected(std::move(serialized.error()));
    }
    return make_sparse_metadata_image(*metadata, std::move(*serialized));
}

}  // namespace kairosboot::image
