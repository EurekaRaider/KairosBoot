#!/usr/bin/env python3
"""Check the declared C ABI manifest and, optionally, a built shared library."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


ROOT = Path(__file__).resolve().parents[1]
ABI_MANIFEST = ROOT / "abi" / "kairosboot-v1.json"
ABI_LAYOUT_CHECKS = ROOT / "abi" / "kairosboot-layout-v1.h"
DECLARATION = re.compile(r"KB_API\s+[^;]*?\bKB_CALL\s+(kb_[a-z0-9_]+)\s*\(", re.DOTALL)
CONCRETE_STRUCT = re.compile(
    r"typedef\s+struct\s+kb_[a-z0-9_]+\s*\{(?P<body>.*?)\}\s*"
    r"(?P<name>kb_[a-z0-9_]+_t)\s*;",
    re.DOTALL,
)
FIELD = re.compile(r"\b([a-z][a-z0-9_]*)\s*;")
INT32_TYPEDEF = re.compile(r"typedef\s+int32_t\s+(kb_[a-z0-9_]+_t)\s*;")
ENUM_CONSTANT = re.compile(r"^\s*(KB_[A-Z0-9_]+)\s*=", re.MULTILINE)
SPECIAL_CONSTANT = re.compile(
    r"^#define\s+(KB_API_VERSION|KB_WAIT_INFINITE|KB_FETCH_UNSPECIFIED)\b",
    re.MULTILINE,
)
ABI_NAME = re.compile(r"^kb_[a-z0-9_]+$")
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


def public_header() -> str:
    return (ROOT / "include" / "kairosboot" / "kairosboot.h").read_text(
        encoding="utf-8"
    )


def abi_manifest() -> dict[str, object]:
    raw = ABI_MANIFEST.read_text(encoding="utf-8")
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as error:
        fail(f"{ABI_MANIFEST.relative_to(ROOT)} is invalid JSON: {error}")
    if not isinstance(data, dict):
        fail("ABI manifest root must be an object")
    canonical = json.dumps(data, indent=2, sort_keys=True) + "\n"
    if raw != canonical:
        fail("abi/kairosboot-v1.json must use canonical sorted JSON formatting")

    expected_keys = {
        "abiVersion",
        "callingConvention",
        "constants",
        "dataModel",
        "headerContractSha256",
        "schemaVersion",
        "structs",
        "symbols",
        "typedefs",
    }
    if set(data) != expected_keys:
        fail(
            "ABI manifest fields differ; "
            f"missing={sorted(expected_keys - set(data))}, "
            f"extra={sorted(set(data) - expected_keys)}"
        )
    if data["schemaVersion"] != 1 or data["abiVersion"] != 1:
        fail("ABI manifest schemaVersion and abiVersion must remain 1")
    if data["callingConvention"] != {
        "nonWindows": "platform-default",
        "windows": "cdecl",
    }:
        fail("ABI manifest calling conventions differ from the public header")
    if data["dataModel"] != {
        "pointerSize": 8,
        "sizeTSize": 8,
        "supportedArchitectures": ["arm64", "x64"],
    }:
        fail("ABI v1 is frozen for the supported 64-bit x64/arm64 data model")
    header_without_comments = re.sub(
        r"/\*.*?\*/", "", public_header(), flags=re.DOTALL
    )
    header_without_comments = re.sub(r"//[^\n]*", "", header_without_comments)
    header_tokens = re.findall(
        r"[A-Za-z_][A-Za-z0-9_]*|0[xX][0-9A-Fa-f]+|[0-9]+|[^\s]",
        header_without_comments,
    )
    header_digest = hashlib.sha256(" ".join(header_tokens).encode("utf-8")).hexdigest()
    if data["headerContractSha256"] != header_digest:
        fail(
            "public C header contract changed; review the ABI and update "
            "headerContractSha256 explicitly"
        )

    symbols = data["symbols"]
    if (
        not isinstance(symbols, list)
        or any(not isinstance(symbol, str) for symbol in symbols)
        or symbols != sorted(set(symbols))
        or any(ABI_NAME.fullmatch(symbol) is None for symbol in symbols)
    ):
        fail("ABI manifest symbols must be sorted, unique kb_* names")

    constants = data["constants"]
    if not isinstance(constants, dict) or any(
        not isinstance(name, str)
        or not name.startswith("KB_")
        or isinstance(value, bool)
        or not isinstance(value, int)
        or value < 0
        for name, value in constants.items()
    ):
        fail("ABI manifest constants must map KB_* names to non-negative integers")

    typedefs = data["typedefs"]
    if not isinstance(typedefs, dict):
        fail("ABI manifest typedefs must be an object")
    for name, layout in typedefs.items():
        if not isinstance(name, str) or not isinstance(layout, dict):
            fail("ABI manifest typedef entries must be named objects")
        if layout != {"alignment": 4, "size": 4}:
            fail(f"ABI typedef {name} must remain a four-byte int32_t")

    structs = data["structs"]
    if not isinstance(structs, dict):
        fail("ABI manifest structs must be an object")
    for name, layout in structs.items():
        if not isinstance(name, str) or not isinstance(layout, dict):
            fail("ABI manifest struct entries must be named objects")
        if set(layout) not in (
            {"alignment", "fields", "size"},
            {"alignment", "fields", "size", "versionSize"},
        ):
            fail(f"ABI struct {name} has invalid layout fields")
        alignment = layout["alignment"]
        size = layout["size"]
        fields = layout["fields"]
        if (
            isinstance(alignment, bool)
            or not isinstance(alignment, int)
            or alignment <= 0
            or isinstance(size, bool)
            or not isinstance(size, int)
            or size <= 0
            or not isinstance(fields, list)
            or not fields
        ):
            fail(f"ABI struct {name} has invalid size, alignment or fields")
        field_names: list[str] = []
        previous_offset = -1
        for field in fields:
            if not isinstance(field, dict) or set(field) != {"name", "offset", "size"}:
                fail(f"ABI struct {name} has an invalid field entry")
            field_name = field["name"]
            offset = field["offset"]
            field_size = field["size"]
            if (
                not isinstance(field_name, str)
                or isinstance(offset, bool)
                or not isinstance(offset, int)
                or offset < 0
                or isinstance(field_size, bool)
                or not isinstance(field_size, int)
                or field_size <= 0
                or offset < previous_offset
                or offset + field_size > size
            ):
                fail(f"ABI struct {name} has an invalid field layout")
            field_names.append(field_name)
            previous_offset = offset
        if field_names != list(dict.fromkeys(field_names)):
            fail(f"ABI struct {name} contains duplicate fields")
        version_size = layout.get("versionSize")
        if version_size is not None and (
            not isinstance(version_size, dict)
            or set(version_size) != {"macro", "value"}
            or not isinstance(version_size["macro"], str)
            or version_size["value"] != size
        ):
            fail(f"ABI struct {name} has an invalid versionSize")
    return data


def manifest_symbols(manifest: Optional[dict[str, object]] = None) -> set[str]:
    if manifest is None:
        manifest = abi_manifest()
    return set(manifest["symbols"])


def linker_manifest_symbols() -> set[str]:
    lines = (ROOT / "abi" / "kairosboot.exports").read_text(encoding="utf-8").splitlines()
    symbols = [line.strip() for line in lines if line.strip() and not line.startswith("#")]
    if symbols != sorted(set(symbols)):
        fail("abi/kairosboot.exports must be sorted and contain no duplicates")
    return set(symbols)


def header_symbols() -> set[str]:
    return set(DECLARATION.findall(public_header()))


def header_constants() -> set[str]:
    header = public_header()
    return set(ENUM_CONSTANT.findall(header)) | set(SPECIAL_CONSTANT.findall(header))


def header_typedefs() -> set[str]:
    return set(INT32_TYPEDEF.findall(public_header()))


def header_struct_fields() -> dict[str, list[str]]:
    return {
        match.group("name"): FIELD.findall(
            re.sub(r"/\*.*?\*/", "", match.group("body"), flags=re.DOTALL)
        )
        for match in CONCRETE_STRUCT.finditer(public_header())
    }


def windows_definition_symbols() -> set[str]:
    lines = (ROOT / "abi" / "kairosboot.def").read_text(encoding="utf-8").splitlines()
    symbols = [
        line.strip() for line in lines if line.strip() and line.strip() != "EXPORTS"
    ]
    if symbols != sorted(set(symbols)):
        fail("abi/kairosboot.def must be sorted and contain no duplicates")
    return set(symbols)


def c_integer(value: int) -> str:
    if value <= 2147483647:
        return str(value)
    if value <= 4294967295:
        return f"UINT32_C({value})"
    if value == 18446744073709551615:
        return "UINT64_MAX"
    return f"UINT64_C({value})"


def render_layout_checks(manifest: dict[str, object]) -> str:
    lines = [
        "/* Generated from abi/kairosboot-v1.json by scripts/check_abi.py. */",
        "#ifndef KAIROSBOOT_ABI_LAYOUT_V1_H",
        "#define KAIROSBOOT_ABI_LAYOUT_V1_H",
        "",
        "#include <kairosboot/kairosboot.h>",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "#if defined(__cplusplus)",
        "#define KB_ABI_ALIGNOF(type) alignof(type)",
        "#define KB_ABI_STATIC_ASSERT(condition, message) static_assert(condition, message)",
        "#else",
        "#define KB_ABI_ALIGNOF(type) _Alignof(type)",
        "#define KB_ABI_STATIC_ASSERT(condition, message) _Static_assert(condition, message)",
        "#endif",
        "",
        "KB_ABI_STATIC_ASSERT(sizeof(void *) == 8, \"ABI v1 pointer size\");",
        "KB_ABI_STATIC_ASSERT(sizeof(size_t) == 8, \"ABI v1 size_t size\");",
    ]
    for name, value in manifest["constants"].items():
        lines.append(
            f'KB_ABI_STATIC_ASSERT({name} == {c_integer(value)}, "{name} value");'
        )
    lines.append("")
    for name, layout in manifest["typedefs"].items():
        lines.extend(
            [
                f'KB_ABI_STATIC_ASSERT(sizeof({name}) == {layout["size"]}, "{name} size");',
                f'KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF({name}) == {layout["alignment"]}, "{name} alignment");',
            ]
        )
    lines.append("")
    for name, layout in manifest["structs"].items():
        lines.extend(
            [
                f'KB_ABI_STATIC_ASSERT(sizeof({name}) == {layout["size"]}, "{name} size");',
                f'KB_ABI_STATIC_ASSERT(KB_ABI_ALIGNOF({name}) == {layout["alignment"]}, "{name} alignment");',
            ]
        )
        for field in layout["fields"]:
            field_name = field["name"]
            lines.extend(
                [
                    f'KB_ABI_STATIC_ASSERT(offsetof({name}, {field_name}) == {field["offset"]}, "{name}.{field_name} offset");',
                    f'KB_ABI_STATIC_ASSERT(sizeof((({name} *)0)->{field_name}) == {field["size"]}, "{name}.{field_name} size");',
                ]
            )
        version_size = layout.get("versionSize")
        if version_size is not None:
            lines.append(
                f'KB_ABI_STATIC_ASSERT({version_size["macro"]} == {version_size["value"]}, "{version_size["macro"]} value");'
            )
        lines.append("")
    lines.extend(
        [
            "#undef KB_ABI_STATIC_ASSERT",
            "#undef KB_ABI_ALIGNOF",
            "",
            "#endif",
            "",
        ]
    )
    return "\n".join(lines)


def check_frozen_contract(manifest: dict[str, object]) -> None:
    manifest_symbols_set = manifest_symbols(manifest)
    linker_symbols = linker_manifest_symbols()
    if linker_symbols != manifest_symbols_set:
        fail(
            "linker whitelist and ABI freeze manifest differ; "
            f"missing={sorted(manifest_symbols_set - linker_symbols)}, "
            f"extra={sorted(linker_symbols - manifest_symbols_set)}"
        )
    constants = set(manifest["constants"])
    declared_constants = header_constants()
    if constants != declared_constants:
        fail(
            "public constants and ABI freeze manifest differ; "
            f"missing={sorted(declared_constants - constants)}, "
            f"stale={sorted(constants - declared_constants)}"
        )
    typedefs = set(manifest["typedefs"])
    declared_typedefs = header_typedefs()
    if typedefs != declared_typedefs:
        fail(
            "public int32 typedefs and ABI freeze manifest differ; "
            f"missing={sorted(declared_typedefs - typedefs)}, "
            f"stale={sorted(typedefs - declared_typedefs)}"
        )
    manifest_fields = {
        name: [field["name"] for field in layout["fields"]]
        for name, layout in manifest["structs"].items()
    }
    declared_fields = header_struct_fields()
    if manifest_fields != declared_fields:
        fail(
            "public struct fields and ABI freeze manifest differ; "
            f"manifest={manifest_fields}, header={declared_fields}"
        )
    expected_layout_checks = render_layout_checks(manifest)
    actual_layout_checks = ABI_LAYOUT_CHECKS.read_text(encoding="utf-8")
    if actual_layout_checks != expected_layout_checks:
        fail(
            "abi/kairosboot-layout-v1.h is stale; regenerate it with "
            "scripts/check_abi.py --write-layout-header"
        )


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
    parser.add_argument("--write-layout-header", action="store_true")
    args = parser.parse_args()

    manifest_data = abi_manifest()
    if args.write_layout_header:
        ABI_LAYOUT_CHECKS.write_text(
            render_layout_checks(manifest_data), encoding="utf-8"
        )
    check_frozen_contract(manifest_data)
    manifest = manifest_symbols(manifest_data)
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
