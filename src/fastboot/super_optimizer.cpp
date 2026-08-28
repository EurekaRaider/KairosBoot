// SPDX-License-Identifier: MIT
#include "super_optimizer.hpp"

#include "src/image/sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace kairosboot::fastboot {
namespace {

constexpr std::uint32_t kGeometryMagic = 0x616c4467U;
constexpr std::uint32_t kHeaderMagic = 0x414c5030U;
constexpr std::uint32_t kSparseMagic = image::kAndroidSparseMagic;
constexpr std::uint64_t kSectorSize = 512U;
constexpr std::uint64_t kReservedBytes = 4096U;
constexpr std::uint64_t kGeometryRegionSize = 4096U;
constexpr std::uint64_t kPrimaryGeometryOffset = kReservedBytes;
constexpr std::uint64_t kBackupGeometryOffset =
    kPrimaryGeometryOffset + kGeometryRegionSize;
constexpr std::uint32_t kGeometryStructSize = 52U;
constexpr std::uint32_t kHeaderV10Size = 128U;
constexpr std::uint32_t kHeaderV12Size = 256U;
constexpr std::uint32_t kMaximumMetadataSize = 16U * 1024U * 1024U;
constexpr std::uint32_t kPartitionEntrySize = 52U;
constexpr std::uint32_t kExtentEntrySize = 24U;
constexpr std::uint32_t kGroupEntrySize = 48U;
constexpr std::uint32_t kBlockDeviceEntrySize = 64U;
constexpr std::uint32_t kPartitionReadOnly = 1U << 0U;
constexpr std::string_view kOptimizedName{"kairosboot-optimized-super.img"};

[[nodiscard]] SuperOptimizationError fail(
    const SuperOptimizationErrorKind kind, std::string message) {
    return {.kind = kind, .message = std::move(message)};
}

[[nodiscard]] std::uint16_t u16(const std::span<const std::byte> bytes,
                                const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t u32(const std::span<const std::byte> bytes,
                                const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(u16(bytes, offset)) |
           (static_cast<std::uint32_t>(u16(bytes, offset + 2U)) << 16U);
}

[[nodiscard]] std::uint64_t u64(const std::span<const std::byte> bytes,
                                const std::size_t offset) noexcept {
    return static_cast<std::uint64_t>(u32(bytes, offset)) |
           (static_cast<std::uint64_t>(u32(bytes, offset + 4U)) << 32U);
}

void put_u16(std::span<std::byte> bytes, const std::size_t offset,
             const std::uint16_t value) noexcept {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(std::span<std::byte> bytes, const std::size_t offset,
             const std::uint32_t value) noexcept {
    put_u16(bytes, offset, static_cast<std::uint16_t>(value & 0xffffU));
    put_u16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> 16U));
}

void put_u64(std::span<std::byte> bytes, const std::size_t offset,
             const std::uint64_t value) noexcept {
    put_u32(bytes, offset, static_cast<std::uint32_t>(value));
    put_u32(bytes, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] bool add_overflows(const std::uint64_t left,
                                 const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left;
}

[[nodiscard]] std::optional<std::uint64_t> align_up(
    const std::uint64_t value, const std::uint64_t alignment,
    const std::uint64_t alignment_offset = 0U) noexcept {
    if (alignment == 0U || alignment_offset >= alignment) {
        return std::nullopt;
    }
    const auto remainder = value % alignment;
    const auto desired = alignment_offset % alignment;
    const auto increment = remainder <= desired ? desired - remainder
                                                 : alignment - remainder + desired;
    if (add_overflows(value, increment)) {
        return std::nullopt;
    }
    return value + increment;
}

[[nodiscard]] std::expected<void, SuperOptimizationError> read_exact(
    const image::IImageSource& source, const std::uint64_t offset,
    const std::span<std::byte> output,
    const std::stop_token cancellation) {
    std::size_t completed = 0U;
    while (completed < output.size()) {
        if (cancellation.stop_requested()) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::Cancelled,
                "super optimization was cancelled while reading an image"));
        }
        auto read = source.read_at(
            offset + completed, output.subspan(completed));
        if (!read) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::Source,
                "unable to read super optimization input: " +
                    read.error().message));
        }
        if (*read == 0U || *read > output.size() - completed) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::Source,
                "super optimization input was truncated"));
        }
        completed += *read;
    }
    return {};
}

class ExpandedSuperReader final {
public:
    explicit ExpandedSuperReader(const image::FlashArtifact& artifact) noexcept
        : source_(artifact.transfer_source()), sparse_(artifact.sparse_image()),
          size_(artifact.metadata().expanded_size) {}

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

    [[nodiscard]] std::expected<void, SuperOptimizationError> read(
        const std::uint64_t offset, const std::span<std::byte> output,
        const std::stop_token cancellation) const {
        if (offset > size_ || output.size() > size_ - offset) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::InvalidMetadata,
                "LP metadata range exceeds super_empty.img"));
        }
        if (!sparse_) {
            return read_exact(*source_, offset, output, cancellation);
        }
        std::size_t completed = 0U;
        while (completed < output.size()) {
            if (cancellation.stop_requested()) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::Cancelled,
                    "super optimization was cancelled while expanding super_empty.img"));
            }
            auto read = sparse_->read_at(offset + completed,
                                         output.subspan(completed));
            if (!read) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::Source,
                    "unable to expand super_empty.img: " + read.error().message));
            }
            if (*read == 0U || *read > output.size() - completed) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::Source,
                    "expanded super_empty.img was truncated"));
            }
            completed += *read;
        }
        return {};
    }

private:
    std::shared_ptr<const image::IImageSource> source_;
    const image::SparseImage* sparse_{};
    std::uint64_t size_{};
};

struct Geometry final {
    std::uint32_t metadata_max_size{};
    std::uint32_t metadata_slot_count{};
    std::uint32_t logical_block_size{};
    std::vector<std::byte> serialized{};
};

