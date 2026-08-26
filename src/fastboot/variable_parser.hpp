// SPDX-License-Identifier: MIT
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kairosboot::fastboot {

// Parses numeric getvar payloads returned by real bootloaders. Decimal and
// 0x-prefixed hexadecimal forms are accepted after trimming ASCII whitespace;
// partial, signed, empty and overflowing values are rejected.
[[nodiscard]] inline std::optional<std::uint64_t> parse_unsigned_variable(
    std::string_view value) noexcept {
    constexpr std::string_view whitespace{" \t\r\n\f\v"};
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto last = value.find_last_not_of(whitespace);
    value = value.substr(first, last - first + 1);

    int base = 10;
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value.remove_prefix(2);
    }
    if (value.empty()) {
        return std::nullopt;
    }

    std::uint64_t result = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result, base);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

}  // namespace kairosboot::fastboot
