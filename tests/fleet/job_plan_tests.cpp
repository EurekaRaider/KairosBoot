// SPDX-License-Identifier: MIT
#include "src/fleet/job_plan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using kairosboot::fleet::FlashJobManifest;
using kairosboot::fleet::JobPlan;
using kairosboot::fleet::JobPlanBuildOptions;
using kairosboot::fleet::JobPlanErrorKind;
using kairosboot::fleet::JobPlanFaultPoint;
using kairosboot::fleet::LocatedManifestString;
using kairosboot::fleet::ManifestActiveSlot;
using kairosboot::fleet::ManifestArtifact;
using kairosboot::fleet::ManifestDeviceFailurePolicy;
using kairosboot::fleet::ManifestEraseStep;
using kairosboot::fleet::ManifestFlashSlot;
using kairosboot::fleet::ManifestFlashStep;
using kairosboot::fleet::ManifestOemStep;
using kairosboot::fleet::ManifestPolicy;
using kairosboot::fleet::ManifestRebootStep;
using kairosboot::fleet::ManifestRebootTarget;
using kairosboot::fleet::ManifestSelector;
using kairosboot::fleet::ManifestSetActiveStep;
using kairosboot::fleet::ManifestSourceLocation;
using kairosboot::fleet::ManifestStep;
using kairosboot::fleet::ManifestTarget;
using kairosboot::fleet::make_job_plan;
using kairosboot::image::Sha256Digest;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            throw CheckFailure(std::string("check failed at line ") +         \
                               std::to_string(__LINE__) + ": " #condition);    \
        }                                                                       \
    } while (false)

static_assert(!std::is_copy_constructible_v<JobPlan>);
static_assert(!std::is_copy_assignable_v<JobPlan>);
static_assert(std::is_nothrow_move_constructible_v<JobPlan>);
static_assert(std::is_nothrow_move_assignable_v<JobPlan>);

inline constexpr ManifestSourceLocation kLocation{1U, 1U};

[[nodiscard]] LocatedManifestString located(std::string value) {
    return {.value = std::move(value), .location = kLocation};
}

[[nodiscard]] unsigned int hex_value(const char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned int>(value - 'a' + 10);
    }
    throw CheckFailure("invalid test SHA-256 fixture");
}

[[nodiscard]] Sha256Digest digest_from_hex(const std::string_view value) {
    CHECK(value.size() == 64U);
    Sha256Digest digest{};
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        const auto high = hex_value(value[index * 2U]);
        const auto low = hex_value(value[index * 2U + 1U]);
        digest[index] = static_cast<std::byte>((high << 4U) | low);
    }
    return digest;
}

[[nodiscard]] ManifestStep flash_step(
    std::string partition,
    std::string artifact,
    const std::optional<ManifestFlashSlot> slot = std::nullopt) {
    return {
        .location = kLocation,
        .payload = ManifestFlashStep{
            .partition = located(std::move(partition)),
            .artifact = located(std::move(artifact)),
            .slot = slot,
            .slot_location = slot.has_value()
                ? std::optional<ManifestSourceLocation>{kLocation}
                : std::nullopt,
        },
    };
}

[[nodiscard]] ManifestStep erase_step(std::string partition) {
    return {
        .location = kLocation,
        .payload = ManifestEraseStep{located(std::move(partition))},
    };
}

[[nodiscard]] ManifestStep active_step(const ManifestActiveSlot slot) {
    return {
        .location = kLocation,
        .payload = ManifestSetActiveStep{slot, kLocation},
    };
}

[[nodiscard]] ManifestStep reboot_step(
    const ManifestRebootTarget target,
    const bool explicit_target = true) {
    return {
        .location = kLocation,
        .payload = ManifestRebootStep{
            .target = target,
            .target_location = explicit_target
                ? std::optional<ManifestSourceLocation>{kLocation}
                : std::nullopt,
        },
    };
}

[[nodiscard]] ManifestStep oem_step(std::string command) {
    return {
        .location = kLocation,
        .payload = ManifestOemStep{located(std::move(command))},
    };
}