struct Descriptor final {
    std::uint32_t offset{};
    std::uint32_t count{};
    std::uint32_t entry_size{};
};

struct Partition final {
    std::string name{};
    std::uint32_t attributes{};
    std::uint32_t first_extent{};
    std::uint32_t extent_count{};
    std::uint32_t group_index{};
};

struct Group final {
    std::string name{};
    std::uint32_t flags{};
    std::uint64_t maximum_size{};
};

struct BlockDevice final {
    std::uint64_t first_logical_sector{};
    std::uint32_t alignment{};
    std::uint32_t alignment_offset{};
    std::uint64_t size{};
    std::string name{};
    std::uint32_t flags{};
};

struct Metadata final {
    Geometry geometry{};
    std::uint16_t minor_version{};
    std::uint32_t header_size{};
    std::uint32_t flags{};
    std::vector<Partition> partitions{};
    std::vector<Group> groups{};
    BlockDevice block_device{};
};

[[nodiscard]] bool valid_name_character(const char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

[[nodiscard]] std::expected<std::string, SuperOptimizationError> read_name(
    const std::span<const std::byte> record, const std::size_t offset,
    const std::size_t capacity, const std::string_view kind) {
    std::size_t length = 0U;
    while (length < capacity && record[offset + length] != std::byte{0}) {
        const auto value = static_cast<char>(
            std::to_integer<unsigned char>(record[offset + length]));
        if (!valid_name_character(value)) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::InvalidMetadata,
                "LP " + std::string(kind) + " name contains an invalid character"));
        }
        ++length;
    }
    if (length == 0U || length == capacity) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP " + std::string(kind) + " name is empty or unterminated"));
    }
    for (std::size_t index = length; index < capacity; ++index) {
        if (record[offset + index] != std::byte{0}) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::InvalidMetadata,
                "LP " + std::string(kind) + " name padding is not zero"));
        }
    }
    return std::string(reinterpret_cast<const char*>(record.data() + offset),
                       length);
}

[[nodiscard]] image::Sha256Digest digest_bytes(
    const std::span<const std::byte> bytes) noexcept {
    image::Sha256Accumulator hash;
    hash.update(bytes);
    return hash.finish();
}

[[nodiscard]] bool all_zero(const std::span<const std::byte> bytes) noexcept {
    return std::ranges::all_of(bytes,
                               [](const std::byte value) { return value == std::byte{0}; });
}

[[nodiscard]] std::expected<Geometry, SuperOptimizationError> parse_geometry(
    const ExpandedSuperReader& reader, const std::stop_token cancellation) {
    if (reader.size() < kBackupGeometryOffset + kGeometryRegionSize) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "super_empty.img is too small for primary and backup LP geometry"));
    }
    std::vector<std::byte> primary(kGeometryRegionSize);
    std::vector<std::byte> backup(kGeometryRegionSize);
    if (auto read = reader.read(kPrimaryGeometryOffset, primary, cancellation);
        !read) {
        return std::unexpected(std::move(read.error()));
    }
    if (auto read = reader.read(kBackupGeometryOffset, backup, cancellation);
        !read) {
        return std::unexpected(std::move(read.error()));
    }
    if (primary != backup || u32(primary, 0U) != kGeometryMagic ||
        u32(primary, 4U) != kGeometryStructSize) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP geometry copies, magic or structure size are inconsistent"));
    }
    auto checksum_input = std::vector<std::byte>(
        primary.begin(), primary.begin() + kGeometryStructSize);
    const auto stored = std::span(primary).subspan(8U, 32U);
    std::fill(checksum_input.begin() + 8, checksum_input.begin() + 40,
              std::byte{0});
    if (!std::ranges::equal(stored, digest_bytes(checksum_input))) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP geometry checksum mismatch"));
    }
    if (!all_zero(std::span(primary).subspan(kGeometryStructSize))) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP geometry reserved bytes are not zero"));
    }
    Geometry result{
        .metadata_max_size = u32(primary, 40U),
        .metadata_slot_count = u32(primary, 44U),
        .logical_block_size = u32(primary, 48U),
        .serialized = std::move(primary),
    };
    if (result.metadata_max_size < kHeaderV10Size ||
        result.metadata_max_size > kMaximumMetadataSize ||
        result.metadata_max_size % kSectorSize != 0U ||
        result.metadata_slot_count == 0U || result.metadata_slot_count > 32U ||
        result.logical_block_size < kSectorSize ||
        result.logical_block_size % kSectorSize != 0U ||
        !std::has_single_bit(result.logical_block_size)) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP geometry contains unsupported metadata or block sizing"));
    }
    return result;
}

[[nodiscard]] std::expected<std::vector<std::byte>, SuperOptimizationError>
read_metadata_copy(const ExpandedSuperReader& reader, const Geometry& geometry,
                   const std::uint64_t offset,
                   const std::stop_token cancellation) {
    if (offset > reader.size() ||
        geometry.metadata_max_size > reader.size() - offset) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP metadata copy exceeds super_empty.img"));
    }
    std::vector<std::byte> copy(geometry.metadata_max_size);
    if (auto read = reader.read(offset, copy, cancellation); !read) {
        return std::unexpected(std::move(read.error()));
    }
    return copy;
}

[[nodiscard]] Descriptor descriptor(const std::span<const std::byte> header,
                                    const std::size_t offset) noexcept {
    return {.offset = u32(header, offset),
            .count = u32(header, offset + 4U),
            .entry_size = u32(header, offset + 8U)};
}

[[nodiscard]] std::expected<std::uint64_t, SuperOptimizationError>
descriptor_end(const Descriptor& value, const std::uint32_t tables_size,
               const std::uint32_t expected_entry_size,
               const std::string_view name) {
    if (value.entry_size != expected_entry_size) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::IncompatibleLayout,
            "LP " + std::string(name) + " table entry size is unsupported"));
    }
    const auto bytes = static_cast<std::uint64_t>(value.count) * value.entry_size;
    const auto end = static_cast<std::uint64_t>(value.offset) + bytes;
    if (end > tables_size) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP " + std::string(name) + " table exceeds the table block"));
    }
    return end;
}

