// SPDX-License-Identifier: MIT
#include "src/fastboot/update_plan.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using kairosboot::fastboot::DeterministicUpdatePlan;
using kairosboot::fastboot::PlannedRebootTarget;
using kairosboot::fastboot::PlannedSlot;
using kairosboot::fastboot::RequirementAction;
using kairosboot::fastboot::UpdateManifestKind;
using kairosboot::fastboot::UpdatePlanError;
using kairosboot::fastboot::UpdatePlanErrorCode;
using kairosboot::fastboot::UpdatePlanLimits;
using kairosboot::fastboot::UpdateTaskKind;
using kairosboot::fastboot::make_update_plan;
using kairosboot::fastboot::parse_update_manifest;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            throw CheckFailure(                                                  \
                std::string("check failed at line ") +                         \
                std::to_string(__LINE__) + ": " #condition);                  \
        }                                                                       \
    } while (false)

UpdatePlanError expect_error(
    const std::string_view android_info,
    const std::string_view fastboot_info,
    const UpdatePlanErrorCode code,
    const UpdatePlanLimits& limits = {}) {
    auto result = parse_update_manifest(android_info, fastboot_info, limits);
    CHECK(!result);
    CHECK(result.error().code == code);
    return std::move(result.error());
}

void source_derived_golden_preserves_order_and_conditions() {
    constexpr std::string_view android_info =
        "product=atlas|boreal\n"
        "reject version-bootloader=bad*\n"
        "require-for-product:atlas version-baseband=1|2\n"
        "require board=atlas\n"
        "require partition-exists=vendor\n";
    constexpr std::string_view fastboot_info =
        "   # generated package plan\n"
        "flash --apply-vbmeta boot\n"
        "version 1\n"
        "flash system images/system.img --slot-other\n"
        "if-wipe erase userdata\n"
        "reboot fastboot\n"
        "update-super\n"
        "reboot\n"
        "erase cache\n";

    auto parsed = parse_update_manifest(android_info, fastboot_info);
    CHECK(parsed);
    CHECK(parsed->fastboot_info_version == std::uint32_t{1});
    CHECK(parsed->requirements.size() == 5);
    CHECK(parsed->tasks.size() == 7);

    CHECK(parsed->requirements[0].variable == "product");
    CHECK(parsed->requirements[0].options ==
          std::vector<std::string>({"atlas", "boreal"}));
    CHECK(parsed->requirements[1].action == RequirementAction::Reject);
    CHECK(parsed->requirements[1].variable == "version-bootloader");
    CHECK(parsed->requirements[2].product == "atlas");
    CHECK(parsed->requirements[2].variable == "version-baseband");
    CHECK(parsed->requirements[3].variable == "product");
    CHECK(parsed->requirements[4].variable == "partition-exists");

    CHECK(parsed->tasks[0].kind == UpdateTaskKind::Flash);
    CHECK(parsed->tasks[0].partition == "boot");
    CHECK(parsed->tasks[0].artifact == "boot.img");
    CHECK(parsed->tasks[0].apply_vbmeta);
    CHECK(parsed->tasks[0].slot == PlannedSlot::Default);
    CHECK(parsed->tasks[1].kind == UpdateTaskKind::Flash);
    CHECK(parsed->tasks[1].partition == "system");
    CHECK(parsed->tasks[1].artifact == "images/system.img");
    CHECK(parsed->tasks[1].slot == PlannedSlot::Other);
    CHECK(parsed->tasks[2].kind == UpdateTaskKind::Erase);
    CHECK(parsed->tasks[2].conditional_on_wipe);
    CHECK(parsed->tasks[3].kind == UpdateTaskKind::Reboot);
    CHECK(parsed->tasks[3].reboot_target == PlannedRebootTarget::Fastboot);
    CHECK(parsed->tasks[4].kind == UpdateTaskKind::UpdateSuper);
    CHECK(parsed->tasks[5].kind == UpdateTaskKind::Reboot);
    CHECK(parsed->tasks[5].reboot_target == PlannedRebootTarget::System);
    CHECK(parsed->tasks[6].kind == UpdateTaskKind::Erase);

    const auto without_wipe = make_update_plan(*parsed, false);
    const auto with_wipe = make_update_plan(*parsed, true);
    CHECK(without_wipe.tasks.size() == 6);
    CHECK(without_wipe.tasks[0].partition == "boot");
    CHECK(without_wipe.tasks[1].partition == "system");
    CHECK(without_wipe.tasks[2].kind == UpdateTaskKind::Reboot);
    CHECK(without_wipe.tasks[5].partition == "cache");
    CHECK(with_wipe.tasks.size() == 7);
    CHECK(with_wipe.tasks[2].partition == "userdata");
    CHECK(with_wipe.requirements.size() == 5);
}

