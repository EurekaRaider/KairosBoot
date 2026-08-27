// SPDX-License-Identifier: MIT
#include "src/fleet/manifest.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::fleet::FlashJobManifest;
using kairosboot::fleet::ManifestActiveSlot;
using kairosboot::fleet::ManifestDeviceFailurePolicy;
using kairosboot::fleet::ManifestError;
using kairosboot::fleet::ManifestErrorKind;
using kairosboot::fleet::ManifestFaultPoint;
using kairosboot::fleet::ManifestFlashSlot;
using kairosboot::fleet::ManifestFlashStep;
using kairosboot::fleet::ManifestOemStep;
using kairosboot::fleet::ManifestParseOptions;
using kairosboot::fleet::ManifestRebootStep;
using kairosboot::fleet::ManifestRebootTarget;
using kairosboot::fleet::ManifestSetActiveStep;
using kairosboot::fleet::kMaximumManifestBytes;
using kairosboot::fleet::load_fleet_manifest_file;
using kairosboot::fleet::parse_fleet_manifest_source;
using kairosboot::image::FileImageSource;
using kairosboot::image::sha256_hex;

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

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::uint64_t next_id{};
        path_ = std::filesystem::temp_directory_path() /
                ("kairosboot-fleet-manifest-" +
                 std::to_string(++next_id) + "-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        std::filesystem::create_directories(path_);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] ManifestParseOptions long_options() {
    return ManifestParseOptions{
        .deadline = kairosboot::fleet::ManifestClock::now() + 1h,
        .cancellation = {},
        .fault_hook = {},
        .fault_context = {},
    };
}

void write_bytes(const std::filesystem::path& path,
                 const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

[[nodiscard]] std::string minimal_manifest() {
    return
        "apiVersion: kairosboot.io/v1\n"
        "kind: FlashJob\n"
        "artifacts:\n"
        "  - id: system\n"
        "    path: images/system.img\n"
        "    sha256: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
        "targets:\n"
        "  - name: product-a\n"
        "    selector:\n"
        "      serials: [SERIAL-A]\n"
        "    expectedProduct: product_a\n"
        "    steps:\n"
        "      - flash: {partition: system, artifact: system}\n";
}

[[nodiscard]] std::string complete_manifest() {
    return
        "apiVersion: kairosboot.io/v1\n"
        "kind: FlashJob\n"
        "artifacts:\n"
        "  - id: system\n"
        "    path: images/system.img\n"
        "    sha256: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
        "  - id: boot\n"
        "    path: images/boot.img\n"
        "    sha256: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "targets:\n"
        "  - name: product-a\n"
        "    selector:\n"
        "      serials: [SERIAL-B, SERIAL-A]\n"
        "      usbPaths: [usb/1/2, usb/1/3]\n"
        "    expectedProduct: product_a\n"
        "    steps:\n"
        "      - flash: {partition: system, artifact: system, slot: other}\n"
        "      - erase: {partition: cache}\n"
        "      - setActive: {slot: b}\n"
        "      - reboot: {target: fastboot}\n"
        "      - oem: {command: diagnostic-mode}\n"
        "  - name: product-b\n"
        "    selector:\n"
        "      usbPaths: [usb/2/1]\n"
        "    expectedProduct: product_b\n"
        "    steps:\n"
        "      - flash: {partition: boot, artifact: boot, slot: current}\n"
        "      - reboot: {}\n"
        "policy:\n"
        "  onDeviceFailure: stop\n"
        "  maxParallelDevices: 7\n"
        "  memoryBudget: 1048576\n";
}

[[nodiscard]] std::string replace_once(std::string input,
                                       const std::string_view from,
                                       const std::string_view to) {
    const auto position = input.find(from);
    CHECK(position != std::string::npos);
    input.replace(position, from.size(), to);
    return input;
}

[[nodiscard]] std::expected<FlashJobManifest, ManifestError> parse_text(
    const std::string_view input,
    const ManifestParseOptions& options = long_options()) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "job.yaml";
    write_bytes(path, input);
    return load_fleet_manifest_file(path, options);
}

