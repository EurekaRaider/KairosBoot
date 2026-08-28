// SPDX-License-Identifier: MIT
#include "boot_image_builder.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kairosboot::image {
namespace {

inline constexpr std::size_t kReadBufferSize = 64U * 1024U;

[[nodiscard]] BootImageBuildError error(
    const BootImageBuildErrorKind kind,
    std::string message) {
    return {.kind = kind, .message = std::move(message)};
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

[[nodiscard]] bool supported_page_size(const std::uint32_t value) noexcept {
    return value == 2048U || value == 4096U || value == 8192U ||
           value == 16384U;
}

[[nodiscard]] std::expected<std::uint64_t, BootImageBuildError> align_up(
    const std::uint64_t value,
    const std::uint32_t alignment) {
    const auto mask = static_cast<std::uint64_t>(alignment - 1U);
    std::uint64_t adjusted = 0U;
    if (!checked_add(value, mask, adjusted)) {
        return std::unexpected(error(
            BootImageBuildErrorKind::SizeOverflow,
            "boot image alignment overflows 64-bit size"));
    }
    return adjusted & ~mask;
}

void write_u32(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void write_u64(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] bool parse_decimal(
    const std::string_view text,
    std::uint32_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto [end, parse_error] = std::from_chars(
        text.data(), text.data() + text.size(), value, 10);
    return parse_error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] std::expected<std::uint32_t, BootImageBuildError>
encode_os_version(const std::string_view value) {
    if (value.empty()) {
        return 0U;
    }
    std::array<std::uint32_t, 3> parts{};
    std::size_t count = 0U;
    std::size_t cursor = 0U;
    while (cursor <= value.size() && count < parts.size()) {
        const auto separator = value.find('.', cursor);
        const auto end = separator == std::string_view::npos
                             ? value.size()
                             : separator;
        if (!parse_decimal(value.substr(cursor, end - cursor), parts[count]) ||
            parts[count] > 0x7fU) {
            return std::unexpected(error(
                BootImageBuildErrorKind::InvalidArgument,
                "boot OS version must be MAJOR[.MINOR[.PATCH]] with each "
                "component in [0, 127]"));
        }
        ++count;
        if (separator == std::string_view::npos) {
            cursor = value.size() + 1U;
            break;
        }
        cursor = separator + 1U;
    }
    if (cursor <= value.size() || count == 0U) {
        return std::unexpected(error(
            BootImageBuildErrorKind::InvalidArgument,
            "boot OS version must be MAJOR[.MINOR[.PATCH]] with each "
            "component in [0, 127]"));
    }
    return (parts[0] << 25U) | (parts[1] << 18U) | (parts[2] << 11U);
}

[[nodiscard]] bool leap_year(const std::uint32_t year) noexcept {
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

[[nodiscard]] std::expected<std::uint32_t, BootImageBuildError>
encode_patch_level(const std::string_view value) {
    if (value.empty()) {
        return 0U;
    }
    if (value.size() != 10U || value[4] != '-' || value[7] != '-') {
        return std::unexpected(error(
            BootImageBuildErrorKind::InvalidArgument,
            "boot OS patch level must be YYYY-MM-DD"));
    }
    std::uint32_t year = 0U;
    std::uint32_t month = 0U;
    std::uint32_t day = 0U;
    if (!parse_decimal(value.substr(0U, 4U), year) ||
        !parse_decimal(value.substr(5U, 2U), month) ||
        !parse_decimal(value.substr(8U, 2U), day) || year < 2000U ||
        year >= 2128U || month == 0U || month > 12U) {
        return std::unexpected(error(
            BootImageBuildErrorKind::InvalidArgument,
            "boot OS patch level is outside the encodable range"));
    }
    constexpr std::array<std::uint32_t, 12> days_per_month{
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U};
    const auto maximum_day =
        month == 2U && leap_year(year) ? 29U : days_per_month[month - 1U];
    if (day == 0U || day > maximum_day) {
        return std::unexpected(error(
            BootImageBuildErrorKind::InvalidArgument,
            "boot OS patch level is not a valid calendar date"));
    }
    // Android boot headers encode year and month; the day is validated but is
    // intentionally not representable in the wire field.
    return ((year - 2000U) << 4U) | month;
}

class Sha1 final {
public:
    void update(const std::span<const std::byte> bytes) noexcept {
        std::size_t consumed = 0U;
        while (consumed < bytes.size()) {
            const auto amount =
                std::min(bytes.size() - consumed, block_.size() - buffered_);
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(consumed),
                        amount,
                        block_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            consumed += amount;
            buffered_ += amount;
            total_bytes_ += amount;
            if (buffered_ == block_.size()) {
                transform(block_);
                buffered_ = 0U;
            }
        }
    }

    [[nodiscard]] std::array<std::byte, 20> finish() noexcept {
        const auto bit_count = total_bytes_ * 8U;
        const std::array marker{std::byte{0x80}};
        update(marker);
        const std::array<std::byte, 64> zeros{};
        const auto zero_count = buffered_ <= 56U ? 56U - buffered_
                                                 : 120U - buffered_;
        update(std::span(zeros).first(zero_count));
        std::array<std::byte, 8> length{};
        for (std::size_t index = 0U; index < length.size(); ++index) {
            length[length.size() - 1U - index] =
                static_cast<std::byte>((bit_count >> (index * 8U)) & 0xffU);
        }
        update(length);

        std::array<std::byte, 20> digest{};
        for (std::size_t word = 0U; word < state_.size(); ++word) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                digest[word * 4U + byte] = static_cast<std::byte>(
                    (state_[word] >> ((3U - byte) * 8U)) & 0xffU);
            }
        }
        return digest;
    }

private:
    void transform(const std::span<const std::byte, 64> block) noexcept {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            for (std::size_t byte = 0U; byte < 4U; ++byte) {
                words[index] =
                    (words[index] << 8U) |
                    std::to_integer<std::uint8_t>(block[index * 4U + byte]);
            }
        }
        for (std::size_t index = 16U; index < words.size(); ++index) {
            words[index] = std::rotl(
                words[index - 3U] ^ words[index - 8U] ^
                    words[index - 14U] ^ words[index - 16U],
                1);
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        for (std::size_t index = 0U; index < words.size(); ++index) {
            std::uint32_t function = 0U;
            std::uint32_t constant = 0U;
            if (index < 20U) {
                function = (b & c) | ((~b) & d);
                constant = 0x5a827999U;
            } else if (index < 40U) {
                function = b ^ c ^ d;
                constant = 0x6ed9eba1U;
            } else if (index < 60U) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8f1bbcdcU;
            } else {
                function = b ^ c ^ d;
                constant = 0xca62c1d6U;
            }
            const auto temporary =
                std::rotl(a, 5) + function + e + constant + words[index];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temporary;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    std::array<std::uint32_t, 5> state_{
        0x67452301U,
        0xefcdab89U,
        0x98badcfeU,
        0x10325476U,
        0xc3d2e1f0U,
    };
    std::array<std::byte, 64> block_{};
    std::uint64_t total_bytes_{};
    std::size_t buffered_{};
};

[[nodiscard]] std::expected<void, BootImageBuildError> hash_payload(
    Sha1& hash,
    const std::shared_ptr<const IImageSource>& source,
    const std::string_view name,
    const std::stop_token cancellation) {
    const auto size = source == nullptr ? 0U : source->size();
    std::array<std::byte, kReadBufferSize> buffer{};
    std::uint64_t completed = 0U;
    while (completed < size) {
        if (cancellation.stop_requested()) {
            return std::unexpected(error(
                BootImageBuildErrorKind::Cancelled,
                "boot image construction was cancelled"));
        }
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - completed));
        auto read = source->read_at(completed, std::span(buffer).first(requested));
        if (!read) {
            return std::unexpected(error(
                BootImageBuildErrorKind::Source,
                std::string{name} + " read failed: " + read.error().message));
        }
        if (*read == 0U || *read > requested) {
            return std::unexpected(error(
                *read == 0U ? BootImageBuildErrorKind::Truncated
                            : BootImageBuildErrorKind::Source,
                std::string{name} + " violated its declared size"));
        }
        hash.update(std::span(buffer).first(*read));
        completed += *read;
    }
    std::array<std::byte, 4> encoded_size{};
    write_u32(encoded_size, 0U, static_cast<std::uint32_t>(size));
    hash.update(encoded_size);
    return {};
}

}  // namespace