[[nodiscard]] std::expected<Metadata, SuperOptimizationError> parse_metadata(
    const ExpandedSuperReader& reader, Geometry geometry,
    const std::stop_token cancellation) {
    const auto primary_start = kBackupGeometryOffset + kGeometryRegionSize;
    auto first = read_metadata_copy(reader, geometry, primary_start, cancellation);
    if (!first) {
        return std::unexpected(std::move(first.error()));
    }
    const auto primary_bytes = static_cast<std::uint64_t>(geometry.metadata_max_size) *
                               geometry.metadata_slot_count;
    if (add_overflows(primary_start, primary_bytes)) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP metadata slot offsets overflow"));
    }
    const auto backup_start = primary_start + primary_bytes;
    for (std::uint32_t slot = 0U; slot < geometry.metadata_slot_count; ++slot) {
        for (const auto base : {primary_start, backup_start}) {
            const auto offset = base +
                                static_cast<std::uint64_t>(slot) *
                                    geometry.metadata_max_size;
            auto copy = read_metadata_copy(reader, geometry, offset, cancellation);
            if (!copy) {
                return std::unexpected(std::move(copy.error()));
            }
            if (*copy != *first) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::SlotMismatch,
                    "LP primary/backup metadata slots are not identical"));
            }
        }
    }

    const auto header_span = std::span(*first);
    if (u32(header_span, 0U) != kHeaderMagic || u16(header_span, 4U) != 10U) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP metadata header magic or major version is unsupported"));
    }
    const auto minor = u16(header_span, 6U);
    const auto header_size = u32(header_span, 8U);
    if (minor > 2U ||
        !((minor < 2U && header_size == kHeaderV10Size) ||
          (minor == 2U && header_size == kHeaderV12Size)) ||
        header_size > geometry.metadata_max_size) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::IncompatibleLayout,
            "LP metadata header version or size is unsupported"));
    }
    std::array<std::byte, kHeaderV12Size> checked_header{};
    std::copy_n(first->begin(), header_size, checked_header.begin());
    const auto stored_header = std::span(*first).subspan(12U, 32U);
    std::fill(checked_header.begin() + 12, checked_header.begin() + 44,
              std::byte{0});
    if (!std::ranges::equal(
            stored_header,
            digest_bytes(std::span(checked_header).first(header_size)))) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP metadata header checksum mismatch"));
    }
    const auto tables_size = u32(header_span, 44U);
    if (tables_size > geometry.metadata_max_size - header_size) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP metadata tables exceed the reserved slot"));
    }
    const auto tables = std::span(*first).subspan(header_size, tables_size);
    if (!std::ranges::equal(std::span(*first).subspan(48U, 32U),
                            digest_bytes(tables))) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP metadata table checksum mismatch"));
    }
    if (!all_zero(std::span(*first).subspan(header_size + tables_size))) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "LP metadata slot padding is not zero"));
    }

    const auto partitions = descriptor(header_span, 80U);
    const auto extents = descriptor(header_span, 92U);
    const auto groups = descriptor(header_span, 104U);
    const auto devices = descriptor(header_span, 116U);
    auto partition_end = descriptor_end(
        partitions, tables_size, kPartitionEntrySize, "partition");
    auto extent_end = descriptor_end(extents, tables_size, kExtentEntrySize,
                                     "extent");
    auto group_end = descriptor_end(groups, tables_size, kGroupEntrySize,
                                    "group");
    auto device_end = descriptor_end(devices, tables_size,
                                     kBlockDeviceEntrySize, "block device");
    if (!partition_end || !extent_end || !group_end || !device_end) {
        return std::unexpected(!partition_end   ? std::move(partition_end.error())
                               : !extent_end    ? std::move(extent_end.error())
                               : !group_end     ? std::move(group_end.error())
                                                : std::move(device_end.error()));
    }
    if (partitions.offset != 0U || *partition_end != extents.offset ||
        *extent_end != groups.offset || *group_end != devices.offset ||
        *device_end != tables_size || extents.count != 0U ||
        devices.count != 1U || groups.count == 0U || partitions.count == 0U) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::IncompatibleLayout,
            "LP tables are not one canonical empty single-device super layout"));
    }

    Metadata result{
        .geometry = std::move(geometry),
        .minor_version = minor,
        .header_size = header_size,
        .flags = header_size == kHeaderV12Size ? u32(header_span, 128U) : 0U,
    };
    if (header_size == kHeaderV12Size &&
        !all_zero(header_span.subspan(132U, 124U))) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::IncompatibleLayout,
            "LP expanded header reserved bytes are not zero"));
    }

    std::set<std::string, std::less<>> names;
    result.partitions.reserve(partitions.count);
    for (std::uint32_t index = 0U; index < partitions.count; ++index) {
        const auto record = tables.subspan(
            partitions.offset + static_cast<std::size_t>(index) * partitions.entry_size,
            partitions.entry_size);
        auto name = read_name(record, 0U, 36U, "partition");
        if (!name) {
            return std::unexpected(std::move(name.error()));
        }
        if (!names.insert(*name).second) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::InvalidMetadata,
                "LP partition names are duplicated"));
        }
        const auto attributes = u32(record, 36U);
        const auto first_extent = u32(record, 40U);
        const auto extent_count = u32(record, 44U);
        const auto group_index = u32(record, 48U);
        // Only the immutable read-only bit is supported by this writer. In
        // particular, SLOT_SUFFIXED, UPDATED, DISABLED and future bits must not
        // be copied into newly synthesized metadata.
        if (attributes != kPartitionReadOnly || first_extent != 0U ||
            extent_count != 0U || group_index >= groups.count) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::IncompatibleLayout,
                "LP partition attributes, extents or group index are not optimizable"));
        }
        result.partitions.push_back(Partition{
            .name = std::move(*name),
            .attributes = attributes,
            .first_extent = 0U,
            .extent_count = 0U,
            .group_index = group_index,
        });
    }

    names.clear();
    result.groups.reserve(groups.count);
    for (std::uint32_t index = 0U; index < groups.count; ++index) {
        const auto record = tables.subspan(
            groups.offset + static_cast<std::size_t>(index) * groups.entry_size,
            groups.entry_size);
        auto name = read_name(record, 0U, 36U, "group");
        if (!name) {
            return std::unexpected(std::move(name.error()));
        }
        if (!names.insert(*name).second || u32(record, 36U) != 0U) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::IncompatibleLayout,
                "LP groups are duplicated or use unsupported flags"));
        }
        result.groups.push_back(Group{
            .name = std::move(*name),
            .flags = u32(record, 36U),
            .maximum_size = u64(record, 40U),
        });
    }

    const auto record = tables.subspan(devices.offset, devices.entry_size);
    auto name = read_name(record, 24U, 36U, "block device");
    if (!name) {
        return std::unexpected(std::move(name.error()));
    }
    result.block_device = BlockDevice{
        .first_logical_sector = u64(record, 0U),
        .alignment = u32(record, 8U),
        .alignment_offset = u32(record, 12U),
        .size = u64(record, 16U),
        .name = std::move(*name),
        .flags = u32(record, 60U),
    };
    const auto metadata_reserved = backup_start + primary_bytes;
    if (result.block_device.flags != 0U ||
        result.block_device.size != reader.size() ||
        result.block_device.first_logical_sector >
            result.block_device.size / kSectorSize ||
        result.block_device.first_logical_sector * kSectorSize <
            metadata_reserved ||
        result.block_device.alignment % kSectorSize != 0U ||
        (result.block_device.alignment != 0U &&
         (result.block_device.alignment < result.geometry.logical_block_size ||
          result.block_device.alignment % result.geometry.logical_block_size != 0U ||
          result.block_device.alignment_offset %
                  result.geometry.logical_block_size !=
              0U)) ||
        result.block_device.alignment_offset >=
            std::max(result.block_device.alignment, 1U)) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::IncompatibleLayout,
            "LP block-device geometry is inconsistent with super_empty.img"));
    }
    return result;
}