[[nodiscard]] ManifestArtifact artifact(std::string id,
                                        std::string path,
                                        std::string sha256) {
    return {
        .location = kLocation,
        .id = located(std::move(id)),
        .path = located(std::move(path)),
        .sha256 = located(std::move(sha256)),
    };
}

[[nodiscard]] ManifestTarget target(
    std::string name,
    std::vector<LocatedManifestString> serials,
    std::vector<LocatedManifestString> usb_paths,
    std::string expected_product,
    std::vector<ManifestStep> steps) {
    return {
        .location = kLocation,
        .name = located(std::move(name)),
        .selector = ManifestSelector{
            .location = kLocation,
            .serials = std::move(serials),
            .usb_paths = std::move(usb_paths),
        },
        .expected_product = located(std::move(expected_product)),
        .steps = std::move(steps),
    };
}

[[nodiscard]] FlashJobManifest fixture_manifest() {
    return {
        .location = kLocation,
        .api_version = located("kairosboot.io/v1"),
        .kind = located("FlashJob"),
        .source_sha256 = digest_from_hex(
            "58539b1d8a0ba3108ffd0f0ea835d25efca9a6ce85b06cd15f0f1307d4b1c9ef"),
        .artifacts = {
            artifact("system",
                     "images/system.img",
                     std::string(64U, '1')),
        },
        .targets = {
            target("product-a",
                   {located("SERIAL-01"), located("SERIAL-02")},
                   {},
                   "product_a",
                   {flash_step("system", "system")}),
        },
        .policy = ManifestPolicy{},
    };
}

[[nodiscard]] FlashJobManifest comprehensive_manifest() {
    std::vector<ManifestStep> steps;
    steps.push_back(flash_step("partition-0", "first"));
    steps.push_back(
        flash_step("partition-1", "first", ManifestFlashSlot::Current));
    steps.push_back(
        flash_step("partition-2", "first", ManifestFlashSlot::Other));
    steps.push_back(
        flash_step("partition-3", "first", ManifestFlashSlot::All));
    steps.push_back(flash_step("partition-4", "first", ManifestFlashSlot::A));
    steps.push_back(flash_step("partition-5", "first", ManifestFlashSlot::B));
    steps.push_back(erase_step("metadata"));
    steps.push_back(active_step(ManifestActiveSlot::A));
    steps.push_back(active_step(ManifestActiveSlot::B));
    steps.push_back(active_step(ManifestActiveSlot::Other));
    steps.push_back(reboot_step(ManifestRebootTarget::System, false));
    steps.push_back(reboot_step(ManifestRebootTarget::Bootloader));
    steps.push_back(reboot_step(ManifestRebootTarget::Recovery));
    steps.push_back(reboot_step(ManifestRebootTarget::Fastboot));
    steps.push_back(oem_step("diagnostic \"mode\"\\next\n\xE9\x9B\xAA"));

    ManifestPolicy policy;
    policy.on_device_failure = ManifestDeviceFailurePolicy::Stop;
    policy.max_parallel_devices = 7U;
    policy.memory_budget.automatic = false;
    policy.memory_budget.bytes = 1048576U;
    policy.memory_budget.location = kLocation;

    return {
        .location = kLocation,
        .api_version = located("kairosboot.io/v1"),
        .kind = located("FlashJob"),
        .source_sha256 = digest_from_hex(std::string(64U, '0')),
        .artifacts = {
            artifact("first",
                     "does-not-exist/first.img",
                     std::string(64U, 'a')),
            artifact("second",
                     "does-not-exist/second.img",
                     std::string(64U, 'b')),
        },
        .targets = {
            target("target-z",
                   {located("SERIAL-Z"), located("SERIAL-A")},
                   {located("usb:2-1")},
                   "product\"\\\n\xE9\x9B\xAA",
                   std::move(steps)),
            target("target-a",
                   {located("same-value")},
                   {located("same-value")},
                   "product-a",
                   {erase_step("userdata")}),
        },
        .policy = std::move(policy),
    };
}

