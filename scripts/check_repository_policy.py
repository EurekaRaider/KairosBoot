#!/usr/bin/env python3
"""Validate repository invariants that must remain true on every pull request."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FULL_SHA = re.compile(r"^[0-9a-f]{40}$")
VERSION = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def fail(message: str) -> None:
    print(f"policy error: {message}", file=sys.stderr)
    raise SystemExit(1)


def check_codeowners() -> None:
    owners = (ROOT / ".github" / "CODEOWNERS").read_text(encoding="utf-8")
    lines = [line.strip() for line in owners.splitlines() if line.strip()]
    if lines != ["* @EurekaRaider"]:
        fail("CODEOWNERS must make @EurekaRaider the sole global code owner")


def check_version() -> None:
    data = json.loads((ROOT / "version.json").read_text(encoding="utf-8"))
    version = data.get("version")
    if not isinstance(version, str) or VERSION.fullmatch(version) is None:
        fail("version.json must contain a semantic version string")


def check_workflows() -> None:
    workflow_dir = ROOT / ".github" / "workflows"
    if not workflow_dir.exists():
        return

    uses_pattern = re.compile(r"^\s*-?\s*uses:\s*([^\s#]+)", re.MULTILINE)
    for workflow in sorted(workflow_dir.glob("*.y*ml")):
        text = workflow.read_text(encoding="utf-8")
        if "pull_request_target:" in text:
            fail(f"{workflow.relative_to(ROOT)} must not use pull_request_target")
        for value in uses_pattern.findall(text):
            if value.startswith("./") or value.startswith("docker://"):
                continue
            if "@" not in value:
                fail(f"{workflow.relative_to(ROOT)} has an unpinned action: {value}")
            _, ref = value.rsplit("@", 1)
            if FULL_SHA.fullmatch(ref) is None:
                fail(f"{workflow.relative_to(ROOT)} must pin actions by full SHA: {value}")

    release_workflow = workflow_dir / "release.yml"
    if release_workflow.is_file():
        release = release_workflow.read_text(encoding="utf-8")
        if "RelWithDebInfo" in release or "CMAKE_BUILD_TYPE=Debug" in release:
            fail("release workflow must build optimized CMake Release artifacts")
        release_requirements = {
            "single-config CMake Release": "-DCMAKE_BUILD_TYPE=Release",
            "multi-config Release build": "cmake --build build --config Release",
            "multi-config Release test": "ctest --test-dir build -C Release",
            "multi-config Release install": "cmake --install build --config Release",
            "external Release symbols": "-DKAIROSBOOT_RELEASE_SYMBOLS=ON",
            "managed Release package": "/p:Configuration=Release",
            "managed assembly Release version": '/p:Version="$VERSION"',
            "Linux split debug symbols": "objcopy --only-keep-debug",
            "libusb split debug symbols": "libusb-1.0.debug",
            "locked Boost source archive": "boost-1.92.0-cmake.tar.xz",
            "Boost release archive digest": (
                "9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d"
            ),
            "Boost license release asset": "Boost-1.92.0-LICENSE_1_0.txt",
            "Boost SBOM source input": "--boost-source",
            "locked miniz source archive": "miniz-3.1.2.zip",
            "miniz release archive digest": (
                "f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a"
            ),
            "miniz license release asset": "miniz-3.1.2-LICENSE",
            "miniz SBOM source input": "--miniz-source",
            "locked yaml-cpp source archive": "yaml-cpp-0.9.0.tar.gz",
            "yaml-cpp release archive digest": (
                "298593d9c440fd9034b8b193d96318b76d49bc97c6ceadb7b0836edf0b6d7539"
            ),
            "yaml-cpp license release asset": "yaml-cpp-0.9.0-LICENSE",
            "yaml-cpp SBOM source input": "--yaml-cpp-source",
            "macOS split debug symbols": "dsymutil",
            "Windows dedicated symbol staging": "--symbols-root build/symbols/Release",
            "macOS stripped libusb runtime": (
                'strip -S "${RUNNER_TEMP}/install/lib/libusb-1.0.0.dylib"'
            ),
            "clean reused draft assets": "gh release delete-asset",
            "exact published asset set": "diff -u expected-assets.txt actual-assets.txt",
        }
        for contract, marker in release_requirements.items():
            if marker not in release:
                fail(f"release workflow is missing {contract}: {marker}")

        for forbidden in (
            "dotnet nuget push",
            "nuget push",
            "npm publish",
            "cargo publish",
            "twine upload",
        ):
            if forbidden in release.lower():
                fail(f"release workflow must not publish to a registry: {forbidden}")
        for marker in (
            "scripts/check_release_context.py",
            "scripts/check_release_build.py",
            "--write-evidence",
            "--write-checksums",
            "--signing-mode \"$SIGNING_MODE\"",
        ):
            if marker not in release:
                fail(f"release workflow is missing a hard Release gate: {marker}")

        xcode = "/Applications/Xcode_26.3.app/Contents/Developer"
        if xcode not in release:
            fail(f"release workflow must pin the validated macOS Xcode: {xcode}")

    ci_workflow = workflow_dir / "ci.yml"
    if ci_workflow.is_file():
        ci = ci_workflow.read_text(encoding="utf-8")
        xcode = "/Applications/Xcode_26.3.app/Contents/Developer"
        if xcode not in ci:
            fail(f"CI workflow must pin the validated macOS Xcode: {xcode}")
        for stale_path in (
            "libkairosboot.so.0.1.0",
            "libkairosboot.0.1.0.dylib",
        ):
            if stale_path in ci:
                fail(f"CI workflow must not hard-code a development library path: {stale_path}")
        if '"/p:Version=${package_version}"' not in ci:
            fail("CI package build must align the managed assembly and package version")

    policy_workflow = workflow_dir / "policy.yml"
    if policy_workflow.is_file():
        policy = policy_workflow.read_text(encoding="utf-8")
        if "compatibility_inventory_tooling" not in policy:
            fail("Policy must execute the compatibility inventory tooling suite")
        if "ctest --test-dir build --output-on-failure" not in policy:
            fail("Policy compatibility tooling must propagate CTest failures")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    for marker in (
        "NAME compatibility_inventory_tooling",
        "tests/tooling/test_compatibility_inventory.py",
    ):
        if marker not in cmake:
            fail(f"root CTest is missing the compatibility inventory gate: {marker}")


def check_release_distribution_contract() -> None:
    environment = json.loads(
        (ROOT / ".github" / "environments" / "github-release.json").read_text(
            encoding="utf-8"
        )
    )
    expected_environment = {
        "wait_timer": 0,
        "prevent_self_review": False,
        "reviewers": [{"type": "User", "id": 299445952}],
        "deployment_branch_policy": {
            "protected_branches": False,
            "custom_branch_policies": True,
        },
    }
    if environment != expected_environment:
        fail("github-release Environment fixture differs from the reviewed contract")

    deployment_policies = json.loads(
        (
            ROOT
            / ".github"
            / "environments"
            / "github-release-deployment-policies.json"
        ).read_text(encoding="utf-8")
    )
    if deployment_policies != {
        "schemaVersion": 1,
        "policies": [{"name": "v*", "type": "tag"}],
    }:
        fail("github-release must allow deployments only from v* tags")

    main_ruleset = json.loads(
        (ROOT / ".github" / "rulesets" / "main.json").read_text(encoding="utf-8")
    )
    owner_bypass = [
        {
            "actor_id": 299445952,
            "actor_type": "User",
            "bypass_mode": "always",
        }
    ]
    if (
        main_ruleset.get("target") != "branch"
        or main_ruleset.get("enforcement") != "active"
        or main_ruleset.get("bypass_actors") != owner_bypass
        or main_ruleset.get("conditions")
        != {"ref_name": {"include": ["refs/heads/main"], "exclude": []}}
    ):
        fail("main ruleset fixture must protect only main with owner-only bypass")
    main_rules = {
        rule.get("type"): rule for rule in main_ruleset.get("rules", [])
    }
    if set(main_rules) != {
        "deletion",
        "non_fast_forward",
        "required_linear_history",
        "pull_request",
        "required_status_checks",
    }:
        fail("main ruleset fixture is missing a required protection rule")
    pull_request = main_rules["pull_request"].get("parameters", {})
    required_pull_request = {
        "allowed_merge_methods": ["squash", "rebase"],
        "dismiss_stale_reviews_on_push": True,
        "require_code_owner_review": True,
        "require_extra_approval_for_unattributed_changes": True,
        "require_last_push_approval": True,
        "required_approving_review_count": 1,
        "required_review_thread_resolution": True,
    }
    if pull_request != required_pull_request:
        fail("main ruleset pull-request review contract differs from policy")
    status_checks = main_rules["required_status_checks"].get("parameters", {})
    if status_checks != {
        "strict_required_status_checks_policy": True,
        "do_not_enforce_on_create": False,
        "required_status_checks": [
            {"context": "ci-required", "integration_id": 15368},
            {"context": "policy", "integration_id": 15368},
        ],
    }:
        fail("main ruleset must require strict ci-required and policy checks")

    tag_ruleset = json.loads(
        (ROOT / ".github" / "rulesets" / "release-tags.json").read_text(
            encoding="utf-8"
        )
    )
    if (
        tag_ruleset.get("target") != "tag"
        or tag_ruleset.get("enforcement") != "active"
        or tag_ruleset.get("bypass_actors") != owner_bypass
        or tag_ruleset.get("conditions")
        != {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}}
        or {rule.get("type") for rule in tag_ruleset.get("rules", [])}
        != {"creation", "update", "deletion", "non_fast_forward"}
    ):
        fail("release tag ruleset fixture differs from the reviewed contract")

    assets = json.loads(
        (ROOT / "release" / "expected-assets.json").read_text(encoding="utf-8")
    )
    expected_platforms = {
        "linux-arm64": "tar.gz",
        "linux-x64": "tar.gz",
        "macos-arm64": "tar.gz",
        "macos-x64": "tar.gz",
        "windows-arm64": "zip",
        "windows-x64": "zip",
    }
    actual_platforms = {
        platform.get("id"): platform.get("archiveExtension")
        for platform in assets.get("platforms", [])
        if isinstance(platform, dict)
    }
    if assets.get("schemaVersion") != 1 or actual_platforms != expected_platforms:
        fail("Release assets must cover the exact six native target platforms")
    if assets.get("nativeKinds") != ["cli", "sdk", "symbols"]:
        fail("each native Release platform must publish CLI, SDK, and symbols")

    required_assets = {
        "KairosBoot.{version}.nupkg",
        "KairosBoot.{version}.snupkg",
        "KairosBoot.spdx.json",
        "KairosBoot-v{version}-UNSIGNED.txt",
        "KairosBoot-v{version}-source.tar.gz",
        "SHA256SUMS",
        "libusb-1.0.30-COPYING",
        "libusb-1.0.30.tar.bz2",
        "provenance.intoto.json",
        "signing-status.json",
    }
    additional_assets = assets.get("additionalAssets")
    if not isinstance(additional_assets, list) or not required_assets.issubset(
        set(additional_assets)
    ):
        fail("Release asset contract is missing required managed or supply-chain assets")

    managed_project = (
        ROOT / "bindings" / "dotnet" / "KairosBoot" / "KairosBoot.csproj"
    ).read_text(encoding="utf-8")
    managed_contract = {
        "target frameworks": "<TargetFrameworks>net48;net10.0</TargetFrameworks>",
        "net48 Windows x64 RID": "<RuntimeIdentifiers>win-x64</RuntimeIdentifiers>",
        "net10 six RIDs": (
            "<RuntimeIdentifiers>win-x64;win-arm64;linux-x64;linux-arm64;"
            "osx-x64;osx-arm64</RuntimeIdentifiers>"
        ),
    }
    for contract, marker in managed_contract.items():
        if marker not in managed_project:
            fail(f"managed Release contract is missing {contract}: {marker}")


def check_required_files() -> None:
    for name in ("LICENSE", "README.md", "SECURITY.md", "CONTRIBUTING.md"):
        if not (ROOT / name).is_file():
            fail(f"required repository file is missing: {name}")


def check_compatibility_baseline() -> None:
    lock = json.loads((ROOT / "compat" / "aosp.lock.json").read_text(encoding="utf-8"))
    inventory = json.loads(
        (ROOT / "compat" / "generated-inventory.json").read_text(encoding="utf-8")
    )
    update_plan_golden = json.loads(
        (ROOT / "tests" / "compat" / "aosp-update-plan-37.0.1.json").read_text(
            encoding="utf-8"
        )
    )
    if lock.get("baselineStatus") != "locked":
        fail("AOSP compatibility baseline must be locked")
    version = lock.get("aosp", {}).get("platformToolsVersion")
    if inventory.get("baseline", {}).get("platformToolsVersion") != version:
        fail("compatibility inventory and AOSP lock use different versions")
    archives = lock.get("aosp", {}).get("officialArchives", {})
    if set(archives) != {"linux", "windows", "darwin"}:
        fail("AOSP lock must contain all three official host archives")
    for platform, archive in archives.items():
        url = archive.get("url", "")
        if "latest" in url or version not in url:
            fail(f"{platform} Platform-Tools URL is not immutable")
        for field in ("sha256", "fastbootSha256"):
            if SHA256.fullmatch(archive.get(field, "")) is None:
                fail(f"{platform} Platform-Tools {field} is not SHA-256")

    if (
        update_plan_golden.get("documentType")
        != "kairosboot.aosp-update-plan-golden"
        or update_plan_golden.get("schemaVersion") != 1
    ):
        fail("update-plan golden has an unsupported document contract")
    update_oracle = update_plan_golden.get("oracle", {})
    if update_oracle.get("platformToolsVersion") != version:
        fail("update-plan golden and AOSP lock use different versions")
    if update_oracle.get("aospSourceCommit") != lock.get("aosp", {}).get(
        "sourceCommit"
    ):
        fail("update-plan golden and AOSP lock use different source commits")
    if update_oracle.get("binaryCrossCheck", {}).get(
        "darwinFastbootSha256"
    ) != archives["darwin"].get("fastbootSha256"):
        fail("update-plan golden and AOSP lock use different fastboot binaries")

    if inventory.get("schemaVersion") != 2:
        fail("compatibility inventory has an unsupported generated schema")
    if inventory.get("claimCompatibility") is not False:
        fail("compatibility claim must remain false before official differentials")
    allowed_statuses = {
        "implemented",
        "partial",
        "missing",
        "intentional-deviation",
    }
    if set(inventory.get("statusVocabulary", [])) != allowed_statuses:
        fail("compatibility inventory status vocabulary is not frozen")
    entries = inventory.get("entries", [])
    if not isinstance(entries, list) or not entries:
        fail("compatibility inventory must contain generated entries")
    if any(entry.get("status") not in allowed_statuses for entry in entries):
        fail("compatibility inventory contains an unknown status")
    deviation_descriptions = sorted(
        entry.get("note")
        for entry in entries
        if isinstance(entry, dict)
        and entry.get("kind") == "deviation"
        and entry.get("scope") == "command.update"
    )
    if deviation_descriptions != sorted(update_plan_golden.get("intentionalDeviations")):
        fail("update-plan intentional deviations differ from the locked golden")
    generator = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "generate_compatibility_inventory.py"),
            "--repository-root",
            str(ROOT),
            "--check",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if generator.returncode != 0:
        fail(generator.stderr.strip() or generator.stdout.strip())
    libusb = lock.get("libusb", {})
    if libusb.get("requiredVersion") != "1.0.30":
        fail("libusb baseline must remain fixed at 1.0.30")
    if SHA256.fullmatch(libusb.get("sourceArchiveSha256", "")) is None:
        fail("libusb source archive hash is not SHA-256")
    if SHA256.fullmatch(libusb.get("windowsBinaryArchiveSha256", "")) is None:
        fail("libusb Windows archive hash is not SHA-256")
    for field in ("sourceArchive", "windowsBinaryArchive"):
        url = libusb.get(field, "")
        if "v1.0.30" not in url or "1.0.30" not in url or "latest" in url:
            fail(f"libusb {field} URL is not immutable")
    boost = lock.get("boost", {})
    boost_url = (
        "https://github.com/boostorg/boost/releases/download/boost-1.92.0/"
        "boost-1.92.0-cmake.tar.xz"
    )
    boost_sha256 = "9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d"
    expected_boost = {
        "requiredVersion": "1.92.0",
        "sourceTag": "boost-1.92.0",
        "sourceCommit": "afdfa32505af73e3d208144b3f623f0096cb62b6",
        "cmakeArchive": boost_url,
        "cmakeArchiveSha256": boost_sha256,
        "license": "BSL-1.0",
    }
    if boost != expected_boost:
        fail("Boost baseline must exactly match the validated stable 1.92.0 release")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    boost_contract = {
        "FetchContent module": "include(FetchContent)",
        "Boost declaration": "FetchContent_Declare(\n    Boost",
        "Boost population": "FetchContent_MakeAvailable(Boost)",
        "Boost digest verification": 'URL_HASH "SHA256=${KAIROSBOOT_BOOST_SHA256}"',
        "Boost.Asio target": "Boost::asio_core",
        "Boost.Asio Windows 10 API baseline": "_WIN32_WINNT=0x0A00 WINVER=0x0A00",
    }
    for contract, marker in boost_contract.items():
        if marker not in cmake:
            fail(f"root CMake is missing {contract}: {marker}")

    release = (ROOT / ".github" / "workflows" / "release.yml").read_text(
        encoding="utf-8"
    )
    for contract, marker in {
        "locked Boost release URL": boost_url,
        "locked Boost release digest": boost_sha256,
        "Boost license asset": "Boost-1.92.0-LICENSE_1_0.txt",
        "Boost SBOM input": "--boost-source",
    }.items():
        if marker not in release:
            fail(f"release workflow is missing {contract}: {marker}")

    distribution_contracts = {
        "third-party notice": (ROOT / "THIRD_PARTY_NOTICES.md", "Boost 1.92.0"),
        "SPDX package": (ROOT / "scripts" / "generate_sbom.py", '"name": "Boost"'),
        "NuGet Boost license": (
            ROOT / "bindings" / "dotnet" / "KairosBoot" / "KairosBoot.csproj",
            "licenses/boost/LICENSE_1_0.txt",
        ),
        "CI Boost license staging": (
            ROOT / ".github" / "workflows" / "ci.yml",
            "share/kairosboot/boost/LICENSE_1_0.txt",
        ),
    }
    for contract, (path, marker) in distribution_contracts.items():
        if marker not in path.read_text(encoding="utf-8"):
            fail(f"Boost distribution contract is missing {contract}: {marker}")

    miniz = lock.get("miniz", {})
    miniz_url = (
        "https://github.com/richgel999/miniz/releases/download/3.1.2/"
        "miniz-3.1.2.zip"
    )
    miniz_sha256 = "f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a"
    expected_miniz = {
        "requiredVersion": "3.1.2",
        "sourceTag": "3.1.2",
        "sourceCommit": "77d0dce8627735138c51770d1799a1ef48f2117d",
        "sourceArchive": miniz_url,
        "sourceArchiveSha256": miniz_sha256,
        "license": "MIT",
    }
    if miniz != expected_miniz:
        fail("miniz baseline must exactly match the validated stable 3.1.2 release")

    miniz_cmake_contract = {
        "miniz declaration": "FetchContent_Declare(\n  kairosboot_miniz_source",
        "miniz population": "FetchContent_MakeAvailable(kairosboot_miniz_source)",
        "miniz digest verification": 'URL_HASH "SHA256=${KAIROSBOOT_MINIZ_SHA256}"',
        "private static target": "add_library(kairosboot_miniz STATIC",
        "position-independent static code": "PROPERTIES POSITION_INDEPENDENT_CODE ON",
        "hidden C symbols": "C_VISIBILITY_PRESET hidden",
        "archive writing disabled": "MINIZ_NO_ARCHIVE_WRITING_APIS",
        "stdio disabled": "MINIZ_NO_STDIO",
        "zlib names disabled": "MINIZ_NO_ZLIB_COMPATIBLE_NAMES",
        "miniz target linked privately": "PRIVATE kairosboot_libusb kairosboot_miniz",
    }
    for contract, marker in miniz_cmake_contract.items():
        if marker not in cmake:
            fail(f"root CMake is missing {contract}: {marker}")
    if "MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS" in cmake:
        fail("miniz ZIP reader CRC validation must remain enabled")
    if "add_library(kairosboot_miniz SHARED" in cmake:
        fail("miniz must not be built as a shared library")
    if "install(TARGETS kairosboot_miniz" in cmake:
        fail("private miniz target must not enter the installed CMake export")

    for contract, marker in {
        "locked miniz release URL": miniz_url,
        "locked miniz release digest": miniz_sha256,
        "miniz license asset": "miniz-3.1.2-LICENSE",
        "miniz SBOM input": "--miniz-source",
    }.items():
        if marker not in release:
            fail(f"release workflow is missing {contract}: {marker}")

    miniz_distribution_contracts = {
        "third-party notice": (ROOT / "THIRD_PARTY_NOTICES.md", "miniz 3.1.2"),
        "SPDX package": (ROOT / "scripts" / "generate_sbom.py", '"name": "miniz"'),
        "NuGet miniz license": (
            ROOT / "bindings" / "dotnet" / "KairosBoot" / "KairosBoot.csproj",
            "licenses/miniz/LICENSE",
        ),
        "CI miniz license staging": (
            ROOT / ".github" / "workflows" / "ci.yml",
            "share/kairosboot/miniz/LICENSE",
        ),
        "native archive runtime dependency gate": (
            ROOT / "scripts" / "smoke_native_archive.py",
            "forbidden private static runtime dependency",
        ),
    }
    for contract, (path, marker) in miniz_distribution_contracts.items():
        if marker not in path.read_text(encoding="utf-8"):
            fail(f"miniz distribution contract is missing {contract}: {marker}")

    yaml_cpp = lock.get("yamlCpp", {})
    yaml_cpp_url = (
        "https://github.com/jbeder/yaml-cpp/releases/download/yaml-cpp-0.9.0/"
        "yaml-cpp-yaml-cpp-0.9.0.tar.gz"
    )
    yaml_cpp_sha256 = (
        "298593d9c440fd9034b8b193d96318b76d49bc97c6ceadb7b0836edf0b6d7539"
    )
    expected_yaml_cpp = {
        "requiredVersion": "0.9.0",
        "sourceTag": "yaml-cpp-0.9.0",
        "sourceCommit": "56e3bb550c91fd7005566f19c079cb7a503223cf",
        "sourceArchive": yaml_cpp_url,
        "sourceArchiveSha256": yaml_cpp_sha256,
        "license": "MIT",
    }
    if yaml_cpp != expected_yaml_cpp:
        fail("yaml-cpp baseline must exactly match the validated stable 0.9.0 release")

    yaml_cpp_cmake_contract = {
        "yaml-cpp declaration": "FetchContent_Declare(\n  kairosboot_yaml_cpp",
        "yaml-cpp population": "FetchContent_MakeAvailable(kairosboot_yaml_cpp)",
        "yaml-cpp digest verification": (
            'URL_HASH "SHA256=${KAIROSBOOT_YAML_CPP_SHA256}"'
        ),
        "private static configuration": "set(YAML_BUILD_SHARED_LIBS OFF",
        "upstream install disabled": "set(YAML_CPP_INSTALL OFF",
        "position-independent static code": "set(YAML_ENABLE_PIC ON",
        "yaml-cpp target linked privately": "yaml-cpp::yaml-cpp Boost::asio_core",
    }
    for contract, marker in yaml_cpp_cmake_contract.items():
        if marker not in cmake:
            fail(f"root CMake is missing {contract}: {marker}")
    if "install(TARGETS yaml-cpp" in cmake:
        fail("private yaml-cpp target must not enter the installed CMake export")

    for contract, marker in {
        "locked yaml-cpp release URL": yaml_cpp_url,
        "locked yaml-cpp release digest": yaml_cpp_sha256,
        "yaml-cpp license asset": "yaml-cpp-0.9.0-LICENSE",
        "yaml-cpp SBOM input": "--yaml-cpp-source",
    }.items():
        if marker not in release:
            fail(f"release workflow is missing {contract}: {marker}")

    yaml_cpp_distribution_contracts = {
        "third-party notice": (ROOT / "THIRD_PARTY_NOTICES.md", "yaml-cpp 0.9.0"),
        "SPDX package": (
            ROOT / "scripts" / "generate_sbom.py",
            '"name": "yaml-cpp"',
        ),
        "NuGet yaml-cpp license": (
            ROOT / "bindings" / "dotnet" / "KairosBoot" / "KairosBoot.csproj",
            "licenses/yaml-cpp/LICENSE",
        ),
        "CI yaml-cpp license staging": (
            ROOT / ".github" / "workflows" / "ci.yml",
            "share/kairosboot/yaml-cpp/LICENSE",
        ),
        "native archive runtime dependency gate": (
            ROOT / "scripts" / "smoke_native_archive.py",
            "libyaml-cpp",
        ),
    }
    for contract, (path, marker) in yaml_cpp_distribution_contracts.items():
        if marker not in path.read_text(encoding="utf-8"):
            fail(f"yaml-cpp distribution contract is missing {contract}: {marker}")


def main() -> None:
    check_required_files()
    check_codeowners()
    check_version()
    check_workflows()
    check_release_distribution_contract()
    check_compatibility_baseline()
    print("repository policy checks passed")


if __name__ == "__main__":
    main()
