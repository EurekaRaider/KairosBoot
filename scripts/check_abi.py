#!/usr/bin/env python3
"""Check the declared C ABI manifest and, optionally, a built shared library."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DECLARATION = re.compile(r"KB_API\s+[^;]*?\bKB_CALL\s+(kb_[a-z0-9_]+)\s*\(", re.DOTALL)
SYMBOL = re.compile(r"_?(kb_[a-z0-9_]+)$")


def fail(message: str) -> None:
    print(f"ABI error: {message}", file=sys.stderr)
    raise SystemExit(1)


def manifest_symbols() -> set[str]:
    lines = (ROOT / "abi" / "kairosboot.exports").read_text(encoding="utf-8").splitlines()
    symbols = [line.strip() for line in lines if line.strip() and not line.startswith("#")]
    if symbols != sorted(set(symbols)):
        fail("abi/kairosboot.exports must be sorted and contain no duplicates")
    return set(symbols)


def header_symbols() -> set[str]:
    header = (ROOT / "include" / "kairosboot" / "kairosboot.h").read_text(encoding="utf-8")
    return set(DECLARATION.findall(header))


def windows_definition_symbols() -> set[str]:
    lines = (ROOT / "abi" / "kairosboot.def").read_text(encoding="utf-8").splitlines()
    return {line.strip() for line in lines if line.strip() and line.strip() != "EXPORTS"}


def run_symbols(library: Path) -> str:
    if sys.platform == "darwin":
        command = ["nm", "-gU", str(library)]
    elif sys.platform.startswith("win"):
        dumpbin = shutil.which("dumpbin")
        if dumpbin is None:
            fail("dumpbin is required to inspect Windows exports")
        command = [dumpbin, "/nologo", "/exports", str(library)]
    else:
        command = ["nm", "-D", "--defined-only", str(library)]
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        fail(f"symbol tool failed: {completed.stderr.strip()}")
    return completed.stdout


def library_symbols(library: Path) -> set[str]:
    exported: set[str] = set()
    for line in run_symbols(library).splitlines():
        match = SYMBOL.search(line.strip())
        if match is not None:
            exported.add(match.group(1))
    return exported


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path)
    args = parser.parse_args()

    manifest = manifest_symbols()
    declared = header_symbols()
    if declared != manifest:
        fail(
            "public header and ABI manifest differ; "
            f"missing={sorted(declared - manifest)}, stale={sorted(manifest - declared)}"
        )
    definitions = windows_definition_symbols()
    if definitions != manifest:
        fail(
            "Windows module definition and ABI manifest differ; "
            f"missing={sorted(manifest - definitions)}, extra={sorted(definitions - manifest)}"
        )

    if args.library is not None:
        exported = library_symbols(args.library)
        if exported != manifest:
            fail(
                "shared-library exports and ABI manifest differ; "
                f"missing={sorted(manifest - exported)}, extra={sorted(exported - manifest)}"
            )
    print(f"ABI check passed ({len(manifest)} kb_* symbols)")


if __name__ == "__main__":
    main()
