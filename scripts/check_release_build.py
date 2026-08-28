#!/usr/bin/env python3
"""Fail closed unless a native install tree came from an optimized Release build."""

from __future__ import annotations

import argparse
import json
import re
import shlex
from pathlib import Path


PLATFORMS = {
    "linux-x64": ("-O3", "-DNDEBUG"),
    "linux-arm64": ("-O3", "-DNDEBUG"),
    "macos-x64": ("-O3", "-DNDEBUG"),
    "macos-arm64": ("-O3", "-DNDEBUG"),
    "windows-x64": ("/O2", "/DNDEBUG"),
    "windows-arm64": ("/O2", "/DNDEBUG"),
}


def fail(message: str) -> None:
    raise SystemExit(f"Release build gate failed: {message}")


def read_cache(path: Path) -> dict[str, str]:
    if not path.is_file():
        fail(f"CMake cache does not exist: {path}")
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="strict").splitlines():
        match = re.fullmatch(r"([^/#][^:=]*):[^=]*=(.*)", line)
        if match is not None:
            values[match.group(1)] = match.group(2)
    return values


def require_release_flags(cache: dict[str, str], platform: str) -> dict[str, str]:
    expected = PLATFORMS[platform]
    actual: dict[str, str] = {}
    for language in ("C", "CXX"):
        key = f"CMAKE_{language}_FLAGS_RELEASE"
        flags = cache.get(key, "")
        if not flags:
            fail(f"{key} is missing or empty")
        windows = platform.startswith("windows-")
        tokens = shlex.split(flags, posix=not windows)
        comparable = {token.upper() if windows else token for token in tokens}
        missing = [
            flag
            for flag in expected
            if (flag.upper() if windows else flag) not in comparable
        ]
        if missing:
            fail(f"{key} lacks required flags {missing}: {flags!r}")
        forbidden = (
            {"/OD", "/UNDEBUG"}
            if windows
            else {"-O0", "-O1", "-O2", "-OG", "-UNDEBUG"}
        )
        conflicts = sorted(comparable & forbidden)
        if conflicts:
            fail(f"{key} contains conflicting flags {conflicts}: {flags!r}")
        actual[key] = flags
    return actual


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--write-evidence", action="store_true")
    args = parser.parse_args()

    if args.configuration != "Release":
        fail(f"configuration must be Release, got {args.configuration!r}")
    cache = read_cache(args.cache.resolve())
    configured_type = cache.get("CMAKE_BUILD_TYPE", "")
    if configured_type and configured_type != "Release":
        fail(f"CMAKE_BUILD_TYPE must be Release, got {configured_type!r}")
    if not args.platform.startswith("windows-") and configured_type != "Release":
        fail("single-config native builds require CMAKE_BUILD_TYPE=Release")

    flags = require_release_flags(cache, args.platform)
    install_root = args.install_root.resolve()
    for directory in ("bin", "include", "lib", "share"):
        if not (install_root / directory).is_dir():
            fail(f"installed SDK is missing {directory}/")

    evidence = {
        "documentType": "kairosboot.release-build",
        "schemaVersion": 1,
        "platform": args.platform,
        "configuration": "Release",
        "optimization": "O2" if args.platform.startswith("windows-") else "O3",
        "ndebug": True,
        "cmakeFlags": flags,
    }
    evidence_path = install_root / "share" / "kairosboot" / "release-build.json"
    if args.write_evidence:
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    print(f"Release build gate passed: {args.platform}")


if __name__ == "__main__":
    main()
