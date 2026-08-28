// SPDX-License-Identifier: MIT
#include "vendor_boot_repacker.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kairosboot::image {
namespace {

inline constexpr std::array<std::byte, 8> kVendorBootMagic{
    std::byte{'V'}, std::byte{'N'}, std::byte{'D'}, std::byte{'R'},
    std::byte{'B'}, std::byte{'O'}, std::byte{'O'}, std::byte{'T'}};
inline constexpr std::uint32_t kHeaderV3Size = 2112U;
inline constexpr std::uint32_t kHeaderV4Size = 2128U;
inline constexpr std::uint32_t kEntryV4Size = 108U;
inline constexpr std::uint32_t kMaximumPageSize = 64U * 1024U;
inline constexpr std::uint32_t kMaximumTableEntries = 65'536U;

inline constexpr std::size_t kHeaderVersionOffset = 8U;
inline constexpr std::size_t kPageSizeOffset = 12U;
inline constexpr std::size_t kRamdiskSizeOffset = 24U;
inline constexpr std::size_t kHeaderSizeOffset = 2096U;
inline constexpr std::size_t kDtbSizeOffset = 2100U;
inline constexpr std::size_t kTableSizeOffset = 2112U;
inline constexpr std::size_t kTableEntryCountOffset = 2116U;
inline constexpr std::size_t kTableEntrySizeOffset = 2120U;
inline constexpr std::size_t kBootconfigSizeOffset = 2124U;

[[nodiscard]] VendorBootRepackError make_error(
    const VendorBootRepackErrorKind kind,
    std::string message) {
    return {.kind = kind, .message = std::move(message)};
}

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_u32(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint32_t value) noexcept {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] std::expected<std::uint64_t, VendorBootRepackError> align_up(
    const std::uint64_t value,
    const std::uint32_t alignment) {
    std::uint64_t adjusted = 0U;
    if (!checked_add(value, alignment - 1U, adjusted)) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::SizeOverflow,
            "vendor_boot section alignment overflows 64-bit size"));
    }
    return adjusted / alignment * alignment;
}

[[nodiscard]] std::expected<void, VendorBootRepackError> read_exact(
    const IImageSource& source,
    const std::uint64_t offset,
    const std::span<std::byte> destination,
    const std::stop_token cancellation) {
    if (offset > source.size() || destination.size() > source.size() - offset) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Malformed,
            "vendor_boot section lies outside the fetched image"));
    }
    std::size_t completed = 0U;
    while (completed < destination.size()) {
        if (cancellation.stop_requested()) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Cancelled,
                "vendor_boot repack was cancelled"));
        }
        auto read = source.read_at(
            offset + completed, destination.subspan(completed));
        if (!read) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Source,
                "vendor_boot read failed: " + read.error().message));
        }
        if (*read == 0U || *read > destination.size() - completed) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Malformed,
                "vendor_boot source was truncated"));
        }
        completed += *read;
    }
    return {};
}

[[nodiscard]] bool valid_ramdisk_name(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 32U) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char character) {
        return character >= 0x21U && character <= 0x7eU && character != ':';
    });
}

struct ParsedHeader final {
    std::uint32_t version{};
    std::uint32_t header_size{};
    std::uint32_t page_size{};
    std::uint32_t ramdisk_size{};
    std::uint32_t dtb_size{};
    std::uint32_t table_size{};
    std::uint32_t table_entries{};
    std::uint32_t table_entry_size{};
    std::uint32_t bootconfig_size{};
    std::uint64_t header_end{};
    std::uint64_t ramdisk_end{};
    std::uint64_t dtb_end{};
    std::uint64_t table_end{};
    std::uint64_t bootconfig_end{};
};