struct SelectedImage final {
    std::size_t task_index{};
    std::size_t partition_index{};
    std::string partition_name{};
    std::shared_ptr<const image::IImageSource> source{};
    std::uint64_t size{};
    std::uint64_t allocated_size{};
    std::uint64_t physical_offset{};
};

[[nodiscard]] std::optional<std::size_t> find_partition(
    const Metadata& metadata, const std::string_view name) noexcept {
    for (std::size_t index = 0U; index < metadata.partitions.size(); ++index) {
        if (metadata.partitions[index].name == name) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<std::string, SuperOptimizationError>
resolve_partition_name(const Metadata& metadata, const PlannedUpdateTask& task,
                       const std::string_view current_slot) {
    if (find_partition(metadata, task.partition)) {
        return task.partition;
    }
    if (current_slot.empty()) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::SlotMismatch,
            "A/B current-slot is required to resolve dynamic partitions"));
    }
    std::string slot{current_slot};
    if (slot.starts_with('_')) {
        slot.erase(slot.begin());
    }
    if (slot.empty() ||
        !std::ranges::all_of(slot, valid_name_character)) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::SlotMismatch,
            "device current-slot is not one valid LP suffix"));
    }
    if (task.slot == PlannedSlot::Other) {
        if (slot == "a") {
            slot = "b";
        } else if (slot == "b") {
            slot = "a";
        } else {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::SlotMismatch,
                "secondary LP slot requires exactly the a/b topology"));
        }
    }
    auto candidate = task.partition + "_" + slot;
    if (!find_partition(metadata, candidate)) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::SlotMismatch,
            "LP metadata does not contain resolved partition " + candidate));
    }
    return candidate;
}

struct MemoryPiece final {
    std::uint64_t output_offset{};
    std::shared_ptr<const std::vector<std::byte>> bytes{};
    std::uint64_t bytes_offset{};
    std::shared_ptr<const image::IImageSource> source{};
    std::uint64_t source_offset{};
    std::uint64_t size{};
};

class OptimizedSuperSource final : public image::IImageSource {
public:
    OptimizedSuperSource(std::uint64_t size,
                         std::vector<MemoryPiece> pieces) noexcept
        : size_(size), pieces_(std::move(pieces)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] std::expected<std::size_t, image::ImageSourceError> read_at(
        const std::uint64_t offset,
        const std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset) {
            return std::unexpected(image::ImageSourceError{
                "optimized super source read exceeds encoded image"});
        }
        if (destination.empty()) {
            return 0U;
        }
        std::size_t completed = 0U;
        while (completed < destination.size()) {
            const auto cursor = offset + completed;
            const auto found = std::upper_bound(
                pieces_.begin(), pieces_.end(), cursor,
                [](const std::uint64_t value, const MemoryPiece& piece) {
                    return value < piece.output_offset;
                });
            if (found == pieces_.begin()) {
                return std::unexpected(image::ImageSourceError{
                    "optimized super piece table has a leading gap"});
            }
            const auto& piece = *std::prev(found);
            if (cursor < piece.output_offset ||
                cursor - piece.output_offset >= piece.size) {
                return std::unexpected(image::ImageSourceError{
                    "optimized super piece table has a gap"});
            }
            const auto in_piece = cursor - piece.output_offset;
            const auto amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(destination.size() - completed,
                                        piece.size - in_piece));
            if (piece.bytes) {
                std::copy_n(piece.bytes->data() + piece.bytes_offset + in_piece,
                            amount, destination.data() + completed);
            } else if (piece.source) {
                auto read = piece.source->read_at(
                    piece.source_offset + in_piece,
                    destination.subspan(completed, amount));
                if (!read) {
                    return std::unexpected(read.error());
                }
                if (*read == 0U || *read > amount) {
                    return std::unexpected(image::ImageSourceError{
                        "optimized super partition source was truncated"});
                }
                completed += *read;
                continue;
            } else {
                std::fill_n(destination.data() + completed, amount,
                            std::byte{0});
            }
            completed += amount;
        }
        return completed;
    }