void fastboot_lexing_matches_literal_space_tokenize() {
    auto comments = parse_update_manifest(
        {}, "   #comment words\n#second\nversion 1\n");
    CHECK(comments);
    CHECK(comments->tasks.empty());

    auto inline_comment =
        parse_update_manifest({}, "version 1\nflash boot #image\n");
    CHECK(inline_comment);
    CHECK(inline_comment->tasks.size() == 1);
    CHECK(inline_comment->tasks[0].artifact == "#image");

    const auto tab = expect_error(
        {}, "flash\tboot\n", UpdatePlanErrorCode::Syntax);
    CHECK(tab.location.line == 1);
    CHECK(tab.location.column == 1);

    const auto crlf = expect_error(
        {}, "version 1\r\n", UpdatePlanErrorCode::Syntax);
    CHECK(crlf.location.line == 1);
    CHECK(crlf.location.column == 9);

    const std::string bom = "\xef\xbb\xbfversion 1\n";
    const auto bom_error =
        expect_error({}, bom, UpdatePlanErrorCode::Syntax);
    CHECK(bom_error.location.column == 1);
}

void version_forms_follow_aosp_parse_uint() {
    for (const auto spelling :
         {"0", "00", "01", "0x0", "0x1", "0X1", "+1", "-0"}) {
        const auto text = std::string("version ") + spelling + "\n";
        auto result = parse_update_manifest({}, text);
        CHECK(result);
        CHECK(result->fastboot_info_version.has_value());
        CHECK(*result->fastboot_info_version <= 1);
    }

    auto absent = parse_update_manifest({}, "# no version\nreboot\n");
    CHECK(absent);
    CHECK(!absent->fastboot_info_version);

    const auto newer = expect_error(
        {}, "version 2\n", UpdatePlanErrorCode::UnsupportedVersion);
    CHECK(newer.location.column == 9);
    expect_error({}, "version\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "version 1 2\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "version .01\n", UpdatePlanErrorCode::Syntax);
    const auto duplicate = expect_error(
        {}, "version 0\nversion 1\n", UpdatePlanErrorCode::Duplicate);
    CHECK(duplicate.location.line == 2);
    CHECK(duplicate.location.column == 1);
}

void flash_flags_and_positionals_follow_frozen_parser() {
    const std::array<std::string_view, 3> forms{
        "flash --slot-other --apply-vbmeta system dir/system.img\n",
        "flash system --apply-vbmeta dir/system.img --slot-other\n",
        "flash system dir/system.img --slot-other --apply-vbmeta\n",
    };
    for (const auto form : forms) {
        auto result = parse_update_manifest({}, form);
        CHECK(result);
        CHECK(result->tasks.size() == 1);
        CHECK(result->tasks[0].partition == "system");
        CHECK(result->tasks[0].artifact == "dir/system.img");
        CHECK(result->tasks[0].slot == PlannedSlot::Other);
        CHECK(result->tasks[0].apply_vbmeta);
    }

    // Frozen AOSP treats an unknown flag spelling as the next positional.
    auto positional = parse_update_manifest({}, "flash --typo image.bin\n");
    CHECK(positional);
    CHECK(positional->tasks[0].partition == "--typo");
    CHECK(positional->tasks[0].artifact == "image.bin");

    const auto duplicate_flag = expect_error(
        {},
        "flash --apply-vbmeta boot --apply-vbmeta\n",
        UpdatePlanErrorCode::Duplicate);
    CHECK(duplicate_flag.location.column == 27);
    expect_error(
        {},
        "flash --slot-other boot --slot-other\n",
        UpdatePlanErrorCode::Duplicate);
    const auto missing = expect_error(
        {},
        "flash --slot-other --apply-vbmeta\n",
        UpdatePlanErrorCode::Syntax);
    CHECK(missing.location.column == 34);
    const auto extra = expect_error(
        {}, "flash boot boot.img extra\n", UpdatePlanErrorCode::Syntax);
    CHECK(extra.location.column == 21);
}

