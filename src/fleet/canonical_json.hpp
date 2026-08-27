// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace kairosboot::fleet {

inline constexpr std::int64_t kCanonicalJsonMaximumSafeInteger =
    9'007'199'254'740'991LL;
inline constexpr std::int64_t kCanonicalJsonMinimumSafeInteger =
    -kCanonicalJsonMaximumSafeInteger;

enum class CanonicalJsonErrorKind : std::uint8_t {
    InvalidUtf8,
    IntegerOutOfRange,
};

struct CanonicalJsonError final {
    CanonicalJsonErrorKind kind{CanonicalJsonErrorKind::InvalidUtf8};
    // For InvalidUtf8, this is the leading byte of the invalid scalar
    // sequence. IntegerOutOfRange does not have a byte offset and uses zero.
    std::size_t input_byte_offset{};

    [[nodiscard]] bool operator==(const CanonicalJsonError&) const = default;
};

// Accepts only shortest-form UTF-8 encodings of Unicode scalar values. UTF-16
// surrogate code points and values greater than U+10FFFF are rejected.
[[nodiscard]] std::expected<void, CanonicalJsonError>
validate_canonical_json_utf8(std::string_view value) noexcept;

// Appends one complete quoted JSON string using the frozen Fleet escaping
// profile. A returned validation error leaves output unchanged.
//
// Allocation and length exceptions from std::string propagate to the caller.
// Encoding is prepared before output is changed and final capacity is reserved
// before the append, so those exceptions also leave output unchanged. The
// caller may pass a value view backed by output itself.
[[nodiscard]] std::expected<void, CanonicalJsonError>
append_canonical_json_quoted_string(std::string& output,
                                    std::string_view value);

// Appends a base-10 I-JSON integer without consulting the process locale. A
// returned range error leaves output unchanged. Allocation and length
// exceptions follow the same rule as append_canonical_json_quoted_string.
[[nodiscard]] std::expected<void, CanonicalJsonError>
append_canonical_json_signed_integer(std::string& output,
                                     std::int64_t value);

[[nodiscard]] std::expected<void, CanonicalJsonError>
append_canonical_json_unsigned_integer(std::string& output,
                                       std::uint64_t value);

}  // namespace kairosboot::fleet