private:
    std::uint64_t size_{};
    std::vector<MemoryPiece> pieces_{};
};

struct OutputRegion final {
    std::uint64_t offset{};
    std::uint64_t size{};
    std::shared_ptr<const std::vector<std::byte>> bytes{};
    std::shared_ptr<const image::IImageSource> source{};
    std::uint64_t source_size{};
};

void write_name(std::span<std::byte> output, const std::size_t offset,
                const std::string_view name) {
    std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(offset), 36U,
                std::byte{0});
    std::transform(name.begin(), name.end(),
                   output.begin() + static_cast<std::ptrdiff_t>(offset),
                   [](const char value) {
                       return static_cast<std::byte>(
                           static_cast<unsigned char>(value));
                   });
}

[[nodiscard]] std::shared_ptr<std::vector<std::byte>> serialize_metadata(
    const Metadata& metadata, const std::span<const SelectedImage> selected) {
    const auto tables_size =
        metadata.partitions.size() * kPartitionEntrySize +
        selected.size() * kExtentEntrySize +
        metadata.groups.size() * kGroupEntrySize + kBlockDeviceEntrySize;
    auto output = std::make_shared<std::vector<std::byte>>(
        metadata.header_size + tables_size, std::byte{0});
    auto bytes = std::span(*output);
    put_u32(bytes, 0U, kHeaderMagic);
    put_u16(bytes, 4U, 10U);
    put_u16(bytes, 6U, metadata.minor_version);
    put_u32(bytes, 8U, metadata.header_size);
    put_u32(bytes, 44U, static_cast<std::uint32_t>(tables_size));

    std::uint32_t table_offset = 0U;
    const auto write_descriptor = [&](const std::size_t descriptor_offset,
                                      const std::uint32_t count,
                                      const std::uint32_t entry_size) {
        put_u32(bytes, descriptor_offset, table_offset);
        put_u32(bytes, descriptor_offset + 4U, count);
        put_u32(bytes, descriptor_offset + 8U, entry_size);
        table_offset += count * entry_size;
    };
    write_descriptor(80U, static_cast<std::uint32_t>(metadata.partitions.size()),
                     kPartitionEntrySize);
    write_descriptor(92U, static_cast<std::uint32_t>(selected.size()),
                     kExtentEntrySize);
    write_descriptor(104U, static_cast<std::uint32_t>(metadata.groups.size()),
                     kGroupEntrySize);
    write_descriptor(116U, 1U, kBlockDeviceEntrySize);
    if (metadata.header_size == kHeaderV12Size) {
        put_u32(bytes, 128U, metadata.flags);
    }

    auto cursor = static_cast<std::size_t>(metadata.header_size);
    for (const auto& partition : metadata.partitions) {
        write_name(bytes, cursor, partition.name);
        put_u32(bytes, cursor + 36U, partition.attributes);
        put_u32(bytes, cursor + 40U, partition.first_extent);
        put_u32(bytes, cursor + 44U, partition.extent_count);
        put_u32(bytes, cursor + 48U, partition.group_index);
        cursor += kPartitionEntrySize;
    }
    for (const auto& image : selected) {
        put_u64(bytes, cursor, image.allocated_size / kSectorSize);
        put_u32(bytes, cursor + 8U, 0U);
        put_u64(bytes, cursor + 12U,
                image.physical_offset / kSectorSize);
        put_u32(bytes, cursor + 20U, 0U);
        cursor += kExtentEntrySize;
    }
    for (const auto& group : metadata.groups) {
        write_name(bytes, cursor, group.name);
        put_u32(bytes, cursor + 36U, group.flags);
        put_u64(bytes, cursor + 40U, group.maximum_size);
        cursor += kGroupEntrySize;
    }
    put_u64(bytes, cursor, metadata.block_device.first_logical_sector);
    put_u32(bytes, cursor + 8U, metadata.block_device.alignment);
    put_u32(bytes, cursor + 12U, metadata.block_device.alignment_offset);
    put_u64(bytes, cursor + 16U, metadata.block_device.size);
    write_name(bytes, cursor + 24U, metadata.block_device.name);
    put_u32(bytes, cursor + 60U, metadata.block_device.flags);

    const auto table_digest = digest_bytes(bytes.subspan(metadata.header_size));
    std::copy(table_digest.begin(), table_digest.end(), bytes.begin() + 48);
    std::array<std::byte, kHeaderV12Size> header_for_hash{};
    std::copy_n(bytes.begin(), metadata.header_size, header_for_hash.begin());
    std::fill(header_for_hash.begin() + 12, header_for_hash.begin() + 44,
              std::byte{0});
    const auto header_digest = digest_bytes(
        std::span(header_for_hash).first(metadata.header_size));
    std::copy(header_digest.begin(), header_digest.end(), bytes.begin() + 12);
    return output;
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    const auto start = bytes.size();
    bytes.resize(start + 2U);
    put_u16(bytes, start, value);
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    const auto start = bytes.size();
    bytes.resize(start + 4U);
    put_u32(bytes, start, value);
}

[[nodiscard]] std::expected<std::shared_ptr<const image::IImageSource>,
                            SuperOptimizationError>