ManifestError expect_error(
    const std::string_view input,
    const ManifestErrorKind kind,
    const ManifestParseOptions& options = long_options()) {
    auto result = parse_text(input, options);
    CHECK(!result);
    if (result.error().kind != kind) {
        throw CheckFailure(
            "expected manifest error kind " +
            std::to_string(static_cast<unsigned int>(kind)) + ", got " +
            std::to_string(static_cast<unsigned int>(result.error().kind)) +
            " at " + result.error().path + ": " + result.error().message);
    }
    return std::move(result.error());
}

void happy_path_preserves_order_and_locations() {
    auto first = parse_text(complete_manifest());
    auto second = parse_text(complete_manifest());
    CHECK(first);
    CHECK(second);
    CHECK(*first == *second);

    CHECK(first->location.line == 1U);
    CHECK(first->location.column == 1U);
    CHECK(first->api_version.location.line == 1U);
    CHECK(first->api_version.location.column == 13U);
    CHECK(first->kind.location.line == 2U);
    CHECK(first->kind.location.column == 7U);
    CHECK(first->artifacts.size() == 2U);
    CHECK(first->artifacts[0].id.value == "system");
    CHECK(first->artifacts[0].id.location.line == 4U);
    CHECK(first->artifacts[0].id.location.column == 9U);
    CHECK(first->artifacts[1].id.value == "boot");
    CHECK(first->artifacts[0].sha256.value == std::string(64U, 'a'));
    CHECK(first->targets.size() == 2U);
    CHECK(first->targets[0].name.value == "product-a");
    CHECK(first->targets[0].name.location.line == 11U);
    CHECK(first->targets[0].name.location.column == 11U);
    CHECK(first->targets[1].name.value == "product-b");
    CHECK(first->targets[0].selector.serials[0].value == "SERIAL-B");
    CHECK(first->targets[0].selector.serials[1].value == "SERIAL-A");
    CHECK(first->targets[0].steps.size() == 5U);
    CHECK(first->targets[1].steps.size() == 2U);

    const auto& flash =
        std::get<ManifestFlashStep>(first->targets[0].steps[0].payload);
    CHECK(flash.partition.value == "system");
    CHECK(flash.artifact.value == "system");
    CHECK(flash.slot == ManifestFlashSlot::Other);
    const auto& active =
        std::get<ManifestSetActiveStep>(first->targets[0].steps[2].payload);
    CHECK(active.slot == ManifestActiveSlot::B);
    const auto& reboot =
        std::get<ManifestRebootStep>(first->targets[0].steps[3].payload);
    CHECK(reboot.target == ManifestRebootTarget::Fastboot);
    CHECK(reboot.target_location.has_value());
    const auto& oem =
        std::get<ManifestOemStep>(first->targets[0].steps[4].payload);
    CHECK(oem.command.value == "diagnostic-mode");
    const auto& default_reboot =
        std::get<ManifestRebootStep>(first->targets[1].steps[1].payload);
    CHECK(default_reboot.target == ManifestRebootTarget::System);
    CHECK(!default_reboot.target_location.has_value());

    CHECK(first->policy.on_device_failure == ManifestDeviceFailurePolicy::Stop);
    CHECK(first->policy.max_parallel_devices == 7U);
    CHECK(!first->policy.memory_budget.automatic);
    CHECK(first->policy.memory_budget.bytes == 1048576U);
    CHECK(first->policy.location.has_value());
}

