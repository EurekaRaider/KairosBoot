// SPDX-License-Identifier: MIT
#include "src/fleet/artifact_preflight.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kairosboot::fleet::ArtifactPreflightErrorKind;
using kairosboot::fleet::ArtifactPreflightFaultPoint;
using kairosboot::fleet::ArtifactPreflightOptions;
using kairosboot::fleet::FlashJobManifest;
using kairosboot::fleet::JobPlan;
using kairosboot::fleet::LocatedManifestString;
using kairosboot::fleet::ManifestArtifact;
using kairosboot::fleet::ManifestFlashStep;
using kairosboot::fleet::ManifestPolicy;
using kairosboot::fleet::ManifestSelector;
using kairosboot::fleet::ManifestSourceLocation;
using kairosboot::fleet::ManifestStep;
using kairosboot::fleet::ManifestTarget;
using kairosboot::fleet::PreparedArtifactSource;
using kairosboot::fleet::PreparedFleetArtifact;
using kairosboot::fleet::PreparedFleetArtifacts;
using kairosboot::fleet::make_job_plan;
using kairosboot::fleet::preflight_fleet_artifacts;
using kairosboot::image::ArtifactSourceErrorKind;
using kairosboot::image::Sha256Accumulator;
using kairosboot::image::Sha256Digest;
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

static_assert(!std::is_default_constructible_v<PreparedFleetArtifacts>);
static_assert(!std::is_copy_constructible_v<PreparedFleetArtifacts>);
static_assert(!std::is_copy_assignable_v<PreparedFleetArtifacts>);
static_assert(std::is_nothrow_move_constructible_v<PreparedFleetArtifacts>);
static_assert(std::is_nothrow_move_assignable_v<PreparedFleetArtifacts>);
static_assert(!std::is_default_constructible_v<PreparedFleetArtifact>);
static_assert(!std::is_copy_constructible_v<PreparedFleetArtifact>);
static_assert(!std::is_constructible_v<PreparedArtifactSource>);

inline constexpr ManifestSourceLocation kLocation{1U, 1U};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> serial{0U};
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(serial.fetch_add(1U, std::memory_order_relaxed));
        path_ = std::filesystem::temp_directory_path() /
            ("kairosboot-fleet-preflight-" + suffix);
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

[[nodiscard]] LocatedManifestString located(std::string value) {
    return {.value = std::move(value), .location = kLocation};
}

[[nodiscard]] std::string digest_for(const std::string_view bytes) {
    Sha256Accumulator accumulator;
    accumulator.update(std::as_bytes(std::span(bytes.data(), bytes.size())));
    return sha256_hex(accumulator.finish());
}

void write_bytes(const std::filesystem::path& path,
                 const std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(output.good());
}

[[nodiscard]] ManifestArtifact artifact(std::string id,
                                        std::string path,
                                        std::string digest) {
    return {
        .location = kLocation,
        .id = located(std::move(id)),
        .path = located(std::move(path)),
        .sha256 = located(std::move(digest)),
    };
}

[[nodiscard]] ManifestStep flash(std::string artifact_id) {
    return {
        .location = kLocation,
        .payload = ManifestFlashStep{
            .partition = located("system"),
            .artifact = located(std::move(artifact_id)),
            .slot = std::nullopt,
            .slot_location = std::nullopt,
        },
    };
}

[[nodiscard]] JobPlan plan_for(std::vector<ManifestArtifact> artifacts,
                               std::vector<ManifestStep> steps = {}) {
    if (steps.empty()) {
        steps.push_back(flash(artifacts.front().id.value));
    }
    FlashJobManifest manifest{
        .location = kLocation,
        .api_version = located("kairosboot.io/v1"),
        .kind = located("FlashJob"),
        .source_sha256 = Sha256Digest{},
        .artifacts = std::move(artifacts),
        .targets = {
            ManifestTarget{
                .location = kLocation,
                .name = located("fixture"),
                .selector = ManifestSelector{
                    .location = kLocation,
                    .serials = {located("SERIAL")},
                    .usb_paths = {},
                },
                .expected_product = located("fixture_product"),
                .steps = std::move(steps),
            },
        },
        .policy = ManifestPolicy{},
    };
    auto plan = make_job_plan(std::move(manifest));
    CHECK(plan.has_value());
    return std::move(*plan);
}

