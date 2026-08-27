// SPDX-License-Identifier: MIT
#include "src/fleet/canonical_json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::fleet::CanonicalJsonErrorKind;
using kairosboot::fleet::append_canonical_json_quoted_string;
using kairosboot::fleet::append_canonical_json_signed_integer;
using kairosboot::fleet::append_canonical_json_unsigned_integer;
using kairosboot::fleet::kCanonicalJsonMaximumSafeInteger;
using kairosboot::fleet::kCanonicalJsonMinimumSafeInteger;
using kairosboot::fleet::validate_canonical_json_utf8;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw CheckFailure(std::string("check failed at line ") +          \
                               std::to_string(__LINE__) + ": " #condition);     \
        }                                                                       \
    } while (false)

[[nodiscard]] std::string bytes(
    const std::initializer_list<unsigned int> values) {
    std::string result;
    result.reserve(values.size());
    for (const auto value : values) {
        CHECK(value <= 0xFFU);
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void valid_scalars_and_frozen_escaping() {
    std::string escaped{"prefix:"};
    std::string controls;
    controls.push_back('"');
    controls.push_back('\\');
    controls.push_back('\b');
    controls.push_back('\f');
    controls.push_back('\n');
    controls.push_back('\r');
    controls.push_back('\t');
    controls.push_back('\0');
    controls.push_back(static_cast<char>(0x01U));
    controls.push_back(static_cast<char>(0x1FU));
    CHECK(append_canonical_json_quoted_string(escaped, controls));
    CHECK(escaped ==
          "prefix:\"\\\"\\\\\\b\\f\\n\\r\\t\\u0000\\u0001\\u001f\"");

    const std::string unicode =
        bytes({0xE9U, 0x9BU, 0xAAU}) + "/" +
        bytes({0xF0U, 0x9FU, 0x98U, 0x80U}) +
        bytes({0xE2U, 0x80U, 0xA8U}) +
        bytes({0xF4U, 0x8FU, 0xBFU, 0xBFU});
    CHECK(validate_canonical_json_utf8(unicode));
    std::string output;
    CHECK(append_canonical_json_quoted_string(output, unicode));
    CHECK(output == std::string{"\""} + unicode + "\"");

    std::string empty;
    CHECK(append_canonical_json_quoted_string(empty, {}));
    CHECK(empty == "\"\"");

    std::string aliased{"same/output"};
    CHECK(append_canonical_json_quoted_string(aliased, aliased));
    CHECK(aliased == "same/output\"same/output\"");

    const std::string composed = bytes({0xC3U, 0xA9U});
    const std::string decomposed = std::string{"e"} + bytes({0xCCU, 0x81U});
    std::string composed_json;
    std::string decomposed_json;
    CHECK(append_canonical_json_quoted_string(composed_json, composed));
    CHECK(append_canonical_json_quoted_string(decomposed_json, decomposed));
    CHECK(composed_json != decomposed_json);
}

void invalid_utf8_is_rejected_without_output_mutation() {
    struct InvalidCase final {
        std::string value;
        std::size_t offset{};
    };
    const std::vector<InvalidCase> invalid{
        {bytes({0xC0U, 0xAFU}), 0U},
        {bytes({0xE0U, 0x80U, 0x80U}), 0U},
        {bytes({0xF0U, 0x9FU, 0x98U}), 0U},
        {bytes({0xEDU, 0xA0U, 0x80U}), 0U},
        {bytes({0xF4U, 0x90U, 0x80U, 0x80U}), 0U},
        {bytes({0x80U}), 0U},
        {std::string{"ok"} + bytes({0xE2U, 0x28U, 0xA1U}), 2U},
    };

    for (const auto& test : invalid) {
        const auto validated = validate_canonical_json_utf8(test.value);
        CHECK(!validated);
        CHECK(validated.error().kind == CanonicalJsonErrorKind::InvalidUtf8);
        CHECK(validated.error().input_byte_offset == test.offset);

        std::string output{"unchanged"};
        const auto appended =
            append_canonical_json_quoted_string(output, test.value);
        CHECK(!appended);
        CHECK(appended.error() == validated.error());
        CHECK(output == "unchanged");
    }
}

class VisibleGrouping final : public std::numpunct<char> {
protected:
    [[nodiscard]] char do_thousands_sep() const override { return '_'; }
    [[nodiscard]] std::string do_grouping() const override { return "\3"; }
};

class GlobalLocaleGuard final {
public:
    explicit GlobalLocaleGuard(std::locale replacement)
        : previous_(std::locale::global(std::move(replacement))) {}

    GlobalLocaleGuard(const GlobalLocaleGuard&) = delete;
    GlobalLocaleGuard& operator=(const GlobalLocaleGuard&) = delete;

    ~GlobalLocaleGuard() { std::locale::global(previous_); }

private:
    std::locale previous_;
};

void safe_integers_are_locale_independent() {
    GlobalLocaleGuard locale{
        std::locale{std::locale::classic(), new VisibleGrouping}};

    std::string output{"["};
    CHECK(append_canonical_json_signed_integer(
        output, kCanonicalJsonMinimumSafeInteger));
    output.push_back(',');
    CHECK(append_canonical_json_signed_integer(output, 0));
    output.push_back(',');
    CHECK(append_canonical_json_signed_integer(
        output, kCanonicalJsonMaximumSafeInteger));
    output.push_back(',');
    CHECK(append_canonical_json_unsigned_integer(
        output,
        static_cast<std::uint64_t>(kCanonicalJsonMaximumSafeInteger)));
    output.push_back(']');
    CHECK(output ==
          "[-9007199254740991,0,9007199254740991,9007199254740991]");
    CHECK(output.find('_') == std::string::npos);
}

void out_of_range_integers_leave_output_unchanged() {
    std::string output{"unchanged"};
    auto result = append_canonical_json_signed_integer(
        output, kCanonicalJsonMinimumSafeInteger - 1);
    CHECK(!result);
    CHECK(result.error().kind == CanonicalJsonErrorKind::IntegerOutOfRange);
    CHECK(result.error().input_byte_offset == 0U);
    CHECK(output == "unchanged");

    result = append_canonical_json_signed_integer(
        output, kCanonicalJsonMaximumSafeInteger + 1);
    CHECK(!result);
    CHECK(result.error().kind == CanonicalJsonErrorKind::IntegerOutOfRange);
    CHECK(output == "unchanged");

    const auto unsigned_result = append_canonical_json_unsigned_integer(
        output,
        static_cast<std::uint64_t>(kCanonicalJsonMaximumSafeInteger) + 1U);
    CHECK(!unsigned_result);
    CHECK(unsigned_result.error().kind ==
          CanonicalJsonErrorKind::IntegerOutOfRange);
    CHECK(output == "unchanged");
}

}  // namespace

int main() {
    const std::array tests{
        std::pair{"valid scalars and frozen escaping",
                  valid_scalars_and_frozen_escaping},
        std::pair{"invalid UTF-8 has transactional output",
                  invalid_utf8_is_rejected_without_output_mutation},
        std::pair{"safe integers are locale independent",
                  safe_integers_are_locale_independent},
        std::pair{"integer range errors have transactional output",
                  out_of_range_integers_leave_output_unchanged},
    };

    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