[[nodiscard]] std::expected<ParsedHeader, VendorBootRepackError> parse_header(
    const std::shared_ptr<const IImageSource>& source,
    const std::uint64_t maximum_image_bytes,
    const std::stop_token cancellation) {
    if (source == nullptr || source->size() == 0U) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::InvalidArgument,
            "vendor_boot source must not be empty"));
    }
    if (source->size() > maximum_image_bytes ||
        source->size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::SizeOverflow,
            "vendor_boot image exceeds the configured 32-bit limit"));
    }
    if (source->size() < kHeaderV3Size) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Malformed,
            "vendor_boot image is smaller than a v3 header"));
    }

    std::array<std::byte, kHeaderV4Size> bytes{};
    const auto prefix_size = static_cast<std::size_t>(
        std::min<std::uint64_t>(bytes.size(), source->size()));
    if (auto read = read_exact(
            *source, 0U, std::span(bytes).first(prefix_size), cancellation);
        !read) {
        return std::unexpected(std::move(read.error()));
    }
    if (!std::ranges::equal(kVendorBootMagic,
                            std::span(bytes).first(kVendorBootMagic.size()))) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Malformed,
            "vendor_boot magic is not VNDRBOOT"));
    }

    ParsedHeader result{};
    result.version = read_u32(bytes, kHeaderVersionOffset);
    if (result.version != 3U && result.version != 4U) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Unsupported,
            "vendor_boot header version must be 3 or 4"));
    }
    result.header_size = read_u32(bytes, kHeaderSizeOffset);
    const auto expected_header_size =
        result.version == 3U ? kHeaderV3Size : kHeaderV4Size;
    if (result.header_size != expected_header_size) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Malformed,
            "vendor_boot header_size does not match its version"));
    }
    result.page_size = read_u32(bytes, kPageSizeOffset);
    if (result.page_size < 512U || result.page_size > kMaximumPageSize ||
        (result.page_size & (result.page_size - 1U)) != 0U) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Malformed,
            "vendor_boot page_size must be a power of two from 512 to 65536"));
    }
    result.ramdisk_size = read_u32(bytes, kRamdiskSizeOffset);
    result.dtb_size = read_u32(bytes, kDtbSizeOffset);
    if (result.ramdisk_size == 0U || result.dtb_size == 0U) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Malformed,
            "vendor_boot ramdisk and DTB sections must be non-empty"));
    }
    if (result.version == 4U) {
        result.table_size = read_u32(bytes, kTableSizeOffset);
        result.table_entries = read_u32(bytes, kTableEntryCountOffset);
        result.table_entry_size = read_u32(bytes, kTableEntrySizeOffset);
        result.bootconfig_size = read_u32(bytes, kBootconfigSizeOffset);
        if (result.table_entries == 0U ||
            result.table_entries > kMaximumTableEntries ||
            result.table_entry_size < kEntryV4Size) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Malformed,
                "vendor_boot v4 table dimensions are invalid"));
        }
        const auto required_table =
            static_cast<std::uint64_t>(result.table_entries) *
            result.table_entry_size;
        if (required_table > result.table_size) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Malformed,
                "vendor_boot v4 table entries exceed table_size"));
        }
    }

    auto header_end = align_up(expected_header_size, result.page_size);
    auto ramdisk_span = align_up(result.ramdisk_size, result.page_size);
    auto dtb_span = align_up(result.dtb_size, result.page_size);
    if (!header_end || !ramdisk_span || !dtb_span) {
        return std::unexpected(!header_end ? std::move(header_end.error())
                                           : !ramdisk_span
                                                 ? std::move(ramdisk_span.error())
                                                 : std::move(dtb_span.error()));
    }
    result.header_end = *header_end;
    if (!checked_add(result.header_end, *ramdisk_span, result.ramdisk_end) ||
        !checked_add(result.ramdisk_end, *dtb_span, result.dtb_end)) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::SizeOverflow,
            "vendor_boot base layout overflows 64-bit size"));
    }
    result.table_end = result.dtb_end;
    result.bootconfig_end = result.dtb_end;
    if (result.version == 4U) {
        auto table_span = align_up(result.table_size, result.page_size);
        auto bootconfig_span = align_up(result.bootconfig_size, result.page_size);
        if (!table_span || !bootconfig_span ||
            !checked_add(result.dtb_end, *table_span, result.table_end) ||
            !checked_add(result.table_end, *bootconfig_span,
                         result.bootconfig_end)) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::SizeOverflow,
                "vendor_boot v4 layout overflows 64-bit size"));
        }
    }
    if (result.bootconfig_end > source->size()) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Malformed,
            "vendor_boot declared sections exceed the fetched image"));
    }
    return result;
}

