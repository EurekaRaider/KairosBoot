#!/usr/bin/env python3
"""Check the declared C ABI manifest and, optionally, a built shared library."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


ROOT = Path(__file__).resolve().parents[1]
DECLARATION = re.compile(r"KB_API\s+[^;]*?\bKB_CALL\s+(kb_[a-z0-9_]+)\s*\(", re.DOTALL)
WINDOWS_EXPORT_HEADER = re.compile(r"^\s*ordinal\s+hint\s+RVA\s+name\s*$", re.IGNORECASE)
WINDOWS_EXPORT = re.compile(
    r"^\s*\d+\s+[0-9a-f]+\s+[0-9a-f]+\s+(\S+)(?:\s+.*)?$", re.IGNORECASE
)

# The selected tools report only externally visible, defined symbols: ELF
# dynamic definitions, Mach-O external definitions, and PE named exports.
# KairosBoot's hidden visibility and Windows .def file mean no linker-generated
# export is unavoidable today. Keep these allowlists exact; adding a platform
# symbol requires captured tool output demonstrating why it cannot be hidden.
PLATFORM_EXPORT_ALLOWLISTS: dict[str, frozenset[str]] = {
    "darwin": frozenset(),
    "linux": frozenset(),
    "windows": frozenset(),
}


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


def platform_name(platform: str = sys.platform) -> str:
    if platform == "darwin":
        return "darwin"
    if platform.startswith("win"):
        return "windows"
    return "linux"


def run_symbols(
    library: Path, platform: str, dumpbin_path: Optional[Path] = None
) -> str:
    if platform == "darwin":
        command = ["nm", "-gUj", str(library)]
    elif platform == "windows":
        dumpbin = (
            str(dumpbin_path)
            if dumpbin_path is not None
            else shutil.which("dumpbin")
        )
        if dumpbin is None:
            fail("dumpbin.exe or link.exe is required to inspect Windows exports")
        tool_name = Path(dumpbin).name.lower()
        if tool_name in {"link", "link.exe", "lld-link", "lld-link.exe"}:
            command = [dumpbin, "/dump", "/nologo", "/exports", str(library)]
        else:
            command = [dumpbin, "/nologo", "/exports", str(library)]
    else:
        command = ["nm", "-D", "--defined-only", "-j", str(library)]
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        fail(f"symbol tool failed: {completed.stderr.strip()}")
    return completed.stdout


def parse_nm_symbols(output: str, *, strip_macho_prefix: bool) -> set[str]:
    exported = {line.strip() for line in output.splitlines() if line.strip()}
    if strip_macho_prefix:
        return {symbol[1:] if symbol.startswith("_") else symbol for symbol in exported}
    return exported


def parse_dumpbin_symbols(output: str) -> set[str]:
    exported: set[str] = set()
    in_export_table = False
    for line in output.splitlines():
        if WINDOWS_EXPORT_HEADER.fullmatch(line) is not None:
            in_export_table = True
            continue
        if not in_export_table:
            continue
        if line.strip().lower() == "summary":
            break
        match = WINDOWS_EXPORT.fullmatch(line)
        if match is not None:
            exported.add(match.group(1))
    if not in_export_table:
        fail("dumpbin output did not contain an export table")
    return exported


def library_symbols(
    library: Path, platform: str, dumpbin_path: Optional[Path] = None
) -> set[str]:
    output = run_symbols(library, platform, dumpbin_path)
    if platform == "windows":
        return parse_dumpbin_symbols(output)
    return parse_nm_symbols(output, strip_macho_prefix=platform == "darwin")


def check_library_exports(
    library: Path,
    manifest: set[str],
    platform: str,
    dumpbin_path: Optional[Path] = None,
) -> None:
    exported = library_symbols(library, platform, dumpbin_path)
    expected = manifest | set(PLATFORM_EXPORT_ALLOWLISTS[platform])
    if exported != expected:
        fail(
            "shared-library exports and ABI manifest differ; "
            f"missing={sorted(expected - exported)}, extra={sorted(exported - expected)}"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path)
    parser.add_argument("--dumpbin", type=Path)
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

    if args.dumpbin is not None and args.library is None:
        fail("--dumpbin requires --library")
    if args.library is not None:
        platform = platform_name()
        if args.dumpbin is not None and platform != "windows":
            fail("--dumpbin is valid only on Windows")
        if args.dumpbin is not None and not args.dumpbin.is_file():
            fail(f"dumpbin does not exist: {args.dumpbin}")
        check_library_exports(args.library, manifest, platform, args.dumpbin)
    print(f"ABI check passed ({len(manifest)} kb_* symbols)")


if __name__ == "__main__":
    main()
