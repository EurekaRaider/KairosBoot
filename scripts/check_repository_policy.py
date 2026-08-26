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


def check_required_files() -> None:
    for name in ("LICENSE", "README.md", "SECURITY.md", "CONTRIBUTING.md"):
        if not (ROOT / name).is_file():
            fail(f"required repository file is missing: {name}")


def main() -> None:
    check_required_files()
    check_codeowners()
    check_version()
    check_workflows()
    print("repository policy checks passed")


if __name__ == "__main__":
    main()