struct TableEntry final {
    std::uint32_t size{};
    std::uint32_t offset{};
    std::string name;
};

[[nodiscard]] std::expected<std::vector<TableEntry>, VendorBootRepackError>
parse_table(
    const std::shared_ptr<const IImageSource>& source,
    const ParsedHeader& header,
    const std::stop_token cancellation) {
    std::vector<TableEntry> entries;
    if (header.version != 4U) {
        return entries;
    }
    entries.reserve(header.table_entries);
    std::set<std::string> names;
    std::uint64_t expected_offset = 0U;
    const auto table_offset = header.dtb_end;
    std::vector<std::byte> bytes(header.table_entry_size);
    for (std::uint32_t index = 0U; index < header.table_entries; ++index) {
        if (auto read = read_exact(
                *source,
                table_offset +
                    static_cast<std::uint64_t>(index) * header.table_entry_size,
                bytes, cancellation);
            !read) {
            return std::unexpected(std::move(read.error()));
        }
        TableEntry entry{};
        entry.size = read_u32(bytes, 0U);
        entry.offset = read_u32(bytes, 4U);
        const auto name_bytes = std::span(bytes).subspan(12U, 32U);
        const auto nul = std::ranges::find(name_bytes, std::byte{0});
        entry.name.assign(
            reinterpret_cast<const char*>(name_bytes.data()),
            static_cast<std::size_t>(nul - name_bytes.begin()));
        if (!entry.name.empty() && !valid_ramdisk_name(entry.name)) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Malformed,
                "vendor_boot v4 table contains an invalid ramdisk name"));
        }
        if (!names.insert(entry.name).second) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Malformed,
                "vendor_boot v4 ramdisk names are not unique"));
        }
        if (entry.offset != expected_offset || entry.size == 0U ||
            !checked_add(expected_offset, entry.size, expected_offset) ||
            expected_offset > header.ramdisk_size) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Malformed,
                "vendor_boot v4 ramdisk entries are not one contiguous section"));
        }
        entries.push_back(std::move(entry));
    }
    if (expected_offset != header.ramdisk_size) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Malformed,
            "vendor_boot v4 table does not describe the complete ramdisk"));
    }
    return entries;
}

}  // namespace

struct VendorBootImageSource::Impl final {
    struct Piece final {
        std::uint64_t output_offset{};
        std::uint64_t size{};
        std::uint64_t source_offset{};
        std::shared_ptr<const IImageSource> source;
        std::shared_ptr<const std::vector<std::byte>> bytes;
        bool zero{};
    };

    std::uint64_t size{};
    VendorBootRepackMetadata metadata;
    std::vector<Piece> pieces;
};

