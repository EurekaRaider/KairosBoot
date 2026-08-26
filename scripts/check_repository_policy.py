#!/usr/bin/env python3
"""Validate repository invariants that must remain true on every pull request."""

from __future__ import annotations

import json
import re
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
            "macOS split debug symbols": "dsymutil",
            "Windows dedicated symbol staging": "--symbols-root build/symbols/Release",
            "macOS stripped libusb runtime": (
                'strip -S "${RUNNER_TEMP}/install/lib/libusb-1.0.0.dylib"'
            ),
        }
        for contract, marker in release_requirements.items():
            if marker not in release:
                fail(f"release workflow is missing {contract}: {marker}")

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


def check_required_files() -> None:
    for name in ("LICENSE", "README.md", "SECURITY.md", "CONTRIBUTING.md"):
        if not (ROOT / name).is_file():
            fail(f"required repository file is missing: {name}")


def check_compatibility_baseline() -> None:
    lock = json.loads((ROOT / "compat" / "aosp.lock.json").read_text(encoding="utf-8"))
    inventory = json.loads(
        (ROOT / "compat" / "generated-inventory.json").read_text(encoding="utf-8")
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


def main() -> None:
    check_required_files()
    check_codeowners()
    check_version()
    check_workflows()
    check_compatibility_baseline()
    print("repository policy checks passed")


if __name__ == "__main__":
    main()
