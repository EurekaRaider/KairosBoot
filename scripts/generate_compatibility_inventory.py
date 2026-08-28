#!/usr/bin/env python3
"""Generate the auditable Platform-Tools compatibility inventory.

The upstream surface is frozen separately from KairosBoot's implementation
evidence.  This tool refuses incomplete assessments, stale public symbols,
stale CLI spellings, missing evidence files, and any status outside the frozen
vocabulary.  Generated files must only be changed by rerunning this tool.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any


ALLOWED_STATUSES = {
    "implemented",
    "partial",
    "missing",
    "intentional-deviation",
}
SOURCE_PATH = Path("compat/aosp-platform-tools-37.0.1-source-inventory.json")
EVIDENCE_PATH = Path("compat/kairosboot-capability-evidence.json")
LOCK_PATH = Path("compat/aosp.lock.json")
JSON_OUTPUT = Path("compat/generated-inventory.json")
YAML_OUTPUT = Path("compat/compatibility.yaml")
C_HEADER = Path("include/kairosboot/kairosboot.h")
CLI_SOURCE = Path("cli/main.cpp")


class InventoryError(RuntimeError):
    """A source or evidence contract is incomplete or stale."""


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise InventoryError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise InventoryError(f"{path} must contain a JSON object")
    return value


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise InventoryError(message)


def _public_c_symbols(header: str) -> set[str]:
    return set(re.findall(r"\bKB_CALL\s+(kb_[a-z0-9_]+)\s*\(", header))


def _cli_registry_literals(source: str) -> set[str]:
    start = source.find("bool is_global_option(")
    end = source.find("std::string_view command_name(")
    _require(start >= 0 and end > start, "cannot locate CLI parser registry")
    parser = source[start:end]
    return set(re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', parser))


def _validate_evidence_paths(root: Path, entry: dict[str, Any]) -> None:
    evidence = entry.get("evidence")
    _require(isinstance(evidence, list), f"{entry['id']}: evidence must be a list")
    _require(
        all(isinstance(value, str) and value for value in evidence),
        f"{entry['id']}: evidence paths must be non-empty strings",
    )
    if entry["status"] == "missing":
        _require(not evidence, f"{entry['id']}: missing entries cannot cite implementation evidence")
    else:
        _require(bool(evidence), f"{entry['id']}: {entry['status']} requires evidence")
    for value in evidence:
        path = Path(value)
        _require(not path.is_absolute() and ".." not in path.parts,
                 f"{entry['id']}: evidence path must be repository-relative: {value}")
        _require((root / path).is_file(), f"{entry['id']}: evidence file does not exist: {value}")


def generate(root: Path) -> tuple[dict[str, Any], str]:
    root = root.resolve()
    lock = _read_json(root / LOCK_PATH)
    source = _read_json(root / SOURCE_PATH)
    evidence = _read_json(root / EVIDENCE_PATH)

    _require(lock.get("baselineStatus") == "locked", "AOSP baseline is not locked")
    _require(lock.get("claimCompatibility") is False,
             "claimCompatibility must remain false until official differentials pass")
    aosp = lock.get("aosp")
    _require(isinstance(aosp, dict), "AOSP lock is missing its aosp object")
    version = aosp.get("platformToolsVersion")
    commit = aosp.get("sourceCommit")
    _require(source.get("documentType") == "kairosboot.aosp-source-help-inventory",
             "unsupported frozen source inventory")
    _require(source.get("schemaVersion") == 1, "unsupported source inventory schema")
    _require(source.get("platformToolsVersion") == version,
             "source inventory and lock use different Platform-Tools versions")
    _require(source.get("sourceCommit") == commit,
             "source inventory and lock use different AOSP commits")
    lock_inventory = aosp.get("compatibilityInventory")
    _require(isinstance(lock_inventory, dict), "AOSP lock does not pin the source inventory")
    _require(lock_inventory.get("path") == SOURCE_PATH.as_posix(),
             "AOSP lock source inventory path is stale")
    _require(lock_inventory.get("sha256") == _sha256(root / SOURCE_PATH),
             "frozen source inventory SHA-256 differs from the AOSP lock")

    source_entries = source.get("entries")
    _require(isinstance(source_entries, list) and source_entries,
             "source inventory entries must be a non-empty list")
    upstream: dict[str, dict[str, Any]] = {}
    allowed_source_keys = {"id", "kind", "required", "spelling", "file", "partition"}
    for raw in source_entries:
        _require(isinstance(raw, dict), "source inventory entries must be objects")
        _require(set(raw).issubset(allowed_source_keys),
                 f"{raw.get('id', '<missing>')}: unknown source inventory fields")
        identifier = raw.get("id")
        _require(isinstance(identifier, str) and identifier,
                 "source inventory entry has no id")
        _require(identifier not in upstream, f"duplicate source inventory id: {identifier}")
        _require(isinstance(raw.get("kind"), str) and raw["kind"],
                 f"{identifier}: kind is required")
        _require(isinstance(raw.get("required"), bool),
                 f"{identifier}: required must be boolean")
        upstream[identifier] = raw

    _require(evidence.get("documentType") == "kairosboot.capability-evidence",
             "unsupported capability evidence document")
    _require(evidence.get("schemaVersion") == 1,
             "unsupported capability evidence schema")
    assessments = evidence.get("assessments")
    _require(isinstance(assessments, list), "assessments must be a list")
    public_symbols = _public_c_symbols((root / C_HEADER).read_text(encoding="utf-8"))
    cli_literals = _cli_registry_literals((root / CLI_SOURCE).read_text(encoding="utf-8"))

    expanded: dict[str, dict[str, Any]] = {}
    for assessment in assessments:
        _require(isinstance(assessment, dict), "assessment must be an object")
        ids = assessment.get("ids")
        status = assessment.get("status")
        note = assessment.get("note")
        _require(isinstance(ids, list) and ids, "assessment ids must be non-empty")
        _require(status in ALLOWED_STATUSES,
                 f"assessment uses forbidden or unknown status: {status!r}")
        _require(isinstance(note, str) and note, "assessment note must be non-empty")
        symbols = assessment.get("cSymbols", [])
        commands = assessment.get("cliCommands", [])
        _require(isinstance(symbols, list) and all(isinstance(v, str) for v in symbols),
                 "cSymbols must be a string list")
        _require(isinstance(commands, list) and all(isinstance(v, str) for v in commands),
                 "cliCommands must be a string list")
        for symbol in symbols:
            _require(symbol in public_symbols,
                     f"declared public C symbol is absent from {C_HEADER}: {symbol}")
        for command in commands:
            _require(command in cli_literals,
                     f"declared CLI spelling is absent from the parser registry: {command}")
        for identifier in ids:
            _require(identifier in upstream, f"assessment references unknown upstream id: {identifier}")
            _require(identifier not in expanded, f"duplicate assessment for {identifier}")
            item = dict(upstream[identifier])
            item.update(
                status=status,
                note=note,
                evidence=list(assessment.get("evidence", [])),
                cSymbols=list(symbols),
                cliCommands=list(commands),
                officialDifferentialEvidence=[],
            )
            _validate_evidence_paths(root, item)
            expanded[identifier] = item

    missing_assessments = sorted(set(upstream) - set(expanded))
    extra_assessments = sorted(set(expanded) - set(upstream))
    _require(not missing_assessments,
             "unassessed upstream entries: " + ", ".join(missing_assessments))
    _require(not extra_assessments,
             "assessment entries absent from source inventory: " + ", ".join(extra_assessments))

    deviations = evidence.get("additionalDeviations")
    _require(isinstance(deviations, list), "additionalDeviations must be a list")
    deviation_entries: list[dict[str, Any]] = []
    seen_ids = set(upstream)
    for raw in deviations:
        _require(isinstance(raw, dict), "additional deviation must be an object")
        identifier = raw.get("id")
        _require(isinstance(identifier, str) and identifier and identifier not in seen_ids,
                 f"duplicate or invalid additional deviation id: {identifier!r}")
        _require(raw.get("status") == "intentional-deviation",
                 f"{identifier}: additional deviations must use intentional-deviation")
        _require(raw.get("scope") in upstream,
                 f"{identifier}: deviation scope is not an upstream entry")
        _require(isinstance(raw.get("note"), str) and raw["note"],
                 f"{identifier}: deviation note must be non-empty")
        item = {
            "id": identifier,
            "kind": "deviation",
            "required": False,
            "status": "intentional-deviation",
            "scope": raw["scope"],
            "note": raw["note"],
            "evidence": list(raw.get("evidence", [])),
            "cSymbols": [],
            "cliCommands": [],
            "officialDifferentialEvidence": [],
        }
        _validate_evidence_paths(root, item)
        deviation_entries.append(item)
        seen_ids.add(identifier)

    entries = sorted([*expanded.values(), *deviation_entries], key=lambda value: value["id"])
    counts = Counter(entry["status"] for entry in entries)
    by_kind: dict[str, dict[str, int]] = {}
    for entry in entries:
        kind_counts = by_kind.setdefault(
            entry["kind"], {status: 0 for status in sorted(ALLOWED_STATUSES)}
        )
        kind_counts[entry["status"]] += 1
    required_entries = [entry for entry in entries if entry["required"]]
    gaps = [
        entry["id"]
        for entry in required_entries
        if entry["status"] in {"partial", "missing"}
    ]
    inventory = {
        "baseline": {
            "aospCommit": commit,
            "platformToolsVersion": version,
            "sourceInventoryPath": SOURCE_PATH.as_posix(),
            "sourceInventorySha256": _sha256(root / SOURCE_PATH),
        },
        "claimCompatibility": False,
        "completionRule": (
            "claimCompatibility may become true only after every required entry is "
            "implemented or an approved intentional deviation, every required entry has "
            "official Platform-Tools 37.0.1 differential evidence, and no partial, missing, "
            "unknown, or unassessed entry remains"
        ),
        "documentType": "kairosboot.fastboot-compatibility-inventory",
        "entries": entries,
        "generationInputs": {
            "capabilityEvidence": EVIDENCE_PATH.as_posix(),
            "cHeader": C_HEADER.as_posix(),
            "cliRegistry": CLI_SOURCE.as_posix(),
        },
        "inventoryStatus": "generated-known-gaps",
        "officialDifferentialCoverage": {
            "requiredEntries": len(required_entries),
            "requiredEntriesWithEvidence": 0,
            "status": "not-run",
        },
        "requiredGaps": gaps,
        "schemaVersion": 2,
        "statusVocabulary": sorted(ALLOWED_STATUSES),
        "summary": {
            "byKind": {key: by_kind[key] for key in sorted(by_kind)},
            "byStatus": {status: counts[status] for status in sorted(ALLOWED_STATUSES)},
            "totalEntries": len(entries),
        },
    }
    return inventory, _render_yaml(inventory)


def _yaml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def _render_yaml(inventory: dict[str, Any]) -> str:
    baseline = inventory["baseline"]
    summary = inventory["summary"]
    lines = [
        "# Generated by scripts/generate_compatibility_inventory.py; do not edit.",
        f"schemaVersion: {inventory['schemaVersion']}",
        f"baselineStatus: locked",
        f"claimCompatibility: false",
        "baseline:",
        f"  platformToolsVersion: {_yaml_string(baseline['platformToolsVersion'])}",
        f"  aospCommit: {_yaml_string(baseline['aospCommit'])}",
        f"  sourceInventoryPath: {_yaml_string(baseline['sourceInventoryPath'])}",
        f"  sourceInventorySha256: {_yaml_string(baseline['sourceInventorySha256'])}",
        "statusVocabulary:",
    ]
    lines.extend(f"  - {_yaml_string(status)}" for status in inventory["statusVocabulary"])
    lines.extend(["summary:", f"  totalEntries: {summary['totalEntries']}", "  byStatus:"])
    lines.extend(
        f"    {status}: {summary['byStatus'][status]}"
        for status in inventory["statusVocabulary"]
    )
    lines.extend([
        "officialDifferentialCoverage:",
        f"  status: {_yaml_string(inventory['officialDifferentialCoverage']['status'])}",
        f"  requiredEntries: {inventory['officialDifferentialCoverage']['requiredEntries']}",
        "  requiredEntriesWithEvidence: 0",
        "entries:",
    ])
    for entry in inventory["entries"]:
        lines.extend([
            f"  - id: {_yaml_string(entry['id'])}",
            f"    kind: {_yaml_string(entry['kind'])}",
            f"    required: {'true' if entry['required'] else 'false'}",
            f"    status: {_yaml_string(entry['status'])}",
            f"    note: {_yaml_string(entry['note'])}",
        ])
        if "scope" in entry:
            lines.append(f"    scope: {_yaml_string(entry['scope'])}")
        if entry["evidence"]:
            lines.append("    evidence:")
            lines.extend(f"      - {_yaml_string(path)}" for path in entry["evidence"])
        else:
            lines.append("    evidence: []")
    lines.extend([
        "requiredGaps:",
        *[f"  - {_yaml_string(identifier)}" for identifier in inventory["requiredGaps"]],
        f"completionRule: {_yaml_string(inventory['completionRule'])}",
        "",
    ])
    return "\n".join(lines)


def _canonical_json(value: dict[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _check_file(path: Path, expected: str) -> None:
    try:
        actual = path.read_text(encoding="utf-8")
    except OSError as error:
        raise InventoryError(f"cannot read generated file {path}: {error}") from error
    _require(actual == expected, f"generated file is stale; run this tool: {path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repository-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
    )
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        inventory, yaml_text = generate(args.repository_root)
        json_text = _canonical_json(inventory)
        if args.check:
            _check_file(args.repository_root / JSON_OUTPUT, json_text)
            _check_file(args.repository_root / YAML_OUTPUT, yaml_text)
        else:
            (args.repository_root / JSON_OUTPUT).write_text(json_text, encoding="utf-8")
            (args.repository_root / YAML_OUTPUT).write_text(yaml_text, encoding="utf-8")
        summary = inventory["summary"]["byStatus"]
        print(
            "compatibility inventory: "
            + ", ".join(f"{status}={summary[status]}" for status in sorted(summary))
            + f", required-gaps={len(inventory['requiredGaps'])}, claim=false"
        )
        return 0
    except InventoryError as error:
        print(f"compatibility inventory error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