[[nodiscard]] std::string read_prepared(
    const PreparedFleetArtifact& artifact) {
    const auto& source = artifact.source()->resolved()->source;
    std::string bytes(static_cast<std::size_t>(source->size()), '\0');
    std::size_t completed = 0U;
    while (completed < bytes.size()) {
        auto read = source->read_at(
            static_cast<std::uint64_t>(completed),
            std::as_writable_bytes(std::span(bytes)).subspan(completed));
        CHECK(read.has_value());
        CHECK(*read > 0U);
        completed += *read;
    }
    return bytes;
}

void immutable_snapshot_survives_in_place_and_rename_replacement() {
    TemporaryDirectory temporary;
    const std::string original = "old-verified-payload";
    const std::string in_place(original.size(), 'x');
    const std::string replacement(original.size(), 'y');
    const auto path = temporary.path() / "images/system.img";
    write_bytes(path, original);

    auto plan = plan_for(
        {artifact("system", "images/system.img", digest_for(original))},
        {flash("system"), flash("system")});
    auto prepared = preflight_fleet_artifacts(plan, temporary.path());
    CHECK(prepared.has_value());
    CHECK(prepared->size() == 1U);
    CHECK(prepared->at(0U).index() == 0U);
    CHECK(prepared->find("system") == &prepared->at(0U));
    CHECK(prepared->find("absent") == nullptr);
    CHECK(read_prepared(prepared->at(0U)) == original);

    write_bytes(path, in_place);
    CHECK(read_prepared(prepared->at(0U)) == original);

    const auto moved = temporary.path() / "images/old.img";
    const auto candidate = temporary.path() / "images/replacement.img";
    write_bytes(candidate, replacement);
    std::filesystem::rename(path, moved);
    std::filesystem::rename(candidate, path);
    CHECK(read_prepared(prepared->at(0U)) == original);
}

void order_lookup_and_sources_are_shared_by_verified_digest() {
    TemporaryDirectory temporary;
    const std::string bytes = "identical verified bytes";
    write_bytes(temporary.path() / "a.img", bytes);
    write_bytes(temporary.path() / "nested/b.img", bytes);
    const auto digest = digest_for(bytes);
    auto plan = plan_for({artifact("first", "a.img", digest),
                          artifact("second", "nested/b.img", digest)});

    auto prepared = preflight_fleet_artifacts(plan, temporary.path());
    CHECK(prepared.has_value());
    CHECK(prepared->size() == 2U);
    CHECK(prepared->at(0U).id() == "first");
    CHECK(prepared->at(1U).id() == "second");
    CHECK(prepared->find("second") == &prepared->at(1U));
    CHECK(prepared->at(0U).source() == prepared->at(1U).source());
    CHECK(prepared->at(0U).source()->sha256_hex() == digest);
}

struct FaultContext final {
    ArtifactPreflightFaultPoint point{ArtifactPreflightFaultPoint::BeforePublish};
    std::size_t index{};
    std::size_t calls{};
    bool throw_bad_alloc{};
    std::chrono::milliseconds delay{};
};

void fault_hook(const ArtifactPreflightFaultPoint point,
                const std::size_t index,
                void* opaque) {
    auto& context = *static_cast<FaultContext*>(opaque);
    ++context.calls;
    if (point != context.point || index != context.index) {
        return;
    }
    if (context.delay.count() > 0) {
        std::this_thread::sleep_for(context.delay);
    }
    if (context.throw_bad_alloc) {
        throw std::bad_alloc{};
    }
}