build_sparse_source(const Metadata& metadata,
                    const std::span<const SelectedImage> selected,
                    const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        return std::unexpected(fail(SuperOptimizationErrorKind::Cancelled,
                                    "super optimization was cancelled"));
    }
    auto serialized = serialize_metadata(metadata, selected);
    if (serialized->size() > metadata.geometry.metadata_max_size) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::NoSpace,
            "optimized LP metadata exceeds metadata_max_size"));
    }
    if (metadata.geometry.metadata_max_size %
            metadata.geometry.logical_block_size !=
        0U) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::IncompatibleLayout,
            "LP metadata slots are not sparse-block aligned"));
    }
    auto metadata_block = std::make_shared<std::vector<std::byte>>(
        metadata.geometry.metadata_max_size, std::byte{0});
    std::copy(serialized->begin(), serialized->end(), metadata_block->begin());

    auto geometry = std::make_shared<const std::vector<std::byte>>(
        metadata.geometry.serialized);
    auto meta = std::shared_ptr<const std::vector<std::byte>>(metadata_block);
    std::vector<OutputRegion> regions;
    regions.push_back(OutputRegion{.offset = kPrimaryGeometryOffset,
                                   .size = kGeometryRegionSize,
                                   .bytes = geometry});
    regions.push_back(OutputRegion{.offset = kBackupGeometryOffset,
                                   .size = kGeometryRegionSize,
                                   .bytes = geometry});
    const auto primary_start = kBackupGeometryOffset + kGeometryRegionSize;
    const auto primary_bytes =
        static_cast<std::uint64_t>(metadata.geometry.metadata_max_size) *
        metadata.geometry.metadata_slot_count;
    const auto backup_start = primary_start + primary_bytes;
    for (std::uint32_t slot = 0U; slot < metadata.geometry.metadata_slot_count;
         ++slot) {
        for (const auto base : {primary_start, backup_start}) {
            regions.push_back(OutputRegion{
                .offset = base + static_cast<std::uint64_t>(slot) *
                                     metadata.geometry.metadata_max_size,
                .size = metadata.geometry.metadata_max_size,
                .bytes = meta,
            });
        }
    }
    for (const auto& image : selected) {
        regions.push_back(OutputRegion{
            .offset = image.physical_offset,
            .size = image.allocated_size,
            .source = image.source,
            .source_size = image.size,
        });
    }
    std::ranges::sort(regions, {}, &OutputRegion::offset);
    std::uint64_t populated_end = 0U;
    for (const auto& region : regions) {
        if (region.offset < populated_end ||
            region.offset % metadata.geometry.logical_block_size != 0U ||
            region.size % metadata.geometry.logical_block_size != 0U ||
            add_overflows(region.offset, region.size)) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::InvalidMetadata,
                "optimized super regions overlap or are not block aligned"));
        }
        populated_end = region.offset + region.size;
    }
    const auto final_offset = metadata.block_device.size;
    if (populated_end > final_offset || final_offset == 0U ||
        final_offset % metadata.geometry.logical_block_size != 0U ||
        final_offset / metadata.geometry.logical_block_size > UINT32_MAX) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::NoSpace,
            "optimized super sparse output exceeds format limits"));
    }

    auto header = std::make_shared<std::vector<std::byte>>();
    header->reserve(28U);
    append_u32(*header, kSparseMagic);
    append_u16(*header, image::kAndroidSparseMajorVersion);
    append_u16(*header, 0U);
    append_u16(*header, 28U);
    append_u16(*header, 12U);
    append_u32(*header, metadata.geometry.logical_block_size);
    append_u32(*header, static_cast<std::uint32_t>(
                           final_offset / metadata.geometry.logical_block_size));
    append_u32(*header, 0U);  // Patched after chunk generation.
    append_u32(*header, 0U);

    std::vector<MemoryPiece> pieces;
    pieces.push_back(MemoryPiece{.output_offset = 0U,
                                 .bytes = header,
                                 .size = header->size()});
    std::uint64_t encoded_offset = header->size();
    std::uint64_t output_cursor = 0U;
    std::uint32_t chunks = 0U;
    const auto append_chunk_header = [&](const std::uint16_t kind,
                                         const std::uint32_t blocks,
                                         const std::uint32_t bytes) {
        auto chunk = std::make_shared<std::vector<std::byte>>();
        chunk->reserve(12U);
        append_u16(*chunk, kind);
        append_u16(*chunk, 0U);
        append_u32(*chunk, blocks);
        append_u32(*chunk, bytes);
        pieces.push_back(MemoryPiece{.output_offset = encoded_offset,
                                     .bytes = std::move(chunk),
                                     .size = 12U});
        encoded_offset += 12U;
        ++chunks;
    };
    for (const auto& region : regions) {
        if (region.offset > output_cursor) {
            const auto blocks = static_cast<std::uint32_t>(
                (region.offset - output_cursor) /
                metadata.geometry.logical_block_size);
            append_chunk_header(image::kSparseChunkDontCare, blocks, 12U);
            output_cursor = region.offset;
        }
        std::uint64_t region_cursor = 0U;
        const auto maximum_raw =
            (static_cast<std::uint64_t>(UINT32_MAX) - 12U) /
            metadata.geometry.logical_block_size *
            metadata.geometry.logical_block_size;
        while (region_cursor < region.size) {
            const auto amount = std::min(region.size - region_cursor, maximum_raw);
            const auto blocks = static_cast<std::uint32_t>(
                amount / metadata.geometry.logical_block_size);
            append_chunk_header(image::kSparseChunkRaw, blocks,
                                static_cast<std::uint32_t>(12U + amount));
            if (region.bytes) {
                pieces.push_back(MemoryPiece{
                    .output_offset = encoded_offset,
                    .bytes = region.bytes,
                    .bytes_offset = region_cursor,
                    .size = amount,
                });
            } else {
                const auto source_remaining =
                    region_cursor < region.source_size
                        ? region.source_size - region_cursor
                        : 0U;
                const auto source_amount = std::min(amount, source_remaining);
                if (source_amount != 0U) {
                    pieces.push_back(MemoryPiece{
                        .output_offset = encoded_offset,
                        .source = region.source,
                        .source_offset = region_cursor,
                        .size = source_amount,
                    });
                }
                if (source_amount < amount) {
                    pieces.push_back(MemoryPiece{
                        .output_offset = encoded_offset + source_amount,
                        .size = amount - source_amount,
                    });
                }
            }
            encoded_offset += amount;
            region_cursor += amount;
            output_cursor += amount;
        }
    }
    if (output_cursor < final_offset) {
        const auto blocks = static_cast<std::uint32_t>(
            (final_offset - output_cursor) /
            metadata.geometry.logical_block_size);
        append_chunk_header(image::kSparseChunkDontCare, blocks, 12U);
        output_cursor = final_offset;
    }
    if (output_cursor != final_offset) {
        return std::unexpected(fail(
            SuperOptimizationErrorKind::InvalidMetadata,
            "optimized super sparse layout does not cover the block device"));
    }
    put_u32(*header, 20U, chunks);
    std::ranges::sort(pieces, {}, &MemoryPiece::output_offset);
    return std::shared_ptr<const image::IImageSource>{
        std::make_shared<OptimizedSuperSource>(encoded_offset, std::move(pieces))};
}