VendorBootImageSource::VendorBootImageSource(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

VendorBootImageSource::~VendorBootImageSource() = default;

std::uint64_t VendorBootImageSource::size() const noexcept {
    return impl_->size;
}

const VendorBootRepackMetadata& VendorBootImageSource::metadata() const noexcept {
    return impl_->metadata;
}

std::expected<std::size_t, ImageSourceError> VendorBootImageSource::read_at(
    const std::uint64_t offset,
    const std::span<std::byte> destination) const {
    if (destination.empty() || offset >= impl_->size) {
        return std::size_t{0};
    }
    const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
        destination.size(), impl_->size - offset));
    std::size_t completed = 0U;
    while (completed < requested) {
        const auto current = offset + completed;
        const auto found = std::ranges::find_if(
            impl_->pieces, [current](const Impl::Piece& piece) {
                return current >= piece.output_offset &&
                       current - piece.output_offset < piece.size;
            });
        if (found == impl_->pieces.end()) {
            return std::unexpected(
                ImageSourceError{"vendor_boot repack piece table has a gap"});
        }
        const auto within = current - found->output_offset;
        const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
            requested - completed, found->size - within));
        auto output = destination.subspan(completed, amount);
        if (found->zero) {
            std::ranges::fill(output, std::byte{0});
        } else if (found->bytes != nullptr) {
            std::copy_n(found->bytes->begin() +
                            static_cast<std::ptrdiff_t>(within),
                        amount, output.begin());
        } else {
            auto read = found->source->read_at(
                found->source_offset + within, output);
            if (!read) {
                return std::unexpected(std::move(read.error()));
            }
            if (*read == 0U || *read > amount) {
                return std::unexpected(ImageSourceError{
                    "vendor_boot repack payload violated its declared size"});
            }
            completed += *read;
            continue;
        }
        completed += amount;
    }
    return completed;
}

std::expected<std::shared_ptr<const VendorBootImageSource>,
              VendorBootRepackError>