void same_path_same_hash_shares_and_conflicting_hash_fails_before_io() {
    TemporaryDirectory temporary;
    const std::string bytes = "shared physical source";
    write_bytes(temporary.path() / "shared.img", bytes);
    const auto digest = digest_for(bytes);

    auto shared_plan = plan_for({artifact("one", "shared.img", digest),
                                 artifact("two", "shared.img", digest)});
    ArtifactPreflightOptions shared_options;
    shared_options.source_limits.max_spool_bytes = bytes.size();
    auto shared = preflight_fleet_artifacts(
        shared_plan, temporary.path(), shared_options);
    CHECK(shared.has_value());
    CHECK(shared->at(0U).source() == shared->at(1U).source());
    CHECK(shared->at(0U).source()->resolved() ==
          shared->at(1U).source()->resolved());

    auto conflict_plan = plan_for(
        {artifact("one", "shared.img", digest),
         artifact("two", "shared.img", std::string(64U, '0'))});
    FaultContext observer{
        .point = ArtifactPreflightFaultPoint::BeforeArtifactResolve,
        .index = 0U,
    };
    ArtifactPreflightOptions options;
    options.fault_hook = fault_hook;
    options.fault_context = &observer;
    auto conflict =
        preflight_fleet_artifacts(conflict_plan, temporary.path(), options);
    CHECK(!conflict);
    CHECK(conflict.error().kind ==
          ArtifactPreflightErrorKind::ConflictingDeclaredDigest);
    CHECK(conflict.error().artifact_index == 1U);
    CHECK(conflict.error().artifact_id == "two");
    CHECK(observer.calls == 0U);
}

void hash_mismatch_is_closed_and_identifies_the_artifact() {
    TemporaryDirectory temporary;
    write_bytes(temporary.path() / "system.img", "actual bytes");
    auto plan = plan_for(
        {artifact("system", "system.img", std::string(64U, '0'))});
    auto result = preflight_fleet_artifacts(plan, temporary.path());
    CHECK(!result);
    CHECK(result.error().kind == ArtifactPreflightErrorKind::HashMismatch);
    CHECK(result.error().artifact_index == 0U);
    CHECK(result.error().artifact_id == "system");
    CHECK(result.error().source_kind == ArtifactSourceErrorKind::Integrity);
    CHECK(result.error().path.filename() == "system.img");
}

void missing_directory_and_unsafe_symlink_fail_closed() {
    TemporaryDirectory temporary;
    auto missing_plan = plan_for(
        {artifact("missing", "missing.img", digest_for("irrelevant"))});
    auto missing = preflight_fleet_artifacts(missing_plan, temporary.path());
    CHECK(!missing);
    CHECK(missing.error().kind == ArtifactPreflightErrorKind::NotFound);

    std::filesystem::create_directories(temporary.path() / "directory.img");
    auto directory_plan = plan_for(
        {artifact("directory", "directory.img", digest_for("irrelevant"))});
    auto directory =
        preflight_fleet_artifacts(directory_plan, temporary.path());
    CHECK(!directory);
    CHECK(directory.error().kind == ArtifactPreflightErrorKind::UnsafePath);
    CHECK(directory.error().source_kind ==
          ArtifactSourceErrorKind::UnsafePath);
    CHECK(directory.error().artifact_index == 0U);

    const auto target = temporary.path() / "target.img";
    const auto link = temporary.path() / "link.img";
    write_bytes(target, "target");
    std::error_code link_error;
    std::filesystem::create_symlink(target, link, link_error);
    if (!link_error) {
        auto link_plan =
            plan_for({artifact("link", "link.img", digest_for("target"))});
        auto linked = preflight_fleet_artifacts(link_plan, temporary.path());
        CHECK(!linked);
        CHECK(linked.error().kind == ArtifactPreflightErrorKind::UnsafePath);
        CHECK(linked.error().source_kind == ArtifactSourceErrorKind::UnsafePath);
    }
}