void fixture_matches_frozen_json_and_digest() {
    constexpr std::string_view expected =
        R"json({"artifacts":[{"id":"system","index":0,"path":"images/system.img","sha256":"1111111111111111111111111111111111111111111111111111111111111111"}],"kind":"FlashJob","manifestApiVersion":"kairosboot.io/v1","manifestSha256":"58539b1d8a0ba3108ffd0f0ea835d25efca9a6ce85b06cd15f0f1307d4b1c9ef","policy":{"maxParallelDevices":32,"memoryBudget":"auto","onDeviceFailure":"continue"},"schemaVersion":1,"targets":[{"expectedProduct":"product_a","index":0,"name":"product-a","selector":{"serials":["SERIAL-01","SERIAL-02"],"usbPaths":[]},"steps":[{"artifact":"system","index":0,"oemCommand":null,"operation":"flash","partition":"system","rebootTarget":null,"slot":null}]}]})json";
    constexpr std::string_view expected_plan_sha256 =
        "992daa21b5ea246910fc5d9ffffafed3e36e883d6a407b70abe3b04def3823f4";

    auto manifest = fixture_manifest();
    const auto expected_manifest = manifest;
    auto plan = make_job_plan(std::move(manifest));
    CHECK(plan);
    CHECK(plan->manifest() == expected_manifest);
    CHECK(plan->canonical_json() == expected);
    CHECK(plan->canonical_json().find('\n') == std::string_view::npos);
    CHECK(plan->sha256_hex() == expected_plan_sha256);
    CHECK(plan->sha256() == digest_from_hex(expected_plan_sha256));

    JobPlan moved = std::move(*plan);
    CHECK(moved.canonical_json() == expected);
    CHECK(moved.sha256_hex() == expected_plan_sha256);
}

