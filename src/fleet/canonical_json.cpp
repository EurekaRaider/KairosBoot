// SPDX-License-Identifier: MIT
#include "src/fleet/canonical_json.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace kairosboot::fleet {
namespace {

[[nodiscard]] CanonicalJsonError invalid_utf8(
    const std::size_t offset) noexcept {
    return {CanonicalJsonErrorKind::InvalidUtf8, offset};
}

[[nodiscard]] CanonicalJsonError integer_out_of_range() noexcept {
    return {CanonicalJsonErrorKind::IntegerOutOfRange, 0U};
}

[[nodiscard]] std::size_t checked_additional_size(
    const std::size_t current,
    const std::size_t additional) {
    if (additional > std::numeric_limits<std::size_t>::max() - current) {
        throw std::length_error("canonical JSON output size overflow");
    }
    return current + additional;
}

void reserve_append(std::string& output, const std::size_t additional) {
    if (additional > output.max_size() - output.size()) {
        throw std::length_error("canonical JSON output exceeds string max_size");
    }
    output.reserve(output.size() + additional);
}

[[nodiscard]] std::size_t quoted_size(const std::string_view value) {
    std::size_t result = 2U;
    for (const unsigned char character : value) {
        std::size_t encoded_size = 1U;
        switch (character) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                encoded_size = 2U;
                break;
            default:
                if (character < 0x20U) {
                    encoded_size = 6U;
                }
                break;
        }
        result = checked_additional_size(result, encoded_size);
    }
    return result;
}

template <typename Integer>
void append_integer_text(std::string& output, const Integer value) {
    std::array<char, 32U> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 10);
    if (error != std::errc{}) {
        throw std::length_error("canonical JSON integer conversion failed");
    }
    const auto size = static_cast<std::size_t>(end - buffer.data());
    reserve_append(output, size);
    output.append(buffer.data(), size);
}

}  // namespace

std::expected<void, CanonicalJsonError>
validate_canonical_json_utf8(const std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t length = 0U;
        std::uint32_t code_point = 0U;
        if (first >= 0xC2U && first <= 0xDFU) {
            length = 2U;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            length = 3U;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            length = 4U;
            code_point = first & 0x07U;
        } else {
            return std::unexpected(invalid_utf8(index));
        }
        if (value.size() - index < length) {
            return std::unexpected(invalid_utf8(index));
        }
        for (std::size_t continuation = 1U;
             continuation < length;
             ++continuation) {
            const auto byte =
                static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return std::unexpected(invalid_utf8(index));
            }
            code_point = (code_point << 6U) | (byte & 0x3FU);
        }

        const bool overlong =
            (length == 2U && code_point < 0x80U) ||
            (length == 3U && code_point < 0x800U) ||
            (length == 4U && code_point < 0x10000U);
        const bool surrogate =
            code_point >= 0xD800U && code_point <= 0xDFFFU;
        if (overlong || surrogate || code_point > 0x10FFFFU) {
            return std::unexpected(invalid_utf8(index));
        }
        index += length;
    }
    return {};
}

std::expected<void, CanonicalJsonError>
append_canonical_json_quoted_string(std::string& output,
                                    const std::string_view value) {
    if (auto validated = validate_canonical_json_utf8(value); !validated) {
        return validated;
    }

    constexpr std::array<char, 16U> hex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string encoded;
    encoded.reserve(quoted_size(value));
    encoded.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                encoded.append("\\\"");
                break;
            case '\\':
                encoded.append("\\\\");
                break;
            case '\b':
                encoded.append("\\b");
                break;
            case '\f':
                encoded.append("\\f");
                break;
            case '\n':
                encoded.append("\\n");
                break;
            case '\r':
                encoded.append("\\r");
                break;
            case '\t':
                encoded.append("\\t");
                break;
            default:
                if (character < 0x20U) {
                    encoded.append("\\u00");
                    encoded.push_back(hex[(character >> 4U) & 0x0FU]);
                    encoded.push_back(hex[character & 0x0FU]);
                } else {
                    encoded.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    encoded.push_back('"');
    reserve_append(output, encoded.size());
    output.append(encoded);
    return {};
}

std::expected<void, CanonicalJsonError>
append_canonical_json_signed_integer(std::string& output,
                                     const std::int64_t value) {
    if (value < kCanonicalJsonMinimumSafeInteger ||
        value > kCanonicalJsonMaximumSafeInteger) {
        return std::unexpected(integer_out_of_range());
    }
    append_integer_text(output, value);
    return {};
}

std::expected<void, CanonicalJsonError>
append_canonical_json_unsigned_integer(std::string& output,
                                       const std::uint64_t value) {
    if (value >
        static_cast<std::uint64_t>(kCanonicalJsonMaximumSafeInteger)) {
        return std::unexpected(integer_out_of_range());
    }
    append_integer_text(output, value);
    return {};
}

}  // namespace kairosboot::fleet