repack_vendor_boot(
    std::shared_ptr<const IImageSource> vendor_boot,
    std::shared_ptr<const IImageSource> new_ramdisk,
    std::shared_ptr<const IImageSource> new_dtb,
    VendorBootRepackOptions options,
    const std::stop_token cancellation) {
    try {
        if (new_ramdisk == nullptr || new_ramdisk->size() == 0U ||
            new_ramdisk->size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::InvalidArgument,
                "new vendor ramdisk must contain 1..UINT32_MAX bytes"));
        }
        if (new_dtb != nullptr &&
            (new_dtb->size() == 0U ||
             new_dtb->size() > std::numeric_limits<std::uint32_t>::max())) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::InvalidArgument,
                "replacement DTB must contain 1..UINT32_MAX bytes"));
        }
        if (!valid_ramdisk_name(options.ramdisk_name)) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::InvalidArgument,
                "vendor ramdisk name must be 1..32 printable ASCII bytes without ':'"));
        }
        if (cancellation.stop_requested()) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Cancelled,
                "vendor_boot repack was cancelled"));
        }

        auto header = parse_header(
            vendor_boot, options.maximum_image_bytes, cancellation);
        if (!header) {
            return std::unexpected(std::move(header.error()));
        }
        auto entries = parse_table(vendor_boot, *header, cancellation);
        if (!entries) {
            return std::unexpected(std::move(entries.error()));
        }

        const bool replace_default = options.ramdisk_name == "default";
        std::size_t replaced_index = 0U;
        if (!replace_default) {
            if (header->version != 4U) {
                return std::unexpected(make_error(
                    VendorBootRepackErrorKind::Unsupported,
                    "named vendor ramdisk replacement requires header v4"));
            }
            const auto found = std::ranges::find(
                *entries, options.ramdisk_name, &TableEntry::name);
            if (found == entries->end()) {
                return std::unexpected(make_error(
                    VendorBootRepackErrorKind::InvalidArgument,
                    "named vendor ramdisk was not found in the v4 table"));
            }
            replaced_index = static_cast<std::size_t>(found - entries->begin());
        }

        std::uint64_t new_ramdisk_size = new_ramdisk->size();
        if (!replace_default) {
            new_ramdisk_size = header->ramdisk_size -
                               (*entries)[replaced_index].size +
                               new_ramdisk->size();
        }
        if (new_ramdisk_size > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::SizeOverflow,
                "repacked vendor ramdisk exceeds UINT32_MAX"));
        }
        const auto new_dtb_size = static_cast<std::uint32_t>(
            new_dtb == nullptr ? header->dtb_size : new_dtb->size());
        const auto new_table_size =
            header->version == 4U
                ? replace_default ? kEntryV4Size : header->table_size
                : 0U;
        const auto new_table_entries =
            header->version == 4U
                ? replace_default ? 1U : header->table_entries
                : 0U;
        const auto new_table_entry_size =
            header->version == 4U
                ? replace_default ? kEntryV4Size : header->table_entry_size
                : 0U;

        auto new_ramdisk_span = align_up(new_ramdisk_size, header->page_size);
        auto new_dtb_span = align_up(new_dtb_size, header->page_size);
        auto new_table_span = align_up(new_table_size, header->page_size);
        auto new_bootconfig_span =
            align_up(header->bootconfig_size, header->page_size);
        if (!new_ramdisk_span || !new_dtb_span || !new_table_span ||
            !new_bootconfig_span) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::SizeOverflow,
                "repacked vendor_boot layout cannot be aligned"));
        }
        std::uint64_t new_known_end = header->header_end;
        if (!checked_add(new_known_end, *new_ramdisk_span, new_known_end) ||
            !checked_add(new_known_end, *new_dtb_span, new_known_end) ||
            !checked_add(new_known_end, *new_table_span, new_known_end) ||
            !checked_add(new_known_end, *new_bootconfig_span, new_known_end) ||
            new_known_end > vendor_boot->size()) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::SizeOverflow,
                "repacked vendor_boot sections do not fit the fetched partition image"));
        }

        auto impl = std::make_unique<VendorBootImageSource::Impl>();
        impl->size = vendor_boot->size();
        impl->metadata = {
            .header_version = header->version,
            .page_size = header->page_size,
            .vendor_ramdisk_size =
                static_cast<std::uint32_t>(new_ramdisk_size),
            .dtb_size = new_dtb_size,
            .table_entry_count = new_table_entries,
        };
        auto add_source = [&impl](
                              const std::uint64_t output,
                              const std::uint64_t size,
                              std::shared_ptr<const IImageSource> source,
                              const std::uint64_t source_offset) {
            if (size != 0U) {
                impl->pieces.push_back({
                    .output_offset = output,
                    .size = size,
                    .source_offset = source_offset,
                    .source = std::move(source),
                });
            }
        };
        auto add_bytes = [&impl](
                             const std::uint64_t output,
                             std::shared_ptr<const std::vector<std::byte>> bytes) {
            if (bytes != nullptr && !bytes->empty()) {
                impl->pieces.push_back({
                    .output_offset = output,
                    .size = bytes->size(),
                    .bytes = std::move(bytes),
                });
            }
        };
        auto add_zero = [&impl](const std::uint64_t output,
                                const std::uint64_t size) {
            if (size != 0U) {
                impl->pieces.push_back({
                    .output_offset = output,
                    .size = size,
                    .zero = true,
                });
            }
        };

        auto header_page =
            std::make_shared<std::vector<std::byte>>(header->header_end);
        if (auto read = read_exact(
                *vendor_boot, 0U, *header_page, cancellation);
            !read) {
            return std::unexpected(std::move(read.error()));
        }
        write_u32(*header_page, kRamdiskSizeOffset,
                  static_cast<std::uint32_t>(new_ramdisk_size));
        write_u32(*header_page, kDtbSizeOffset, new_dtb_size);
        if (header->version == 4U) {
            write_u32(*header_page, kTableSizeOffset, new_table_size);
            write_u32(*header_page, kTableEntryCountOffset, new_table_entries);
            write_u32(*header_page, kTableEntrySizeOffset,
                      new_table_entry_size);
        }
        add_bytes(0U, std::move(header_page));

        std::uint64_t output = header->header_end;
        if (replace_default) {
            add_source(output, new_ramdisk->size(), new_ramdisk, 0U);
            output += new_ramdisk->size();
        } else {
            for (std::size_t index = 0U; index < entries->size(); ++index) {
                const auto& entry = (*entries)[index];
                if (index == replaced_index) {
                    add_source(output, new_ramdisk->size(), new_ramdisk, 0U);
                    output += new_ramdisk->size();
                } else {
                    add_source(output, entry.size, vendor_boot,
                               header->header_end + entry.offset);
                    output += entry.size;
                }
            }
        }
        add_zero(output, *new_ramdisk_span - new_ramdisk_size);
        output = header->header_end + *new_ramdisk_span;

        if (new_dtb != nullptr) {
            add_source(output, new_dtb->size(), new_dtb, 0U);
        } else {
            add_source(output, header->dtb_size, vendor_boot,
                       header->ramdisk_end);
        }
        output += new_dtb_size;
        add_zero(output, *new_dtb_span - new_dtb_size);
        output = header->header_end + *new_ramdisk_span + *new_dtb_span;

        const auto old_table_offset = header->dtb_end;
        if (header->version == 4U) {
            if (replace_default) {
                auto entry =
                    std::make_shared<std::vector<std::byte>>(kEntryV4Size);
                write_u32(*entry, 0U,
                          static_cast<std::uint32_t>(new_ramdisk_size));
                add_bytes(output, std::move(entry));
                output += kEntryV4Size;
            } else {
                std::uint32_t ramdisk_offset = 0U;
                for (std::size_t index = 0U; index < entries->size(); ++index) {
                    auto prefix =
                        std::make_shared<std::vector<std::byte>>(8U);
                    const auto size = index == replaced_index
                                          ? static_cast<std::uint32_t>(
                                                new_ramdisk->size())
                                          : (*entries)[index].size;
                    write_u32(*prefix, 0U, size);
                    write_u32(*prefix, 4U, ramdisk_offset);
                    add_bytes(output, std::move(prefix));
                    add_source(output + 8U,
                               header->table_entry_size - 8U,
                               vendor_boot,
                               old_table_offset +
                                   static_cast<std::uint64_t>(index) *
                                       header->table_entry_size +
                                   8U);
                    output += header->table_entry_size;
                    ramdisk_offset += size;
                }
                const auto used = static_cast<std::uint64_t>(header->table_entries) *
                                  header->table_entry_size;
                add_source(output, header->table_size - used, vendor_boot,
                           old_table_offset + used);
                output += header->table_size - used;
            }
            add_zero(output, *new_table_span - new_table_size);
            output = header->header_end + *new_ramdisk_span + *new_dtb_span +
                     *new_table_span;

            const auto old_bootconfig_offset = header->table_end;
            add_source(output, header->bootconfig_size, vendor_boot,
                       old_bootconfig_offset);
            output += header->bootconfig_size;
            add_zero(output, *new_bootconfig_span - header->bootconfig_size);
            output = new_known_end;
        }

        // Preserve bytes outside the standardized layout at their absolute
        // partition offsets. When the new standardized layout shrinks, clear
        // the vacated old section pages before retaining the true tail. When
        // it grows, retain only tail bytes that were not consumed.
        const auto preserved_tail = std::max(output, header->bootconfig_end);
        add_zero(output, preserved_tail - output);
        if (preserved_tail < vendor_boot->size()) {
            add_source(preserved_tail, vendor_boot->size() - preserved_tail,
                       vendor_boot, preserved_tail);
        }
        std::ranges::sort(impl->pieces, {}, &VendorBootImageSource::Impl::Piece::output_offset);
        std::uint64_t expected = 0U;
        for (const auto& piece : impl->pieces) {
            if (piece.output_offset != expected ||
                !checked_add(expected, piece.size, expected)) {
                return std::unexpected(make_error(
                    VendorBootRepackErrorKind::Malformed,
                    "internal vendor_boot piece table is not contiguous"));
            }
        }
        if (expected != impl->size) {
            return std::unexpected(make_error(
                VendorBootRepackErrorKind::Malformed,
                "internal vendor_boot piece table has the wrong size"));
        }

        return std::shared_ptr<const VendorBootImageSource>(
            new VendorBootImageSource(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Allocation,
            "unable to allocate vendor_boot repack metadata"));
    } catch (...) {
        return std::unexpected(make_error(
            VendorBootRepackErrorKind::Source,
            "unexpected vendor_boot repack failure"));
    }
}

}  // namespace kairosboot::image