void all_operations_slots_defaults_order_and_escaping_are_frozen() {
    auto first_manifest = comprehensive_manifest();
    auto second_manifest = comprehensive_manifest();
    auto first = make_job_plan(std::move(first_manifest));
    auto second = make_job_plan(std::move(second_manifest));
    CHECK(first);
    CHECK(second);
    CHECK(first->canonical_json() == second->canonical_json());
    CHECK(first->sha256() == second->sha256());
    CHECK(first->sha256_hex() == second->sha256_hex());

    const auto json = first->canonical_json();
    CHECK(json.find('\n') == std::string_view::npos);
    CHECK(json.find("product\\\"\\\\\\n\xE9\x9B\xAA") !=
          std::string_view::npos);
    CHECK(json.find("diagnostic \\\"mode\\\"\\\\next\\n\xE9\x9B\xAA") !=
          std::string_view::npos);
    CHECK(json.find("\"memoryBudget\":1048576") != std::string_view::npos);
    CHECK(json.find("\"onDeviceFailure\":\"stop\"") !=
          std::string_view::npos);
    CHECK(json.find("\"serials\":[\"SERIAL-Z\",\"SERIAL-A\"]") !=
          std::string_view::npos);
    CHECK(json.find("\"serials\":[\"same-value\"],\"usbPaths\":["
                    "\"same-value\"]") != std::string_view::npos);

    const std::array<std::string_view, 15U> frozen_steps{
        R"json({"artifact":"first","index":0,"oemCommand":null,"operation":"flash","partition":"partition-0","rebootTarget":null,"slot":null})json",
        R"json({"artifact":"first","index":1,"oemCommand":null,"operation":"flash","partition":"partition-1","rebootTarget":null,"slot":"current"})json",
        R"json({"artifact":"first","index":2,"oemCommand":null,"operation":"flash","partition":"partition-2","rebootTarget":null,"slot":"other"})json",
        R"json({"artifact":"first","index":3,"oemCommand":null,"operation":"flash","partition":"partition-3","rebootTarget":null,"slot":"all"})json",
        R"json({"artifact":"first","index":4,"oemCommand":null,"operation":"flash","partition":"partition-4","rebootTarget":null,"slot":"a"})json",
        R"json({"artifact":"first","index":5,"oemCommand":null,"operation":"flash","partition":"partition-5","rebootTarget":null,"slot":"b"})json",
        R"json({"artifact":null,"index":6,"oemCommand":null,"operation":"erase","partition":"metadata","rebootTarget":null,"slot":null})json",
        R"json({"artifact":null,"index":7,"oemCommand":null,"operation":"set_active","partition":null,"rebootTarget":null,"slot":"a"})json",
        R"json({"artifact":null,"index":8,"oemCommand":null,"operation":"set_active","partition":null,"rebootTarget":null,"slot":"b"})json",
        R"json({"artifact":null,"index":9,"oemCommand":null,"operation":"set_active","partition":null,"rebootTarget":null,"slot":"other"})json",
        R"json({"artifact":null,"index":10,"oemCommand":null,"operation":"reboot","partition":null,"rebootTarget":"system","slot":null})json",
        R"json({"artifact":null,"index":11,"oemCommand":null,"operation":"reboot","partition":null,"rebootTarget":"bootloader","slot":null})json",
        R"json({"artifact":null,"index":12,"oemCommand":null,"operation":"reboot","partition":null,"rebootTarget":"recovery","slot":null})json",
        R"json({"artifact":null,"index":13,"oemCommand":null,"operation":"reboot","partition":null,"rebootTarget":"fastboot","slot":null})json",
        "{\"artifact\":null,\"index\":14,\"oemCommand\":\"diagnostic "
        "\\\"mode\\\"\\\\next\\n\xE9\x9B\xAA\",\"operation\":\"oem\","
        "\"partition\":null,\"rebootTarget\":null,\"slot\":null}",
    };
    std::size_t cursor = 0U;
    for (const auto expected_step : frozen_steps) {
        const auto found = json.find(expected_step, cursor);
        CHECK(found != std::string_view::npos);
        cursor = found + expected_step.size();
    }

    const auto first_artifact = json.find("\"id\":\"first\"");
    const auto second_artifact = json.find("\"id\":\"second\"");
    const auto first_target = json.find("\"name\":\"target-z\"");
    const auto second_target = json.find("\"name\":\"target-a\"");
    CHECK(first_artifact < second_artifact);
    CHECK(first_target < second_target);
}

void raw_manifest_digest_changes_plan_without_artifact_io() {
    auto first_manifest = comprehensive_manifest();
    auto second_manifest = comprehensive_manifest();
    second_manifest.source_sha256[0] = std::byte{1U};

    auto first = make_job_plan(std::move(first_manifest));
    auto second = make_job_plan(std::move(second_manifest));
    CHECK(first);
    CHECK(second);
    CHECK(first->manifest().artifacts[0].path.value ==
          "does-not-exist/first.img");
    CHECK(first->canonical_json() != second->canonical_json());
    CHECK(first->sha256() != second->sha256());
    CHECK(first->sha256_hex() != second->sha256_hex());
    CHECK(first->canonical_json().find(std::string(64U, '0')) !=
          std::string_view::npos);
    CHECK(second->canonical_json().find(
              "0100000000000000000000000000000000000000000000000000000000000000") !=
          std::string_view::npos);
}

enum class InjectedException : std::uint8_t {
    BadAllocation,
    Length,
    Other,
};

struct Fault final {
    JobPlanFaultPoint point{JobPlanFaultPoint::BeforeSerialization};
    InjectedException exception{InjectedException::BadAllocation};
    std::size_t calls{};
};

void throw_fault(const JobPlanFaultPoint point, void* context) {
    auto& fault = *static_cast<Fault*>(context);
    ++fault.calls;
    if (point != fault.point) {
        return;
    }
    switch (fault.exception) {
        case InjectedException::BadAllocation:
            throw std::bad_alloc{};
        case InjectedException::Length:
            throw std::length_error("injected length error");
        case InjectedException::Other:
            throw std::runtime_error("injected unexpected error");
    }
}