struct LegacyBootImageSource::Impl final {
    struct Piece final {
        std::uint64_t output_offset{};
        std::uint64_t size{};
        std::uint64_t source_offset{};
        std::shared_ptr<const IImageSource> source;
        std::shared_ptr<const std::vector<std::byte>> bytes;
        bool zero{};
    };

    std::uint64_t size{};
    std::vector<Piece> pieces;
};

LegacyBootImageSource::LegacyBootImageSource(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LegacyBootImageSource::~LegacyBootImageSource() = default;

std::uint64_t LegacyBootImageSource::size() const noexcept {
    return impl_->size;
}

std::expected<std::size_t, ImageSourceError> LegacyBootImageSource::read_at(
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
        const auto found = std::find_if(
            impl_->pieces.begin(),
            impl_->pieces.end(),
            [current](const Impl::Piece& piece) {
                return current >= piece.output_offset &&
                       current - piece.output_offset < piece.size;
            });
        if (found == impl_->pieces.end()) {
            return std::unexpected(
                ImageSourceError{"boot image piece table has a gap"});
        }
        const auto within = current - found->output_offset;
        const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
            requested - completed, found->size - within));
        auto output = destination.subspan(completed, amount);
        if (found->zero) {
            std::fill(output.begin(), output.end(), std::byte{0});
        } else if (found->bytes != nullptr) {
            std::copy_n(found->bytes->begin() +
                            static_cast<std::ptrdiff_t>(within),
                        amount,
                        output.begin());
        } else {
            auto read = found->source->read_at(found->source_offset + within, output);
            if (!read) {
                return std::unexpected(std::move(read.error()));
            }
            if (*read == 0U || *read > amount) {
                return std::unexpected(
                    ImageSourceError{"boot image payload violated its declared size"});
            }
            completed += *read;
            continue;
        }
        completed += amount;
    }
    return completed;
}