void root_boundary_rejects_child_escape_and_parent_replacement() {
    TemporaryDirectory temporary;
    const auto root = temporary.path() / "root";
    const auto outside = temporary.path() / "outside";
    std::filesystem::create_directories(root / "images");
    std::filesystem::create_directory(outside);
    write_bytes(root / "images/system.img", "inside");
    write_bytes(outside / "system.img", "outside");

    const auto jump = root / "jump";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, jump, link_error);
    if (link_error) {
        return;
    }

    auto escape_plan = plan_for(
        {artifact("system", "jump/system.img", digest_for("outside"))});
    auto escaped = preflight_fleet_artifacts(escape_plan, root);
    CHECK(!escaped);
    CHECK(escaped.error().kind == ArtifactPreflightErrorKind::UnsafePath);
    CHECK(escaped.error().source_kind == ArtifactSourceErrorKind::UnsafePath);

    const auto root_alias = temporary.path() / "root-alias";
    std::filesystem::create_directory_symlink(root, root_alias, link_error);
    CHECK(!link_error);
    auto alias_plan = plan_for(
        {artifact("system", "images/system.img", digest_for("inside"))});
    auto through_root_alias = preflight_fleet_artifacts(alias_plan, root_alias);
    CHECK(through_root_alias);
    CHECK(read_prepared(through_root_alias->at(0U)) == "inside");

    const auto trusted_anchor = temporary.path() / "trusted-anchor";
    const auto outside_anchor = temporary.path() / "outside-anchor";
    std::filesystem::create_directories(trusted_anchor / "root/images");
    std::filesystem::create_directories(outside_anchor / "root/images");
    write_bytes(trusted_anchor / "root/images/system.img", "inside");
    write_bytes(outside_anchor / "root/images/system.img", "outside");
    ArtifactPreflightOptions ancestor_options;
    ancestor_options.source_limits.package_entry_observer =
        [&](std::string_view) {
            std::filesystem::rename(
                trusted_anchor, temporary.path() / "retained-anchor");
            std::filesystem::create_directory_symlink(
                outside_anchor, trusted_anchor);
        };
    auto after_ancestor_replacement = preflight_fleet_artifacts(
        alias_plan, trusted_anchor / "root", ancestor_options);
    CHECK(after_ancestor_replacement);
    CHECK(read_prepared(after_ancestor_replacement->at(0U)) == "inside");

    const auto original_parent = root / "images-original";
    ArtifactPreflightOptions race_options;
    race_options.source_limits.package_entry_observer = [&](std::string_view) {
        std::filesystem::rename(root / "images", original_parent);
        std::filesystem::create_directory_symlink(outside, root / "images");
    };
    auto race_plan = plan_for(
        {artifact("system", "images/system.img", digest_for("outside"))});
    auto replaced =
        preflight_fleet_artifacts(race_plan, root, race_options);
    CHECK(!replaced);
    CHECK(replaced.error().kind == ArtifactPreflightErrorKind::UnsafePath);
}

void cancellation_wins_and_absolute_deadline_stops_resolution() {
    TemporaryDirectory temporary;
    const std::string bytes = "deadline fixture";
    write_bytes(temporary.path() / "system.img", bytes);
    auto plan = plan_for(
        {artifact("system", "system.img", digest_for(bytes))});

    std::stop_source cancellation;
    cancellation.request_stop();
    ArtifactPreflightOptions cancelled_options;
    cancelled_options.cancellation = cancellation.get_token();
    cancelled_options.deadline =
        kairosboot::fleet::ArtifactPreflightClock::now() - 1ms;
    auto cancelled = preflight_fleet_artifacts(
        plan, temporary.path(), cancelled_options);
    CHECK(!cancelled);
    CHECK(cancelled.error().kind == ArtifactPreflightErrorKind::Cancelled);

    FaultContext slow{
        .point = ArtifactPreflightFaultPoint::BeforeArtifactResolve,
        .index = 0U,
        .delay = 40ms,
    };
    ArtifactPreflightOptions deadline_options;
    deadline_options.deadline =
        kairosboot::fleet::ArtifactPreflightClock::now() + 5ms;
    deadline_options.fault_hook = fault_hook;
    deadline_options.fault_context = &slow;
    auto timed_out =
        preflight_fleet_artifacts(plan, temporary.path(), deadline_options);
    CHECK(!timed_out);
    CHECK(timed_out.error().kind == ArtifactPreflightErrorKind::TimedOut);
}