void failure_does_not_publish_a_partial_plan_or_consume_input() {
    auto published_input = fixture_manifest();
    auto published = make_job_plan(std::move(published_input));
    CHECK(published);
    const std::string published_json{published->canonical_json()};
    const std::string published_sha256{published->sha256_hex()};

    auto failing_input = comprehensive_manifest();
    const auto failing_input_before = failing_input;
    Fault fault{
        .point = JobPlanFaultPoint::BeforeSnapshotCommit,
        .exception = InjectedException::Length,
        .calls = 0U,
    };
    const JobPlanBuildOptions options{
        .fault_hook = &throw_fault,
        .fault_context = &fault,
    };
    auto failed = make_job_plan(std::move(failing_input), options);

    // The commit-point failure occurs after JSON and hashing, but expected
    // contains no JobPlan and the caller's manifest has not been moved from.
    CHECK(!failed);
    CHECK(failed.error().kind == JobPlanErrorKind::OutputTooLarge);
    CHECK(failing_input == failing_input_before);
    CHECK(published->canonical_json() == published_json);
    CHECK(published->sha256_hex() == published_sha256);
}

void errors_are_fail_closed_and_do_not_consume_input() {
    auto invalid_utf8 = fixture_manifest();
    invalid_utf8.targets[0].expected_product.value =
        std::string{"ok"} + static_cast<char>(0xC0U);
    const auto invalid_utf8_before = invalid_utf8;
    auto invalid_utf8_result = make_job_plan(std::move(invalid_utf8));
    CHECK(!invalid_utf8_result);
    CHECK(invalid_utf8_result.error().kind == JobPlanErrorKind::InvalidUtf8);
    CHECK(invalid_utf8_result.error().input_byte_offset == 2U);
    CHECK(invalid_utf8 == invalid_utf8_before);

    auto invalid_enum = fixture_manifest();
    invalid_enum.policy.on_device_failure =
        static_cast<ManifestDeviceFailurePolicy>(255U);
    const auto invalid_enum_before = invalid_enum;
    auto invalid_enum_result = make_job_plan(std::move(invalid_enum));
    CHECK(!invalid_enum_result);
    CHECK(invalid_enum_result.error().kind == JobPlanErrorKind::InvalidManifest);
    CHECK(invalid_enum == invalid_enum_before);

    const std::array cases{
        std::pair{Fault{JobPlanFaultPoint::BeforeSerialization,
                        InjectedException::BadAllocation,
                        0U},
                  JobPlanErrorKind::ResourceExhausted},
        std::pair{Fault{JobPlanFaultPoint::BeforeSnapshotCommit,
                        InjectedException::Other,
                        0U},
                  JobPlanErrorKind::UnexpectedFailure},
    };
    for (auto [fault, expected_error] : cases) {
        auto manifest = comprehensive_manifest();
        const auto before = manifest;
        const JobPlanBuildOptions options{
            .fault_hook = &throw_fault,
            .fault_context = &fault,
        };
        auto result = make_job_plan(std::move(manifest), options);
        CHECK(!result);
        CHECK(result.error().kind == expected_error);
        CHECK(result.error().input_byte_offset == 0U);
        CHECK(fault.calls >= 1U);
        CHECK(manifest == before);
    }
}

}  // namespace

int main() {
    using Test = std::pair<std::string_view, void (*)()>;
    const std::array<Test, 5U> tests{
        Test{"fixture matches frozen JSON and digest",
             &fixture_matches_frozen_json_and_digest},
        Test{"all operations, slots, defaults, order, and escaping",
             &all_operations_slots_defaults_order_and_escaping_are_frozen},
        Test{"raw manifest digest changes plan without artifact I/O",
             &raw_manifest_digest_changes_plan_without_artifact_io},
        Test{"failure does not publish a partial plan or consume input",
             &failure_does_not_publish_a_partial_plan_or_consume_input},
        Test{"errors are fail closed and preserve input",
             &errors_are_fail_closed_and_do_not_consume_input},
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