std::expected<std::shared_ptr<const LegacyBootImageSource>, BootImageBuildError>
build_legacy_boot_image(
    std::shared_ptr<const IImageSource> kernel,
    std::shared_ptr<const IImageSource> ramdisk,
    std::shared_ptr<const IImageSource> second,
    LegacyBootImageOptions options,
    const std::stop_token cancellation) {
    return build_boot_image(std::move(kernel), std::move(ramdisk),
                            std::move(second), {}, std::move(options),
                            cancellation);
}

std::expected<std::shared_ptr<const LegacyBootImageSource>, BootImageBuildError>
build_boot_image(
    std::shared_ptr<const IImageSource> kernel,
    std::shared_ptr<const IImageSource> ramdisk,
    std::shared_ptr<const IImageSource> second,
    std::shared_ptr<const IImageSource> dtb,
    LegacyBootImageOptions options,
    const std::stop_token cancellation) {
    try {
        if (kernel == nullptr || kernel->size() == 0U) {
            return std::unexpected(error(
                BootImageBuildErrorKind::InvalidArgument,
                "boot image requires a non-empty kernel"));
        }
        if (options.header_version > 4U) {
            return std::unexpected(error(
                BootImageBuildErrorKind::InvalidArgument,
                "boot header version must be in [0, 4]"));
        }
        const bool v3_or_later = options.header_version >= 3U;
        const std::uint32_t page_size = v3_or_later ? 4096U : options.page_size;
        if (!v3_or_later && !supported_page_size(page_size)) {
            return std::unexpected(error(
                BootImageBuildErrorKind::InvalidArgument,
                "boot page size must be 2048, 4096, 8192, or 16384"));
        }
        if (options.maximum_output_bytes < page_size) {
            return std::unexpected(error(
                BootImageBuildErrorKind::InvalidArgument,
                "boot image output limit is smaller than its header page"));
        }
        const std::size_t maximum_command_line = v3_or_later ? 1535U : 1534U;
        if (options.command_line.find('\0') != std::string::npos ||
            options.command_line.size() > maximum_command_line) {
            return std::unexpected(error(
                BootImageBuildErrorKind::InvalidArgument,
                "boot command line must be NUL-free and fit the selected "
                "header version"));
        }
        const auto encoded_version = encode_os_version(options.os_version);
        if (!encoded_version) {
            return std::unexpected(encoded_version.error());
        }
        const auto encoded_patch = encode_patch_level(options.os_patch_level);
        if (!encoded_patch) {
            return std::unexpected(encoded_patch.error());
        }
        const std::uint32_t os_version = *encoded_version | *encoded_patch;

        constexpr LegacyBootImageOptions defaults{};
        if (v3_or_later) {
            if (second != nullptr) {
                return std::unexpected(error(
                    BootImageBuildErrorKind::InvalidArgument,
                    "boot header v3/v4 does not support a second-stage payload"));
            }
            if (dtb != nullptr || !options.dtb_path.empty()) {
                return std::unexpected(error(
                    BootImageBuildErrorKind::InvalidArgument,
                    "boot header v3/v4 does not support a DTB payload"));
            }
            if (options.base != defaults.base ||
                options.page_size != defaults.page_size ||
                options.kernel_offset != defaults.kernel_offset ||
                options.ramdisk_offset != defaults.ramdisk_offset ||
                options.second_offset != defaults.second_offset ||
                options.tags_offset != defaults.tags_offset ||
                options.dtb_offset != defaults.dtb_offset) {
                return std::unexpected(error(
                    BootImageBuildErrorKind::InvalidArgument,
                    "legacy address, page-size, and DTB-offset options are "
                    "not applicable to boot header v3/v4"));
            }
        } else if (options.header_version != 2U) {
            if (dtb != nullptr || !options.dtb_path.empty()) {
                return std::unexpected(error(
                    BootImageBuildErrorKind::InvalidArgument,
                    "a DTB payload is supported only by boot header v2"));
            }
            if (options.dtb_offset != defaults.dtb_offset) {
                return std::unexpected(error(
                    BootImageBuildErrorKind::InvalidArgument,
                    "DTB offset is supported only by boot header v2"));
            }
        }
        if (dtb != nullptr && dtb->size() == 0U) {
            return std::unexpected(error(
                BootImageBuildErrorKind::InvalidArgument,
                "boot DTB payload must not be empty"));
        }

        const std::array components{kernel, ramdisk, second, dtb};
        for (const auto& component : components) {
            if (component != nullptr &&
                component->size() > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(error(
                    BootImageBuildErrorKind::SizeOverflow,
                    "boot component exceeds its 32-bit size field"));
            }
        }
        if (!v3_or_later) {
            const std::array offsets{
                options.kernel_offset,
                options.ramdisk_offset,
                options.second_offset,
                options.tags_offset,
            };
            for (const auto offset : offsets) {
                if (offset >
                    std::numeric_limits<std::uint32_t>::max() - options.base) {
                    return std::unexpected(error(
                        BootImageBuildErrorKind::SizeOverflow,
                        "boot base plus address offset exceeds UINT32_MAX"));
                }
            }
            if (options.header_version == 2U &&
                options.dtb_offset >
                    std::numeric_limits<std::uint64_t>::max() - options.base) {
                return std::unexpected(error(
                    BootImageBuildErrorKind::SizeOverflow,
                    "boot base plus DTB offset exceeds UINT64_MAX"));
            }
        }
        std::uint64_t planned_size = page_size;
        for (const auto& component : components) {
            if (component != nullptr &&
                !checked_add(planned_size, component->size(), planned_size)) {
                return std::unexpected(error(
                    BootImageBuildErrorKind::SizeOverflow,
                    "boot image size overflows 64 bits"));
            }
            auto aligned = align_up(planned_size, page_size);
            if (!aligned) {
                return std::unexpected(std::move(aligned.error()));
            }
            planned_size = *aligned;
        }
        if (planned_size > options.maximum_output_bytes) {
            return std::unexpected(error(
                BootImageBuildErrorKind::SizeOverflow,
                "legacy boot image exceeds the configured output limit"));
        }
        if (cancellation.stop_requested()) {
            return std::unexpected(error(
                BootImageBuildErrorKind::Cancelled,
                "boot image construction was cancelled"));
        }

        auto header = std::make_shared<std::vector<std::byte>>(
            page_size, std::byte{0});
        constexpr std::array magic{
            std::byte{'A'}, std::byte{'N'}, std::byte{'D'}, std::byte{'R'},
            std::byte{'O'}, std::byte{'I'}, std::byte{'D'}, std::byte{'!'},
        };
        std::copy(magic.begin(), magic.end(), header->begin());
        if (v3_or_later) {
            write_u32(*header, 8U, static_cast<std::uint32_t>(kernel->size()));
            write_u32(*header, 12U,
                      static_cast<std::uint32_t>(ramdisk == nullptr
                                                     ? 0U
                                                     : ramdisk->size()));
            write_u32(*header, 16U, os_version);
            write_u32(*header, 20U, 1580U);
            write_u32(*header, 40U, options.header_version);
            for (std::size_t index = 0U;
                 index < options.command_line.size(); ++index) {
                (*header)[44U + index] =
                    static_cast<std::byte>(options.command_line[index]);
            }
            // v4 appends signature_size at 1580. AOSP's host-side builder does
            // not synthesize a signature, so the zero-filled field is exact.
        } else {
            write_u32(*header, 8U, static_cast<std::uint32_t>(kernel->size()));
            write_u32(*header, 12U, options.base + options.kernel_offset);
            write_u32(*header, 16U,
                      static_cast<std::uint32_t>(ramdisk == nullptr
                                                     ? 0U
                                                     : ramdisk->size()));
            write_u32(*header, 20U, options.base + options.ramdisk_offset);
            write_u32(*header, 24U,
                      static_cast<std::uint32_t>(second == nullptr
                                                     ? 0U
                                                     : second->size()));
            write_u32(*header, 28U, options.base + options.second_offset);
            write_u32(*header, 32U, options.base + options.tags_offset);
            write_u32(*header, 36U, page_size);
            write_u32(*header, 40U, options.header_version);
            write_u32(*header, 44U, os_version);
            const auto first_length =
                std::min<std::size_t>(511U, options.command_line.size());
            for (std::size_t index = 0U; index < first_length; ++index) {
                (*header)[64U + index] =
                    static_cast<std::byte>(options.command_line[index]);
            }
            const auto extra_length =
                options.command_line.size() - first_length;
            for (std::size_t index = 0U; index < extra_length; ++index) {
                (*header)[608U + index] = static_cast<std::byte>(
                    options.command_line[first_length + index]);
            }
            if (options.header_version == 1U) {
                write_u32(*header, 1644U, 1648U);
            } else if (options.header_version == 2U) {
                write_u32(*header, 1644U, 1660U);
                write_u32(*header, 1648U,
                          static_cast<std::uint32_t>(dtb == nullptr
                                                         ? 0U
                                                         : dtb->size()));
                write_u64(*header, 1652U, options.base + options.dtb_offset);
            }

            Sha1 hash;
            if (auto result =
                    hash_payload(hash, kernel, "kernel", cancellation);
                !result) {
                return std::unexpected(std::move(result.error()));
            }
            if (auto result =
                    hash_payload(hash, ramdisk, "ramdisk", cancellation);
                !result) {
                return std::unexpected(std::move(result.error()));
            }
            if (auto result =
                    hash_payload(hash, second, "second", cancellation);
                !result) {
                return std::unexpected(std::move(result.error()));
            }
            if (options.header_version == 2U) {
                if (auto result =
                        hash_payload(hash, dtb, "dtb", cancellation);
                    !result) {
                    return std::unexpected(std::move(result.error()));
                }
            }
            const auto digest = hash.finish();
            std::copy(digest.begin(), digest.end(), header->begin() + 576);
        }

        auto impl = std::make_unique<LegacyBootImageSource::Impl>();
        impl->pieces.reserve(9U);
        impl->pieces.push_back({
            .output_offset = 0U,
            .size = page_size,
            .source_offset = 0U,
            .source = {},
            .bytes = std::move(header),
            .zero = false,
        });
        std::uint64_t cursor = page_size;
        for (const auto& component : components) {
            if (component != nullptr && component->size() != 0U) {
                impl->pieces.push_back({
                    .output_offset = cursor,
                    .size = component->size(),
                    .source_offset = 0U,
                    .source = component,
                    .bytes = {},
                    .zero = false,
                });
                if (!checked_add(cursor, component->size(), cursor)) {
                    return std::unexpected(error(
                        BootImageBuildErrorKind::SizeOverflow,
                        "boot image size overflows 64 bits"));
                }
            }
            auto aligned = align_up(cursor, page_size);
            if (!aligned) {
                return std::unexpected(std::move(aligned.error()));
            }
            if (*aligned != cursor) {
                impl->pieces.push_back({
                    .output_offset = cursor,
                    .size = *aligned - cursor,
                    .source_offset = 0U,
                    .source = {},
                    .bytes = {},
                    .zero = true,
                });
                cursor = *aligned;
            }
        }
        if (cursor != planned_size) {
            return std::unexpected(error(
                BootImageBuildErrorKind::SizeOverflow,
                "boot image plan changed during construction"));
        }
        impl->size = cursor;
        return std::shared_ptr<const LegacyBootImageSource>(
            new LegacyBootImageSource(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(
            BootImageBuildErrorKind::Allocation,
            "out of memory while constructing boot image"));
    }
}

}  // namespace kairosboot::image