void deterministic_error_order_and_no_partial_publication() {
    TemporaryDirectory temporary;
    auto plan = plan_for(
        {artifact("first", "first.img", digest_for("first")),
         artifact("second", "second.img", digest_for("second"))});
    for (std::size_t attempt = 0U; attempt < 3U; ++attempt) {
        auto result = preflight_fleet_artifacts(plan, temporary.path());
        CHECK(!result);
        CHECK(result.error().kind == ArtifactPreflightErrorKind::NotFound);
        CHECK(result.error().artifact_index == 0U);
        CHECK(result.error().artifact_id == "first");
    }

    write_bytes(temporary.path() / "first.img", "first");
    write_bytes(temporary.path() / "second.img", "second");
    FaultContext failure{
        .point = ArtifactPreflightFaultPoint::BeforePublish,
        .index = 1U,
        .throw_bad_alloc = true,
    };
    ArtifactPreflightOptions options;
    options.fault_hook = fault_hook;
    options.fault_context = &failure;
    auto unpublished = preflight_fleet_artifacts(plan, temporary.path(), options);
    CHECK(!unpublished);
    CHECK(unpublished.error().kind ==
          ArtifactPreflightErrorKind::ResourceExhausted);
    CHECK(unpublished.error().artifact_index == 1U);
}

void invalid_sparse_image_and_spool_budget_are_rejected() {
    TemporaryDirectory temporary;
    const std::string invalid_sparse{"\x3a\xff\x26\xed", 4U};
    write_bytes(temporary.path() / "invalid-sparse.img", invalid_sparse);
    auto sparse_plan = plan_for({artifact("sparse",
                                          "invalid-sparse.img",
                                          digest_for(invalid_sparse))});
    auto sparse = preflight_fleet_artifacts(sparse_plan, temporary.path());
    CHECK(!sparse);
    CHECK(sparse.error().kind == ArtifactPreflightErrorKind::InvalidImage);

    const std::string oversized = "larger than budget";
    write_bytes(temporary.path() / "oversized.img", oversized);
    auto budget_plan = plan_for(
        {artifact("oversized", "oversized.img", digest_for(oversized))});
    ArtifactPreflightOptions options;
    options.source_limits.max_spool_bytes = oversized.size() - 1U;
    auto budget =
        preflight_fleet_artifacts(budget_plan, temporary.path(), options);
    CHECK(!budget);
    CHECK(budget.error().kind == ArtifactPreflightErrorKind::LimitExceeded);
}

}  // namespace

int main() {
    using Test = std::pair<std::string_view, void (*)()>;
    constexpr std::array<Test, 9U> tests{{
        {"immutable snapshot survives source replacement",
         &immutable_snapshot_survives_in_place_and_rename_replacement},
        {"order, lookup, and verified digest sharing",
         &order_lookup_and_sources_are_shared_by_verified_digest},
        {"same path sharing and conflicting digest",
         &same_path_same_hash_shares_and_conflicting_hash_fails_before_io},
        {"hash mismatch context",
         &hash_mismatch_is_closed_and_identifies_the_artifact},
        {"missing, directory, and symlink",
         &missing_directory_and_unsafe_symlink_fail_closed},
        {"root boundary confinement",
         &root_boundary_rejects_child_escape_and_parent_replacement},
        {"cancellation and deadline",
         &cancellation_wins_and_absolute_deadline_stops_resolution},
        {"deterministic failure and atomic publication",
         &deterministic_error_order_and_no_partial_publication},
        {"invalid image and spool budget",
         &invalid_sparse_image_and_spool_budget_are_rejected},
    }};

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