void source_sha256_binds_the_stable_original_bytes() {
    const auto fixture_path =
        std::filesystem::path{__FILE__}.parent_path().parent_path() /
        "contracts" / "fleet-job-v1.fixture.yaml";
    auto first = load_fleet_manifest_file(fixture_path, long_options());
    auto second = load_fleet_manifest_file(fixture_path, long_options());
    CHECK(first);
    CHECK(second);
    CHECK(*first == *second);
    CHECK(first->source_sha256 == second->source_sha256);
    CHECK(sha256_hex(first->source_sha256) ==
          "58539b1d8a0ba3108ffd0f0ea835d25efca9a6ce85b06cd15f0f1307d4b1c9ef");

    std::ifstream input(fixture_path, std::ios::binary);
    CHECK(input.good());
    const std::string fixture{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(!input.bad());
    const auto changed = replace_once(fixture, "SERIAL-02", "SERIAL-03");
    CHECK(changed.size() == fixture.size());
    std::size_t changed_bytes = 0U;
    for (std::size_t index = 0U; index < fixture.size(); ++index) {
        changed_bytes += fixture[index] == changed[index] ? 0U : 1U;
    }
    CHECK(changed_bytes == 1U);

    auto changed_manifest = parse_text(changed);
    CHECK(changed_manifest);
    CHECK(changed_manifest->source_sha256 != first->source_sha256);
}

void defaults_are_owned_and_deterministic() {
    auto parsed = parse_text(replace_once(
        minimal_manifest(),
        "      - flash: {partition: system, artifact: system}\n",
        "      - reboot: {}\n"));
    CHECK(parsed);
    CHECK(parsed->policy.on_device_failure ==
          ManifestDeviceFailurePolicy::Continue);
    CHECK(parsed->policy.max_parallel_devices == 32U);
    CHECK(parsed->policy.memory_budget.automatic);
    CHECK(parsed->policy.memory_budget.bytes == 0U);
    CHECK(!parsed->policy.location.has_value());
    CHECK(!parsed->policy.memory_budget.location.has_value());
    const auto& reboot =
        std::get<ManifestRebootStep>(parsed->targets[0].steps[0].payload);
    CHECK(reboot.target == ManifestRebootTarget::System);
}

void document_and_yaml_graph_security_is_strict() {
    expect_error({}, ManifestErrorKind::Syntax);
    expect_error(minimal_manifest() + "---\n{}\n",
                 ManifestErrorKind::MultipleDocuments);
    CHECK(parse_text(replace_once(minimal_manifest(),
                                  "kind: FlashJob",
                                  "kind: \"FlashJob\"")));
    CHECK(parse_text(replace_once(minimal_manifest(),
                                  "kind: FlashJob",
                                  "kind: 'FlashJob'")));
    expect_error(replace_once(minimal_manifest(),
                              "kind: FlashJob",
                              "kind: ! FlashJob"),
                 ManifestErrorKind::UnsupportedTag);
    expect_error(replace_once(minimal_manifest(),
                              "kind: FlashJob",
                              "kind: ! \"FlashJob\""),
                 ManifestErrorKind::UnsupportedTag);
    expect_error(replace_once(minimal_manifest(),
                              "artifacts:\n",
                              "artifacts: !\n"),
                 ManifestErrorKind::UnsupportedTag);
    expect_error(replace_once(minimal_manifest(),
                              "kind: FlashJob",
                              "kind: !application FlashJob"),
                 ManifestErrorKind::UnsupportedTag);
    expect_error(replace_once(minimal_manifest(),
                              "kind: FlashJob",
                              "kind: !!str FlashJob"),
                 ManifestErrorKind::UnsupportedTag);
    expect_error(replace_once(minimal_manifest(),
                              "kind: FlashJob",
                              "kind: !!null null"),
                 ManifestErrorKind::UnsupportedTag);
    expect_error(replace_once(minimal_manifest(),
                              "  - id: system",
                              "  - &artifact id: system"),
                 ManifestErrorKind::AliasNotAllowed);
    expect_error(replace_once(
                     minimal_manifest(),
                     "  - id: system\n    path: images/system.img\n"
                     "    sha256: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n",
                     "  - &artifact {id: system, path: images/system.img, "
                     "sha256: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA}\n"
                     "  - *artifact\n"),
                 ManifestErrorKind::AliasNotAllowed);
    expect_error("&root [*root]\n", ManifestErrorKind::AliasNotAllowed);

    auto embedded_nul = minimal_manifest();
    embedded_nul.insert(embedded_nul.find("FlashJob") + 4U, 1U, '\0');
    expect_error(embedded_nul, ManifestErrorKind::InvalidValue);
    for (const auto escape : {"\\0", "\\x00", "\\u0000"}) {
        expect_error(replace_once(
                         minimal_manifest(),
                         "expectedProduct: product_a",
                         std::string("expectedProduct: \"product") + escape +
                             "a\""),
                     ManifestErrorKind::InvalidValue);
    }

    auto invalid_utf8 = minimal_manifest();
    const auto product = invalid_utf8.find("product_a");
    CHECK(product != std::string::npos);
    invalid_utf8[product] = static_cast<char>(0xC0U);
    invalid_utf8[product + 1U] = static_cast<char>(0xAFU);
    expect_error(invalid_utf8, ManifestErrorKind::InvalidUtf8);

    expect_error(replace_once(minimal_manifest(),
                              "kind: FlashJob\n",
                              "kind: FlashJob\nkind: FlashJob\n"),
                 ManifestErrorKind::DuplicateKey);
    expect_error("? [bad]\n: value\n" + minimal_manifest(),
                 ManifestErrorKind::NonScalarKey);
    expect_error(replace_once(minimal_manifest(),
                              "kind: FlashJob\n",
                              "kind: FlashJob\nunknown: value\n"),
                 ManifestErrorKind::UnknownField);
    expect_error(replace_once(minimal_manifest(), "kind: FlashJob\n", {}),
                 ManifestErrorKind::MissingField);
    expect_error(replace_once(
                     minimal_manifest(),
                     "artifacts:\n"
                     "  - id: system\n"
                     "    path: images/system.img\n"
                     "    sha256: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n",
                     "artifacts: {}\n"),
                 ManifestErrorKind::TypeMismatch);
    expect_error("apiVersion: [\n", ManifestErrorKind::Syntax);
}

void yaml_core_string_typing_matches_the_json_schema() {
    const std::vector<std::string> core_non_strings{
        "~",       "null",    "Null",      "NULL",      "true",
        "True",    "TRUE",    "false",     "False",     "FALSE",
        "0",       "-12",     "+12",       "0o17",      "0x2A",
        "+0o17",   "-0o17",   "+0x2A",     "-0x2A",     "1.0",
        ".5",      "1e3",     "-2.5E-2",   ".inf",      "-.Inf",
        ".NaN",
    };
    for (const auto& value : core_non_strings) {
        expect_error(replace_once(minimal_manifest(),
                                  "expectedProduct: product_a",
                                  "expectedProduct: " + value),
                     ManifestErrorKind::TypeMismatch);
        auto quoted = parse_text(replace_once(minimal_manifest(),
                                              "expectedProduct: product_a",
                                              "expectedProduct: \"" + value +
                                                  "\""));
        CHECK(quoted);
        CHECK(quoted->targets[0].expected_product.value == value);
    }

    for (const auto value : {"yes", "no", "on", "off", "123abc"}) {
        auto parsed = parse_text(replace_once(minimal_manifest(),
                                              "expectedProduct: product_a",
                                              std::string("expectedProduct: ") +
                                                  value));
        CHECK(parsed);
        CHECK(parsed->targets[0].expected_product.value == value);
    }
}

void path_hash_and_identity_rules_are_strict() {
    const std::vector<std::string> unsafe_paths{
        "/images/system.img",
        "../system.img",
        "images/../system.img",
        "./system.img",
        ".",
        "..",
        "images//system.img",
        "images/",
        "images\\system.img",
        "C:system.img",
    };
    for (const auto& path : unsafe_paths) {
        expect_error(replace_once(minimal_manifest(),
                                  "images/system.img",
                                  path),
                     ManifestErrorKind::UnsafePath);
    }
    expect_error(replace_once(minimal_manifest(),
                              "path: images/system.img",
                              "path: \"images/\\x01system.img\""),
                 ManifestErrorKind::UnsafePath);
    expect_error(replace_once(minimal_manifest(),
                              std::string(64U, 'A'),
                              std::string(63U, 'a')),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(minimal_manifest(),
                              std::string(64U, 'A'),
                              std::string(63U, 'a') + "g"),
                 ManifestErrorKind::InvalidValue);

    const auto second_artifact =
        "  - id: system\n"
        "    path: images/other.img\n"
        "    sha256: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n";
    expect_error(replace_once(minimal_manifest(),
                              "targets:\n",
                              second_artifact + std::string("targets:\n")),
                 ManifestErrorKind::DuplicateValue);
    const auto duplicate_path =
        "  - id: other\n"
        "    path: images/system.img\n"
        "    sha256: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n";
    expect_error(replace_once(minimal_manifest(),
                              "targets:\n",
                              duplicate_path + std::string("targets:\n")),
                 ManifestErrorKind::DuplicateValue);
}

void schema_fields_selectors_and_references_are_closed() {
    expect_error(replace_once(minimal_manifest(),
                              "  - id: system\n",
                              "  - id: system\n    extra: value\n"),
                 ManifestErrorKind::UnknownField);
    expect_error(replace_once(minimal_manifest(), "    path: images/system.img\n", {}),
                 ManifestErrorKind::MissingField);
    expect_error(replace_once(minimal_manifest(),
                              "  - name: product-a\n",
                              "  - name: product-a\n    extra: value\n"),
                 ManifestErrorKind::UnknownField);
    expect_error(replace_once(minimal_manifest(), "    expectedProduct: product_a\n", {}),
                 ManifestErrorKind::MissingField);
    expect_error(replace_once(minimal_manifest(),
                              "      serials: [SERIAL-A]\n",
                              "      serials: [SERIAL-A, SERIAL-A]\n"),
                 ManifestErrorKind::DuplicateValue);
    expect_error(replace_once(minimal_manifest(),
                              "      serials: [SERIAL-A]\n",
                              "      serials: []\n"),
                 ManifestErrorKind::LimitExceeded);
    expect_error(replace_once(minimal_manifest(),
                              "    selector:\n"
                              "      serials: [SERIAL-A]\n",
                              "    selector: {}\n"),
                 ManifestErrorKind::MissingField);
    expect_error(replace_once(minimal_manifest(),
                              "artifact: system",
                              "artifact: missing"),
                 ManifestErrorKind::UnknownArtifact);

    auto duplicate_target = replace_once(
        minimal_manifest(),
        "  - name: product-a\n",
        "  - name: product-a\n");
    duplicate_target +=
        "  - name: product-a\n"
        "    selector: {serials: [SERIAL-B]}\n"
        "    expectedProduct: product_a\n"
        "    steps: [{reboot: {}}]\n";
    expect_error(duplicate_target, ManifestErrorKind::DuplicateValue);

    auto duplicate_selector = minimal_manifest();
    duplicate_selector +=
        "  - name: product-b\n"
        "    selector: {usbPaths: [SERIAL-A]}\n"
        "    expectedProduct: product_b\n"
        "    steps: [{reboot: {}}]\n";
    expect_error(duplicate_selector, ManifestErrorKind::DuplicateValue);

    std::string too_many = "      serials: [";
    for (std::size_t index = 0U; index < 257U; ++index) {
        if (index != 0U) {
            too_many += ", ";
        }
        too_many += "SERIAL-" + std::to_string(index);
    }
    too_many += "]\n";
    expect_error(replace_once(minimal_manifest(),
                              "      serials: [SERIAL-A]\n",
                              too_many),
                 ManifestErrorKind::LimitExceeded);
}

void action_shapes_enums_and_policy_bounds_are_strict() {
    const auto step = "      - flash: {partition: system, artifact: system}\n";
    expect_error(replace_once(minimal_manifest(), step, "      - unknown: {}\n"),
                 ManifestErrorKind::UnknownField);
    expect_error(replace_once(minimal_manifest(),
                              step,
                              "      - reboot: {}\n        erase: {partition: cache}\n"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(minimal_manifest(),
                              step,
                              "      - flash: {artifact: system}\n"),
                 ManifestErrorKind::MissingField);
    expect_error(replace_once(minimal_manifest(),
                              step,
                              "      - flash: {partition: system, artifact: system, "
                              "extra: x}\n"),
                 ManifestErrorKind::UnknownField);
    expect_error(replace_once(minimal_manifest(),
                              step,
                              "      - flash: {partition: system, artifact: system, "
                              "slot: invalid}\n"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(minimal_manifest(),
                              step,
                              "      - erase: {}\n"),
                 ManifestErrorKind::MissingField);
    expect_error(replace_once(minimal_manifest(),
                              step,
                              "      - setActive: {slot: current}\n"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(minimal_manifest(),
                              step,
                              "      - reboot: {target: invalid}\n"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(minimal_manifest(),
                              step,
                              "      - oem: {}\n"),
                 ManifestErrorKind::MissingField);

    const auto with_policy = minimal_manifest() +
        "policy: {onDeviceFailure: continue, maxParallelDevices: 32, "
        "memoryBudget: auto}\n";
    CHECK(parse_text(with_policy));
    expect_error(replace_once(with_policy, "onDeviceFailure: continue",
                              "onDeviceFailure: retry"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(with_policy, "maxParallelDevices: 32",
                              "maxParallelDevices: 0"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(with_policy, "maxParallelDevices: 32",
                              "maxParallelDevices: 257"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(with_policy, "maxParallelDevices: 32",
                              "maxParallelDevices: 032"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(with_policy, "maxParallelDevices: 32",
                              "maxParallelDevices: \"32\""),
                 ManifestErrorKind::TypeMismatch);
    expect_error(replace_once(with_policy, "memoryBudget: auto",
                              "memoryBudget: 1048575"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(with_policy, "memoryBudget: auto",
                              "memoryBudget: 2147483649"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(with_policy, "memoryBudget: auto",
                              "memoryBudget: invalid"),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(with_policy, "memoryBudget: auto",
                              "memoryBudget: \"1048576\""),
                 ManifestErrorKind::TypeMismatch);
    expect_error(replace_once(with_policy, "memoryBudget: auto",
                              "memoryBudget: auto, extra: value"),
                 ManifestErrorKind::UnknownField);
}

void structural_limits_fail_before_schema_use() {
    std::string deep = "unknown: ";
    for (std::size_t index = 0U; index < 65U; ++index) {
        deep += '[';
    }
    deep += 'x';
    for (std::size_t index = 0U; index < 65U; ++index) {
        deep += ']';
    }
    deep += '\n';
    expect_error(deep, ManifestErrorKind::LimitExceeded);

    auto long_scalar = replace_once(minimal_manifest(),
                                    "expectedProduct: product_a",
                                    "expectedProduct: " +
                                        std::string(4097U, 'x'));
    expect_error(long_scalar, ManifestErrorKind::LimitExceeded);

    auto no_artifacts = replace_once(
        minimal_manifest(),
        "artifacts:\n"
        "  - id: system\n"
        "    path: images/system.img\n"
        "    sha256: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n",
        "artifacts: []\n");
    expect_error(no_artifacts, ManifestErrorKind::LimitExceeded);
    auto no_targets = minimal_manifest().substr(
        0U, minimal_manifest().find("targets:\n"));
    no_targets += "targets: []\n";
    expect_error(no_targets, ManifestErrorKind::LimitExceeded);
    expect_error(replace_once(minimal_manifest(),
                              "    steps:\n"
                              "      - flash: {partition: system, artifact: system}\n",
                              "    steps: []\n"),
                 ManifestErrorKind::LimitExceeded);

    std::string too_many_artifacts = "artifacts: [";
    too_many_artifacts.reserve(70000U);
    for (std::size_t index = 0U; index < 16385U; ++index) {
        if (index != 0U) {
            too_many_artifacts += ',';
        }
        too_many_artifacts += "{}";
    }
    too_many_artifacts += "]\n";
    expect_error(replace_once(
                     minimal_manifest(),
                     "artifacts:\n"
                     "  - id: system\n"
                     "    path: images/system.img\n"
                     "    sha256: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n",
                     too_many_artifacts),
                 ManifestErrorKind::LimitExceeded);

    std::string too_many_targets = "targets: [";
    for (std::size_t index = 0U; index < 257U; ++index) {
        if (index != 0U) {
            too_many_targets += ',';
        }
        too_many_targets += "{}";
    }
    too_many_targets += "]\n";
    const auto targets_begin = minimal_manifest().find("targets:\n");
    CHECK(targets_begin != std::string::npos);
    expect_error(minimal_manifest().substr(0U, targets_begin) + too_many_targets,
                 ManifestErrorKind::LimitExceeded);

    expect_error(replace_once(minimal_manifest(),
                              "id: system",
                              "id: " + std::string(257U, 'i')),
                 ManifestErrorKind::InvalidValue);
    expect_error(replace_once(minimal_manifest(),
                              "name: product-a",
                              "name: " + std::string(257U, 'n')),
                 ManifestErrorKind::InvalidValue);

    std::string too_many_steps;
    too_many_steps.reserve(300000U);
    too_many_steps = "    steps:\n";
    for (std::size_t index = 0U; index < 16385U; ++index) {
        too_many_steps += "      - reboot: {}\n";
    }
    expect_error(replace_once(minimal_manifest(),
                              "    steps:\n"
                              "      - flash: {partition: system, artifact: system}\n",
                              too_many_steps),
                 ManifestErrorKind::LimitExceeded);

    std::string too_many_nodes = "unknown: [";
    too_many_nodes.reserve(400000U);
    for (std::size_t index = 0U; index < 131073U; ++index) {
        if (index != 0U) {
            too_many_nodes += ',';
        }
        too_many_nodes += 'x';
    }
    too_many_nodes += "]\n";
    expect_error(too_many_nodes, ManifestErrorKind::LimitExceeded);
}

void cancellation_deadline_and_exception_boundaries_are_stable() {
    std::stop_source cancelled;
    cancelled.request_stop();
    auto cancelled_options = long_options();
    cancelled_options.cancellation = cancelled.get_token();
    expect_error(minimal_manifest(), ManifestErrorKind::Cancelled,
                 cancelled_options);

    auto expired = long_options();
    expired.deadline = kairosboot::fleet::ManifestClock::now() - 1ns;
    expect_error(minimal_manifest(), ManifestErrorKind::TimedOut, expired);

    struct CancellationContext final {
        std::stop_source source;
    } cancellation_context;
    const auto cancel_hook = [](const ManifestFaultPoint point, void* opaque) {
        if (point == ManifestFaultPoint::EventScan) {
            static_cast<CancellationContext*>(opaque)->source.request_stop();
        }
    };
    auto during_scan = long_options();
    during_scan.cancellation = cancellation_context.source.get_token();
    during_scan.fault_hook = cancel_hook;
    during_scan.fault_context = &cancellation_context;
    expect_error(minimal_manifest(), ManifestErrorKind::Cancelled, during_scan);

    const auto oom_hook = [](const ManifestFaultPoint, void*) {
        throw std::bad_alloc{};
    };
    for (const auto point : {ManifestFaultPoint::InputBuffer,
                             ManifestFaultPoint::EventScan,
                             ManifestFaultPoint::AstConstruction}) {
        struct FaultContext final {
            ManifestFaultPoint target;
        } context{.target = point};
        const auto selective_oom = [](const ManifestFaultPoint current,
                                      void* opaque) {
            if (current == static_cast<FaultContext*>(opaque)->target) {
                throw std::bad_alloc{};
            }
        };
        auto options = long_options();
        options.fault_hook = selective_oom;
        options.fault_context = &context;
        expect_error(minimal_manifest(), ManifestErrorKind::ResourceExhausted,
                     options);
    }
    auto unconditional_oom = long_options();
    unconditional_oom.fault_hook = oom_hook;
    expect_error(minimal_manifest(), ManifestErrorKind::ResourceExhausted,
                 unconditional_oom);

    const auto unexpected_hook = [](const ManifestFaultPoint, void*) {
        throw std::runtime_error("injected");
    };
    auto unexpected = long_options();
    unexpected.fault_hook = unexpected_hook;
    expect_error(minimal_manifest(), ManifestErrorKind::UnexpectedFailure,
                 unexpected);
}

void file_snapshot_and_size_boundaries_are_fail_closed() {
    TemporaryDirectory directory;
    auto missing = load_fleet_manifest_file(directory.path() / "missing.yaml",
                                            long_options());
    CHECK(!missing);
    CHECK(missing.error().kind == ManifestErrorKind::NotFound);

    auto as_directory = load_fleet_manifest_file(directory.path(), long_options());
    CHECK(!as_directory);
    CHECK(as_directory.error().kind == ManifestErrorKind::UnsafePath);

    const auto oversized_path = directory.path() / "oversized.yaml";
    write_bytes(oversized_path,
                std::string(static_cast<std::size_t>(kMaximumManifestBytes) + 1U,
                            'x'));
    auto oversized = load_fleet_manifest_file(oversized_path, long_options());
    CHECK(!oversized);
    CHECK(oversized.error().kind == ManifestErrorKind::TooLarge);

    const auto target = directory.path() / "target.yaml";
    const auto symlink = directory.path() / "link.yaml";
    write_bytes(target, minimal_manifest());
    std::error_code symlink_error;
    std::filesystem::create_symlink(target.filename(), symlink, symlink_error);
    if (!symlink_error) {
        auto linked = load_fleet_manifest_file(symlink, long_options());
        CHECK(!linked);
        CHECK(linked.error().kind == ManifestErrorKind::UnsafePath);
    }

    const auto snapshot_path = directory.path() / "snapshot.yaml";
    const auto moved_path = directory.path() / "snapshot-held.yaml";
    write_bytes(snapshot_path, minimal_manifest());
    auto source = FileImageSource::open(snapshot_path);
    CHECK(source);
    std::filesystem::rename(snapshot_path, moved_path);
    write_bytes(snapshot_path, "not: the original snapshot\n");
    auto snapshotted = parse_fleet_manifest_source(**source, long_options());
    CHECK(snapshotted);
    CHECK(snapshotted->targets[0].name.value == "product-a");

    const auto truncated_path = directory.path() / "truncated.yaml";
    write_bytes(truncated_path, minimal_manifest());
    auto truncated_source = FileImageSource::open(truncated_path);
    CHECK(truncated_source);
    write_bytes(truncated_path, {});
    auto truncated =
        parse_fleet_manifest_source(**truncated_source, long_options());
    CHECK(!truncated);
    CHECK(truncated.error().kind == ManifestErrorKind::Io);

    const auto mutated_path = directory.path() / "mutated.yaml";
    write_bytes(mutated_path, minimal_manifest());
    auto mutated_source = FileImageSource::open(mutated_path);
    CHECK(mutated_source);
    struct MutationContext final {
        std::filesystem::path path;
        std::string replacement;
    } mutation_context{
        .path = mutated_path,
        .replacement = replace_once(minimal_manifest(),
                                    "product_a",
                                    "product_z"),
    };
    const auto mutation_hook = [](const ManifestFaultPoint point, void* opaque) {
        if (point == ManifestFaultPoint::InputBuffer) {
            auto& context = *static_cast<MutationContext*>(opaque);
            write_bytes(context.path, context.replacement);
        }
    };
    auto mutation_options = long_options();
    mutation_options.fault_hook = mutation_hook;
    mutation_options.fault_context = &mutation_context;
    auto mutated =
        parse_fleet_manifest_source(**mutated_source, mutation_options);
    CHECK(!mutated);
    CHECK(mutated.error().kind == ManifestErrorKind::Io);
}

using TestFunction = void (*)();

void run(const std::string_view name, const TestFunction test) {
    try {
        test();
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& exception) {
        std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
        throw;
    }
}

}  // namespace

int main() {
    try {
        run("happy_path_preserves_order_and_locations",
            happy_path_preserves_order_and_locations);
        run("source_sha256_binds_the_stable_original_bytes",
            source_sha256_binds_the_stable_original_bytes);
        run("defaults_are_owned_and_deterministic",
            defaults_are_owned_and_deterministic);
        run("document_and_yaml_graph_security_is_strict",
            document_and_yaml_graph_security_is_strict);
        run("yaml_core_string_typing_matches_the_json_schema",
            yaml_core_string_typing_matches_the_json_schema);
        run("path_hash_and_identity_rules_are_strict",
            path_hash_and_identity_rules_are_strict);
        run("schema_fields_selectors_and_references_are_closed",
            schema_fields_selectors_and_references_are_closed);
        run("action_shapes_enums_and_policy_bounds_are_strict",
            action_shapes_enums_and_policy_bounds_are_strict);
        run("structural_limits_fail_before_schema_use",
            structural_limits_fail_before_schema_use);
        run("cancellation_deadline_and_exception_boundaries_are_stable",
            cancellation_deadline_and_exception_boundaries_are_stable);
        run("file_snapshot_and_size_boundaries_are_fail_closed",
            file_snapshot_and_size_boundaries_are_fail_closed);
    } catch (...) {
        return 1;
    }
    return 0;
}