[[nodiscard]] const PreparedUpdateArtifact* find_artifact(
    const PreparedUpdatePackage& prepared, const std::string_view name) noexcept {
    const auto found = std::ranges::find(prepared.artifacts, name,
                                         &PreparedUpdateArtifact::name);
    return found == prepared.artifacts.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<std::size_t> find_optimization_window(
    const PreparedUpdatePackage& prepared) noexcept {
    for (std::size_t index = 0U; index + 2U < prepared.plan.tasks.size(); ++index) {
        const auto& reboot = prepared.plan.tasks[index];
        if (reboot.kind == UpdateTaskKind::Reboot &&
            reboot.reboot_target == PlannedRebootTarget::Fastboot &&
            prepared.plan.tasks[index + 1U].kind == UpdateTaskKind::UpdateSuper &&
            prepared.plan.tasks[index + 2U].kind == UpdateTaskKind::Flash) {
            return index;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string unique_optimized_name(
    const PreparedUpdatePackage& prepared) {
    const auto used = [&prepared](const std::string_view candidate) {
        return std::ranges::any_of(
                   prepared.artifacts,
                   [candidate](const PreparedUpdateArtifact& artifact) {
                       return artifact.name == candidate;
                   }) ||
               std::ranges::any_of(
                   prepared.plan.tasks,
                   [candidate](const PlannedUpdateTask& task) {
                       return task.artifact == candidate;
                   });
    };
    if (!used(kOptimizedName)) {
        return std::string(kOptimizedName);
    }
    for (std::size_t suffix = 1U;; ++suffix) {
        auto candidate = std::string{"kairosboot-optimized-super-"} +
                         std::to_string(suffix) + ".img";
        if (!used(candidate)) {
            return candidate;
        }
    }
}

}  // namespace

bool has_super_optimization_candidate(
    const PreparedUpdatePackage& prepared) noexcept {
    return find_optimization_window(prepared).has_value();
}

std::expected<SuperOptimizationReport, SuperOptimizationError>
optimize_prepared_super(PreparedUpdatePackage& prepared,
                        const SuperOptimizationDeviceInfo& device,
                        const std::stop_token cancellation) {
    try {
        const auto window = find_optimization_window(prepared);
        if (!window) {
            return SuperOptimizationReport{};
        }
        if (cancellation.stop_requested()) {
            return std::unexpected(fail(SuperOptimizationErrorKind::Cancelled,
                                        "super optimization was cancelled"));
        }
        if (!prepared.prepared_super_artifact ||
            prepared.update_super_state != UpdateSuperPreparationState::Prepared ||
            device.super_partition.empty()) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::InvalidMetadata,
                "super optimization candidate lacks immutable super/device metadata"));
        }
        const ExpandedSuperReader reader(
            *prepared.prepared_super_artifact->artifact());
        auto geometry = parse_geometry(reader, cancellation);
        if (!geometry) {
            return std::unexpected(std::move(geometry.error()));
        }
        auto metadata = parse_metadata(reader, std::move(*geometry), cancellation);
        if (!metadata) {
            if (metadata.error().kind ==
                SuperOptimizationErrorKind::IncompatibleLayout) {
                return SuperOptimizationReport{};
            }
            return std::unexpected(std::move(metadata.error()));
        }
        if (device.super_partition_size != 0U &&
            metadata->block_device.size != device.super_partition_size) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::SizeMismatch,
                "device super size does not match super_empty.img LP metadata"));
        }
        if (metadata->block_device.name != device.super_partition) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::SizeMismatch,
                "device super name does not match super_empty.img LP metadata"));
        }
        if (device.super_partition_size == 0U || device.current_slot.empty()) {
            // The frozen path remains available when optional device getvars
            // are unsupported, but malformed host LP metadata is rejected
            // above before that fallback can execute.
            return SuperOptimizationReport{};
        }

        std::vector<SelectedImage> selected;
        std::set<std::string, std::less<>> selected_names;
        for (const auto index : {*window + 2U}) {
            const auto& task = prepared.plan.tasks[index];
            const auto exact_partition = find_partition(*metadata, task.partition);
            const auto slotted_prefix = task.partition + "_";
            const auto has_slotted_partition = std::ranges::any_of(
                metadata->partitions, [&slotted_prefix](const Partition& partition) {
                    return partition.name.starts_with(slotted_prefix);
                });
            if (!exact_partition && !has_slotted_partition) {
                return SuperOptimizationReport{};
            }
            auto resolved_name = resolve_partition_name(
                *metadata, task, device.current_slot);
            if (!resolved_name) {
                return std::unexpected(std::move(resolved_name.error()));
            }
            if (!selected_names.insert(*resolved_name).second) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::SlotMismatch,
                    "multiple flash tasks resolve to LP partition " +
                        *resolved_name));
            }
            const auto* artifact = find_artifact(prepared, task.artifact);
            if (!artifact || !artifact->artifact || !artifact->resolved ||
                !artifact->resolved->source ||
                artifact->artifact->metadata().kind !=
                    image::FlashArtifactKind::Raw ||
                artifact->artifact->metadata().expanded_size !=
                    artifact->resolved->source->size()) {
                // Frozen AOSP falls back for sparse logical images.
                return SuperOptimizationReport{};
            }
            selected.push_back(SelectedImage{
                .task_index = index,
                .partition_index = *find_partition(*metadata, *resolved_name),
                .partition_name = std::move(*resolved_name),
                .source = artifact->resolved->source,
                .size = artifact->resolved->source->size(),
            });
        }
        if (selected.empty()) {
            return SuperOptimizationReport{};
        }

        std::vector<std::uint64_t> group_sizes(metadata->groups.size(), 0U);
        auto allocation = align_up(
            metadata->block_device.first_logical_sector * kSectorSize,
            std::max<std::uint64_t>(metadata->block_device.alignment,
                                    metadata->geometry.logical_block_size),
            metadata->block_device.alignment == 0U
                ? 0U
                : metadata->block_device.alignment_offset);
        if (!allocation) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::IncompatibleLayout,
                "LP allocation alignment is invalid"));
        }
        for (std::size_t index = 0U; index < selected.size(); ++index) {
            auto size = align_up(selected[index].size,
                                 metadata->geometry.logical_block_size);
            if (!size || add_overflows(*allocation, *size) ||
                *allocation + *size > metadata->block_device.size) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::NoSpace,
                    "logical partition images do not fit in super"));
            }
            const auto group =
                metadata->partitions[selected[index].partition_index].group_index;
            if (add_overflows(group_sizes[group], *size)) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::NoSpace,
                    "LP partition group size overflows"));
            }
            group_sizes[group] += *size;
            if (metadata->groups[group].maximum_size != 0U &&
                group_sizes[group] > metadata->groups[group].maximum_size) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::NoSpace,
                    "logical partition images exceed their LP group limit"));
            }
            selected[index].allocated_size = *size;
            selected[index].physical_offset = *allocation;
            auto& partition =
                metadata->partitions[selected[index].partition_index];
            partition.first_extent = static_cast<std::uint32_t>(index);
            partition.extent_count = 1U;
            *allocation += *size;
            auto next = align_up(
                *allocation,
                std::max<std::uint64_t>(metadata->block_device.alignment,
                                        metadata->geometry.logical_block_size),
                metadata->block_device.alignment == 0U
                    ? 0U
                    : metadata->block_device.alignment_offset);
            if (!next) {
                return std::unexpected(fail(
                    SuperOptimizationErrorKind::NoSpace,
                    "LP allocation offset overflows"));
            }
            allocation = next;
        }
        // The extent table has exactly one record per selected partition. The
        // serializer uses this count even though untouched partitions stay at
        // zero extents.
        auto source = build_sparse_source(*metadata, selected, cancellation);
        if (!source) {
            return std::unexpected(std::move(source.error()));
        }
        auto inspected = image::FlashArtifact::inspect(*source, cancellation);
        if (!inspected) {
            return std::unexpected(fail(
                SuperOptimizationErrorKind::Source,
                "generated optimized super sparse image failed validation: " +
                    inspected.error().message));
        }
        auto digest = image::compute_sha256(**source, cancellation);
        if (!digest) {
            return std::unexpected(fail(
                digest.error().kind == image::Sha256ErrorKind::Cancelled
                    ? SuperOptimizationErrorKind::Cancelled
                    : SuperOptimizationErrorKind::Source,
                "unable to hash optimized super image: " +
                    digest.error().message));
        }
        auto resolved = std::make_shared<const image::ResolvedArtifact>(
            image::ResolvedArtifact{
                .source = *source,
                .sha256 = *digest,
                .origin = image::ArtifactSourceOrigin::DirectFile,
                .logical_name = unique_optimized_name(prepared),
            });
        auto artifact = std::make_shared<const image::FlashArtifact>(
            std::move(*inspected));

        SuperOptimizationReport report{.optimized = true};
        for (const auto& image : selected) {
            report.absorbed_partitions.push_back(image.partition_name);
        }
        const auto optimized_name = resolved->logical_name;
        const auto absorbed_artifact =
            prepared.plan.tasks[*window + 2U].artifact;
        std::vector<PlannedUpdateTask> tasks;
        tasks.reserve(prepared.plan.tasks.size() - 2U);
        tasks.insert(tasks.end(), prepared.plan.tasks.begin(),
                     prepared.plan.tasks.begin() +
                         static_cast<std::ptrdiff_t>(*window));
        tasks.push_back(PlannedUpdateTask{
            .kind = UpdateTaskKind::Flash,
            .location = prepared.plan.tasks[*window].location,
            .partition = device.super_partition,
            .artifact = optimized_name,
            .slot = PlannedSlot::Default,
        });
        tasks.insert(tasks.end(),
                     prepared.plan.tasks.begin() +
                         static_cast<std::ptrdiff_t>(*window + 3U),
                     prepared.plan.tasks.end());
        prepared.plan.tasks = std::move(tasks);
        std::erase_if(prepared.artifacts,
                      [&prepared, &absorbed_artifact](
                          const PreparedUpdateArtifact& value) {
                          return value.name == absorbed_artifact &&
                                 std::ranges::none_of(
                                     prepared.plan.tasks,
                                     [&value](const PlannedUpdateTask& task) {
                                         return task.artifact == value.name;
                                     });
                      });
        prepared.artifacts.push_back(PreparedUpdateArtifact{
            .name = optimized_name,
            .resolved = std::move(resolved),
            .artifact = std::move(artifact),
        });
        const auto has_remaining_update_super = std::ranges::any_of(
            prepared.plan.tasks, [](const PlannedUpdateTask& task) {
                return task.kind == UpdateTaskKind::UpdateSuper;
            });
        if (!has_remaining_update_super) {
            prepared.update_super_state = UpdateSuperPreparationState::NotRequired;
            prepared.prepared_super_artifact.reset();
        }
        return report;
    } catch (const std::bad_alloc&) {
        return std::unexpected(fail(SuperOptimizationErrorKind::NoSpace,
                                    "memory allocation failed during super optimization"));
    }
}

}  // namespace kairosboot::fastboot
