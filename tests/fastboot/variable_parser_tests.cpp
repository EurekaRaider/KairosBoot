// SPDX-License-Identifier: MIT
#include "src/fastboot/variable_parser.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::parse_unsigned_variable;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string("check failed at line ") + std::to_string(__LINE__) + \
                ": " #condition);                                                \
        }                                                                        \
    } while (false)

void accepts_decimal_and_hexadecimal_bootloader_values() {
    CHECK(parse_unsigned_variable("0").value() == 0);
    CHECK(parse_unsigned_variable("268435456").value() == 268435456ULL);
    CHECK(parse_unsigned_variable("0x10000000").value() == 268435456ULL);
    CHECK(parse_unsigned_variable("0Xffffffff").value() == 0xFFFFFFFFULL);
    CHECK(parse_unsigned_variable("18446744073709551615").value() ==
          UINT64_MAX);
}

void trims_only_outer_ascii_whitespace() {
    CHECK(parse_unsigned_variable("  \t268435456\r\n").value() ==
          268435456ULL);
    CHECK(parse_unsigned_variable("\f0x10\v").value() == 16);
    CHECK(!parse_unsigned_variable("12 34"));
    CHECK(!parse_unsigned_variable("0x1 0"));
}

void rejects_empty_signed_partial_and_overflowing_values() {
    CHECK(!parse_unsigned_variable(""));
    CHECK(!parse_unsigned_variable(" \t\r\n\f\v"));
    CHECK(!parse_unsigned_variable("-1"));
    CHECK(!parse_unsigned_variable("+1"));
    CHECK(!parse_unsigned_variable("0x"));
    CHECK(!parse_unsigned_variable("123junk"));
    CHECK(!parse_unsigned_variable("0xg"));
    CHECK(!parse_unsigned_variable("18446744073709551616"));
    CHECK(!parse_unsigned_variable("0x10000000000000000"));
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"accepted spellings", accepts_decimal_and_hexadecimal_bootloader_values},
        {"outer whitespace", trims_only_outer_ascii_whitespace},
        {"invalid values", rejects_empty_signed_partial_and_overflowing_values},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " variable parser test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " variable parser tests passed\n";
    return 0;
}