void commands_cover_only_executable_frozen_tasks() {
    auto result = parse_update_manifest(
        {},
        "reboot\n"
        "reboot bootloader\n"
        "reboot recovery\n"
        "reboot fastboot\n"
        "update-super\n"
        "erase cache\n"
        "if-wipe erase userdata\n");
    CHECK(result);
    CHECK(result->tasks.size() == 7);
    CHECK(result->tasks[0].reboot_target == PlannedRebootTarget::System);
    CHECK(result->tasks[1].reboot_target ==
          PlannedRebootTarget::Bootloader);
    CHECK(result->tasks[2].reboot_target == PlannedRebootTarget::Recovery);
    CHECK(result->tasks[3].reboot_target == PlannedRebootTarget::Fastboot);
    CHECK(!result->tasks[5].conditional_on_wipe);
    CHECK(result->tasks[6].conditional_on_wipe);

    expect_error({}, "reboot userspace\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "reboot fastboot extra\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "update-super super\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "erase\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "erase cache more\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "fetch system\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "if-wipe nonsense\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "if-wipe version 1\n", UpdatePlanErrorCode::Syntax);
    expect_error({}, "if-wipe if-wipe erase cache\n", UpdatePlanErrorCode::Syntax);
}

void requirement_grammar_preserves_aosp_edge_cases() {
    auto result = parse_update_manifest(
        "require product=alpha| beta |\n"
        "reject version-bootloader=bad*\n"
        "board=alpha\n"
        "require-for-product:gamma version-baseband=1|2\n"
        "require = alpha\n"
        "reject = beta\n"
        "require-for-product:gamma = value\n"
        "foo=bar=baz\n",
        {});
    CHECK(result);
    CHECK(result->requirements.size() == 8);
    CHECK(result->requirements[0].options ==
          std::vector<std::string>({"alpha", "beta", ""}));
    CHECK(result->requirements[1].action == RequirementAction::Reject);
    CHECK(result->requirements[2].variable == "product");
    CHECK(result->requirements[3].product == "gamma");
    CHECK(result->requirements[4].variable == "require");
    CHECK(result->requirements[4].action == RequirementAction::Require);
    CHECK(result->requirements[5].variable == "reject");
    CHECK(result->requirements[5].action == RequirementAction::Require);
    CHECK(result->requirements[6].variable ==
          "require-for-product:gamma");
    CHECK(!result->requirements[6].product);
    CHECK(result->requirements[7].variable == "foo=bar");
    CHECK(result->requirements[7].options ==
          std::vector<std::string>({"baz"}));

    auto leading = parse_update_manifest("   product = alpha\n", {});
    CHECK(leading);
    CHECK(leading->requirements[0].variable == "product");

    expect_error("   require product=alpha\n", {}, UpdatePlanErrorCode::Syntax);
    expect_error(" \t \r", {}, UpdatePlanErrorCode::Syntax);
    expect_error("# comment\n", {}, UpdatePlanErrorCode::Syntax);
    expect_error("require product\n", {}, UpdatePlanErrorCode::Syntax);
    expect_error("require-for-product:gamma product\n", {},
                 UpdatePlanErrorCode::Syntax);
}

void unsafe_artifact_paths_are_rejected_at_the_offending_segment() {
    const std::array<std::string_view, 8> unsafe_paths{
        "../boot.img",
        "images/../boot.img",
        "/absolute.img",
        "C:relative.img",
        "images\\boot.img",
        "images//boot.img",
        "images/./boot.img",
        "images/",
    };
    for (const auto path : unsafe_paths) {
        const auto line = std::string("flash boot ") + std::string(path) + "\n";
        const auto error = expect_error(
            {}, line, UpdatePlanErrorCode::UnsafeArtifactPath);
        CHECK(error.location.manifest == UpdateManifestKind::FastbootInfo);
        CHECK(error.location.line == 1);
    }

    auto safe =
        parse_update_manifest({}, "flash boot images/nested/boot.img\n");
    CHECK(safe);
    CHECK(safe->tasks[0].artifact == "images/nested/boot.img");

    expect_error(
        {}, "flash ../boot\n", UpdatePlanErrorCode::UnsafeArtifactPath);
}

void ambiguous_flash_targets_and_flags_are_rejected() {
    const auto duplicate = expect_error(
        {},
        "flash system first.img\nflash system second.img\n",
        UpdatePlanErrorCode::Duplicate);
    CHECK(duplicate.location.line == 2);
    CHECK(duplicate.location.column == 7);
    CHECK(duplicate.message.find("line 1, column 7") != std::string::npos);

    auto separate_slots = parse_update_manifest(
        {},
        "flash system first.img\n"
        "flash --slot-other system second.img\n");
    CHECK(separate_slots);
    CHECK(separate_slots->tasks.size() == 2);

    expect_error(
        {},
        "flash system first.img\n"
        "if-wipe flash system second.img\n",
        UpdatePlanErrorCode::Duplicate);
}

void diagnostics_report_original_byte_line_and_column() {
    std::string android_nul = "product=ok\nrequire bad=va";
    android_nul.push_back('\0');
    android_nul += "lue\n";
    const auto android_error = expect_error(
        android_nul, {}, UpdatePlanErrorCode::EmbeddedNul);
    CHECK(android_error.location.manifest == UpdateManifestKind::AndroidInfo);
    CHECK(android_error.location.line == 2);
    CHECK(android_error.location.column == 15);
    CHECK(android_error.location.byte_offset == 25);

    std::string fastboot_nul = "version 1\nflash bo";
    fastboot_nul.push_back('\0');
    fastboot_nul += "ot\n";
    const auto fastboot_error = expect_error(
        {}, fastboot_nul, UpdatePlanErrorCode::EmbeddedNul);
    CHECK(fastboot_error.location.manifest ==
          UpdateManifestKind::FastbootInfo);
    CHECK(fastboot_error.location.line == 2);
    CHECK(fastboot_error.location.column == 9);

    const auto unknown = expect_error(
        {}, "# ok\n  stage artifact\n", UpdatePlanErrorCode::Syntax);
    CHECK(unknown.location.line == 2);
    CHECK(unknown.location.column == 3);

    const auto missing_equals = expect_error(
        "product=ok\nrequire version-bootloader\n",
        {},
        UpdatePlanErrorCode::Syntax);
    CHECK(missing_equals.location.line == 2);
    CHECK(missing_equals.location.column == 27);
}

void every_declared_bound_fails_without_partial_output() {
    UpdatePlanLimits limits;
    limits.maximum_file_bytes = 3;
    auto error = expect_error(
        {}, "flash boot\n", UpdatePlanErrorCode::LimitExceeded, limits);
    CHECK(error.location.column == 4);

    limits = {};
    limits.maximum_lines = 1;
    error = expect_error(
        {}, "# one\n# two", UpdatePlanErrorCode::LimitExceeded, limits);
    CHECK(error.location.line == 2);
    CHECK(error.location.column == 1);

    limits = {};
    limits.maximum_line_bytes = 5;
    error = expect_error(
        {}, "123456", UpdatePlanErrorCode::LimitExceeded, limits);
    CHECK(error.location.column == 6);

    limits = {};
    limits.maximum_field_bytes = 4;
    expect_error({}, "flash boot\n", UpdatePlanErrorCode::LimitExceeded, limits);

    limits = {};
    limits.maximum_tokens_per_line = 2;
    expect_error(
        {}, "flash boot boot.img\n", UpdatePlanErrorCode::LimitExceeded, limits);

    limits = {};
    limits.maximum_requirements = 1;
    expect_error(
        "product=a\nboard=b\n", {}, UpdatePlanErrorCode::LimitExceeded, limits);

    limits = {};
    limits.maximum_options_per_requirement = 2;
    expect_error(
        "product=a|b|c\n", {}, UpdatePlanErrorCode::LimitExceeded, limits);

    limits = {};
    limits.maximum_total_options = 2;
    expect_error(
        "product=a|b\nboard=c\n", {}, UpdatePlanErrorCode::LimitExceeded, limits);

    limits = {};
    limits.maximum_tasks = 1;
    expect_error(
        {}, "reboot\nerase cache\n", UpdatePlanErrorCode::LimitExceeded, limits);
}

[[nodiscard]] std::string large_comment_manifest(const std::size_t lines) {
    std::string result;
    result.reserve(lines * 10 + 10);
    for (std::size_t index = 0; index < lines; ++index) {
        result += "# ignored\n";
    }
    result += "version 1\n";
    return result;
}

[[nodiscard]] std::chrono::nanoseconds parse_duration(
    const std::string& fastboot_info,
    const UpdatePlanLimits& limits) {
    const auto start = std::chrono::steady_clock::now();
    auto parsed = parse_update_manifest({}, fastboot_info, limits);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(parsed);
    CHECK(parsed->tasks.empty());
    CHECK(parsed->fastboot_info_version == std::uint32_t{1});
    return elapsed;
}

void hundred_thousand_lines_remain_near_linear() {
    UpdatePlanLimits limits;
    limits.maximum_file_bytes = 2U * 1024U * 1024U;
    limits.maximum_lines = 100'010;
    const auto small = large_comment_manifest(25'000);
    const auto large = large_comment_manifest(100'000);

    const auto small_time = parse_duration(small, limits);
    const auto large_time = parse_duration(large, limits);
    CHECK(large_time <= small_time * 8 + std::chrono::milliseconds(50));
    std::cout << "METRIC: 100k update-plan lines "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     large_time)
                     .count()
              << " ms\n";
}

[[nodiscard]] std::string summarize(const DeterministicUpdatePlan& plan) {
    std::string result =
        std::to_string(plan.requirements.size()) + ":" +
        std::to_string(plan.tasks.size()) + ":" +
        (plan.fastboot_info_version
             ? std::to_string(*plan.fastboot_info_version)
             : "none");
    for (const auto& task : plan.tasks) {
        result.push_back('|');
        result += std::to_string(static_cast<unsigned>(task.kind));
        result.push_back(':');
        result += task.partition;
        result.push_back(':');
        result += task.artifact;
    }
    return result;
}

void pure_planning_is_deterministic_under_concurrency() {
    constexpr std::string_view android_info =
        "product=atlas|boreal\nboard=atlas\n";
    constexpr std::string_view fastboot_info =
        "version 1\nflash boot\nif-wipe erase userdata\nreboot\n";
    auto parsed = parse_update_manifest(android_info, fastboot_info);
    CHECK(parsed);
    const auto expected = summarize(make_update_plan(*parsed, true));

    std::array<std::string, 8> results;
    std::array<std::thread, 8> threads;
    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::thread([&, index] {
            for (std::size_t iteration = 0; iteration < 100; ++iteration) {
                auto local = parse_update_manifest(android_info, fastboot_info);
                CHECK(local);
                results[index] = summarize(make_update_plan(*local, true));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (const auto& result : results) {
        CHECK(result == expected);
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"source-derived golden", source_derived_golden_preserves_order_and_conditions},
        {"literal-space tokenization", fastboot_lexing_matches_literal_space_tokenize},
        {"AOSP uint32 versions", version_forms_follow_aosp_parse_uint},
        {"flash grammar", flash_flags_and_positionals_follow_frozen_parser},
        {"supported commands", commands_cover_only_executable_frozen_tasks},
        {"requirement grammar", requirement_grammar_preserves_aosp_edge_cases},
        {"artifact path safety", unsafe_artifact_paths_are_rejected_at_the_offending_segment},
        {"duplicate ambiguity", ambiguous_flash_targets_and_flags_are_rejected},
        {"source diagnostics", diagnostics_report_original_byte_line_and_column},
        {"bounded inputs", every_declared_bound_fails_without_partial_output},
        {"100k line scale", hundred_thousand_lines_remain_near_linear},
        {"concurrent determinism", pure_planning_is_deterministic_under_concurrency},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "FAIL: " << name << ": unknown exception\n";
        }
    }
    if (failures != 0) {
        std::cerr << failures << " update-plan test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " update-plan tests passed\n";
    return 0;
}
