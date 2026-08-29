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
import subprocess
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
OFFICIAL_EVIDENCE_PATH = Path("compat/official-differential-evidence.json")
JSON_OUTPUT = Path("compat/generated-inventory.json")
C_HEADER = Path("include/kairosboot/kairosboot.h")
CLI_SOURCE = Path("cli/main.cpp")


MATCHED_SCENARIO_CONTRACTS: dict[str, dict[str, Any]] = {
    "official-host-devices": {
        "transport": "host", "coverageIds": ["command.devices"],
        "argv": ["devices"], "getvars": [], "commands": [], "data": [],
    },
    "official-host-help": {
        "transport": "host", "coverageIds": ["option.help"],
        "argv": ["--help"], "getvars": [], "commands": [], "data": [],
    },
    "official-host-help-short": {
        "transport": "host", "coverageIds": ["option.help-short"],
        "argv": ["-h"], "getvars": [], "commands": [], "data": [],
    },
    "official-host-version": {
        "transport": "host", "coverageIds": ["option.version"],
        "argv": ["--version"], "getvars": [], "commands": [], "data": [],
    },
    "official-tcp-getvar": {
        "transport": "tcp",
        "coverageIds": ["command.getvar", "transport.tcp"],
        "argv": ["getvar", "product"],
        "getvars": ["product"],
        "commands": [],
        "data": [],
    },
    "official-udp-getvar": {
        "transport": "udp",
        "coverageIds": ["command.getvar", "transport.udp"],
        "argv": ["getvar", "product"],
        "getvars": ["product"],
        "commands": [],
        "data": [],
    },
    "official-tcp-flash": {
        "transport": "tcp",
        "coverageIds": [
            "command.flash", "protocol.command", "protocol.download",
            "capability.raw-image", "image.system",
        ],
        "argv": ["flash", "system", "<ARTIFACT>/system.img"],
        "getvars": [
            "is-userspace", "has-slot:system", "is-logical:system",
            "max-download-size",
        ],
        "commands": ["download:00000020", "flash:system"],
        "data": [["host-to-device", 32]],
    },
    "official-udp-flash": {
        "transport": "udp",
        "coverageIds": [
            "command.flash", "protocol.command", "protocol.download",
            "capability.raw-image", "image.system",
        ],
        "argv": ["flash", "system", "<ARTIFACT>/system.img"],
        "getvars": [
            "is-userspace", "has-slot:system", "is-logical:system",
            "max-download-size",
        ],
        "commands": ["download:00000020", "flash:system"],
        "data": [["host-to-device", 32]],
    },
    "official-tcp-flash-default-file": {
        "transport": "tcp",
        "coverageIds": [
            "command.flash", "protocol.command", "protocol.download",
            "capability.raw-image", "image.system",
        ],
        "argv": ["flash", "system"],
        "getvars": [
            "is-userspace", "has-slot:system", "is-logical:system",
            "max-download-size",
        ],
        "commands": ["download:00000020", "flash:system"],
        "data": [["host-to-device", 32]],
    },
    "official-tcp-flash-force": {
        "transport": "tcp", "coverageIds": ["option.force"],
        "argv": ["--force", "flash", "system", "<ARTIFACT>/system.img"],
        "getvars": [
            "is-userspace", "has-slot:system", "is-logical:system",
            "max-download-size",
        ],
        "commands": ["download:00000020", "flash:system"],
        "data": [["host-to-device", 32]],
    },
    "official-tcp-signature": {
        "transport": "tcp",
        "coverageIds": ["command.signature"],
        "argv": ["signature", "<ARTIFACT>/signature.bin"],
        "getvars": [],
        "commands": ["download:00000100", "signature"],
        "data": [["host-to-device", 256]],
    },
    "official-udp-signature": {
        "transport": "udp",
        "coverageIds": ["command.signature"],
        "argv": ["signature", "<ARTIFACT>/signature.bin"],
        "getvars": [],
        "commands": ["download:00000100", "signature"],
        "data": [["host-to-device", 256]],
    },
    "official-tcp-reboot": {
        "transport": "tcp", "coverageIds": ["command.reboot"],
        "argv": ["reboot"], "getvars": [], "commands": ["reboot"], "data": [],
    },
    "official-tcp-reboot-bootloader": {
        "transport": "tcp", "coverageIds": ["command.reboot-bootloader"],
        "argv": ["reboot", "bootloader"], "getvars": [],
        "commands": ["reboot-bootloader"], "data": [],
    },
    "official-tcp-reboot-recovery": {
        "transport": "tcp", "coverageIds": ["command.reboot-recovery"],
        "argv": ["reboot-recovery"], "getvars": [],
        "legacyArgv": [["reboot", "recovery"]],
        "commands": ["reboot-recovery"], "data": [],
    },
    "official-tcp-continue": {
        "transport": "tcp", "coverageIds": ["command.continue"],
        "argv": ["continue"], "getvars": [], "commands": ["continue"], "data": [],
    },
    "official-tcp-oem": {
        "transport": "tcp", "coverageIds": ["command.oem"],
        "argv": ["oem", "differential"], "getvars": [],
        "commands": ["oem differential"], "data": [],
    },
    "official-tcp-informational-responses": {
        "transport": "tcp", "coverageIds": ["protocol.responses"],
        "argv": ["oem", "differential-info"], "getvars": [],
        "commands": ["oem differential-info"], "data": [],
        "responses": [
            ["INFO", "phase one"], ["TEXT", "phase two"],
            ["OKAY", "accepted"],
        ],
    },
    "official-tcp-stage": {
        "transport": "tcp", "coverageIds": ["command.stage"],
        "argv": ["stage", "<ARTIFACT>/stage.bin"], "getvars": [],
        "commands": ["download:00000020"], "data": [["host-to-device", 32]],
    },
    "official-tcp-get-staged": {
        "transport": "tcp",
        "coverageIds": ["command.get-staged", "protocol.upload"],
        "argv": ["get_staged", "<OUTPUT>/stage.bin"], "getvars": [],
        "commands": ["upload"], "data": [["device-to-host", 20]],
    },
    "official-tcp-flashing-get-unlock-ability": {
        "transport": "tcp",
        "coverageIds": ["command.flashing-get-unlock-ability"],
        "argv": ["flashing", "get_unlock_ability"], "getvars": [],
        "legacyArgv": [["flashing", "get-unlock-ability"]],
        "commands": ["flashing get_unlock_ability"], "data": [],
    },
    "official-tcp-flashing-lock": {
        "transport": "tcp", "coverageIds": ["command.flashing-lock"],
        "argv": ["flashing", "lock"], "getvars": [],
        "commands": ["flashing lock"], "data": [],
    },
    "official-tcp-flashing-unlock": {
        "transport": "tcp", "coverageIds": ["command.flashing-unlock"],
        "argv": ["flashing", "unlock"], "getvars": [],
        "commands": ["flashing unlock"], "data": [],
    },
    "official-tcp-flashing-lock-critical": {
        "transport": "tcp",
        "coverageIds": ["command.flashing-lock-critical"],
        "argv": ["flashing", "lock_critical"], "getvars": [],
        "legacyArgv": [["flashing", "lock-critical"]],
        "commands": ["flashing lock_critical"], "data": [],
    },
    "official-tcp-flashing-unlock-critical": {
        "transport": "tcp",
        "coverageIds": ["command.flashing-unlock-critical"],
        "argv": ["flashing", "unlock_critical"], "getvars": [],
        "legacyArgv": [["flashing", "unlock-critical"]],
        "commands": ["flashing unlock_critical"], "data": [],
    },
    "official-tcp-create-logical-partition": {
        "transport": "tcp", "coverageIds": ["command.create-logical-partition"],
        "argv": ["create-logical-partition", "differential", "4096"],
        "getvars": [], "commands": ["create-logical-partition:differential:4096"],
        "data": [],
    },
    "official-tcp-delete-logical-partition": {
        "transport": "tcp", "coverageIds": ["command.delete-logical-partition"],
        "argv": ["delete-logical-partition", "differential"], "getvars": [],
        "commands": ["delete-logical-partition:differential"], "data": [],
    },
    "official-tcp-gsi-wipe": {
        "transport": "tcp", "coverageIds": ["command.gsi-wipe"],
        "argv": ["gsi", "wipe"], "getvars": [], "commands": ["gsi:wipe"],
        "data": [],
    },
    "official-tcp-gsi-disable": {
        "transport": "tcp", "coverageIds": ["command.gsi-disable"],
        "argv": ["gsi", "disable"], "getvars": [],
        "commands": ["gsi:disable"], "data": [],
    },
    "official-tcp-gsi-status": {
        "transport": "tcp", "coverageIds": ["command.gsi-status"],
        "argv": ["gsi", "status"], "getvars": [],
        "commands": ["gsi:status"], "data": [],
    },
    "official-tcp-snapshot-cancel": {
        "transport": "tcp", "coverageIds": ["command.snapshot-cancel"],
        "argv": ["snapshot-update", "cancel"], "getvars": [],
        "commands": ["snapshot-update:cancel"], "data": [],
    },
    "official-tcp-snapshot-merge": {
        "transport": "tcp", "coverageIds": ["command.snapshot-merge"],
        "argv": ["snapshot-update", "merge"], "getvars": [],
        "commands": ["snapshot-update:merge"], "data": [],
    },
    "official-tcp-serial-selector": {
        "transport": "tcp", "coverageIds": ["option.serial"],
        "argv": ["-s", "<ENDPOINT>", "getvar", "product"],
        "legacyArgv": [["--device", "<ENDPOINT>", "getvar", "product"]],
        "getvars": ["product"], "commands": [], "data": [],
    },
    "official-tcp-verbose": {
        "transport": "tcp", "coverageIds": ["option.verbose"],
        "argv": ["--verbose", "getvar", "product"],
        "getvars": ["product"], "commands": [], "data": [],
    },
    "official-tcp-boot-raw": {
        "transport": "tcp",
        "coverageIds": ["command.boot", "capability.boot-image-construction"],
        "argv": ["boot", "<ARTIFACT>/kernel.bin", "<ARTIFACT>/ramdisk.bin"],
        "getvars": [],
        "commands": ["download:00001800", "boot"],
        "data": [["host-to-device", 6144]],
    },
    "official-tcp-boot-raw-options": {
        "transport": "tcp",
        "coverageIds": [
            "option.base", "option.kernel-offset", "option.ramdisk-offset",
            "option.tags-offset", "option.page-size", "option.header-version",
            "option.os-version", "option.os-patch-level", "option.cmdline",
            "option.dtb", "option.dtb-offset",
        ],
        "argv": [
            "--base", "0x10000000", "--kernel-offset", "0x00008000",
            "--ramdisk-offset", "0x01000000", "--tags-offset", "0x00000100",
            "--page-size", "4096", "--header-version", "2",
            "--os-version", "13.0.0", "--os-patch-level", "2024-01-05",
            "--cmdline", "console=ttyS0 differential",
            "--dtb", "<ARTIFACT>/dtb.bin", "--dtb-offset", "0x01100000",
            "boot", "<ARTIFACT>/kernel.bin", "<ARTIFACT>/ramdisk.bin",
        ],
        "getvars": [], "commands": ["download:00004000", "boot"],
        "data": [["host-to-device", 16384]],
    },
    "official-tcp-flash-raw": {
        "transport": "tcp",
        "coverageIds": ["command.flash-raw",
                        "capability.boot-image-construction"],
        "argv": ["flash:raw", "boot", "<ARTIFACT>/kernel.bin",
                 "<ARTIFACT>/ramdisk.bin"],
        "getvars": ["has-slot:boot"],
        "commands": ["download:00001800", "flash:boot"],
        "data": [["host-to-device", 6144]],
    },
}

UNCOVERED_SCENARIO_CONTRACTS: dict[str, list[str]] = {
    "official-scripted-fetch-chunking": ["command.fetch"],
    "official-scripted-reboot-fastboot": ["command.reboot-fastboot"],
    "official-scripted-erase": ["command.erase"],
    # Retain the old mapping so a stale pre-fix capture remains structurally
    # auditable; current evidence is accepted only from the matched scenarios.
    "official-scripted-boot-flash-raw": [
        "command.boot", "command.flash-raw", "capability.boot-image-construction",
    ],
    "official-scripted-slot-policy": [
        "command.set-active", "option.slot", "option.set-active", "capability.a-b-slots",
    ],
    "official-scripted-avb-flags": [
        "option.disable-verity", "option.disable-verification",
        "capability.vbmeta-avb-mutation",
    ],
    "official-scripted-sparse-limit": [
        "option.sparse-limit", "capability.android-sparse",
    ],
    "official-scripted-format": ["command.format", "option.fs-options"],
    "official-scripted-wipe-super": [
        "command.wipe-super", "capability.dynamic-partitions",
    ],
    "official-scripted-update-flashall": [
        "command.update", "command.flashall", "capability.update-zip",
    ],
    "official-scripted-resize-logical-partition": [
        "command.resize-logical-partition",
    ],
}

EVIDENCE_ONLY_PATHS = {
    OFFICIAL_EVIDENCE_PATH.as_posix(),
    JSON_OUTPUT.as_posix(),
}


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


def _repository_file(root: Path, value: Any, label: str) -> Path:
    _require(isinstance(value, str) and value, f"{label} must be a non-empty path")
    path = Path(value)
    _require(not path.is_absolute() and ".." not in path.parts,
             f"{label} must be repository-relative: {value}")
    resolved = root / path
    _require(resolved.is_file(), f"{label} does not exist: {value}")
    return resolved


def _repository_commit(root: Path) -> str | None:
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "--verify", "HEAD^{commit}"],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    commit = completed.stdout.strip()
    if completed.returncode != 0 or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        return None
    try:
        status = subprocess.run(
            [
                "git", "-C", str(root), "status", "--porcelain",
                "--untracked-files=no",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if status.returncode != 0 or status.stdout.strip():
        return None
    return commit


def _capture_matches_current_repository(
    root: Path, capture_commit: str, current_commit: str | None
) -> bool:
    if current_commit is None:
        return False
    if capture_commit == current_commit:
        return True
    try:
        ancestor = subprocess.run(
            [
                "git", "-C", str(root), "merge-base", "--is-ancestor",
                capture_commit, current_commit,
            ],
            check=False,
            capture_output=True,
            timeout=10,
        )
        if ancestor.returncode != 0:
            return False
        changed = subprocess.run(
            [
                "git", "-C", str(root), "diff", "--name-only",
                f"{capture_commit}..{current_commit}", "--",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    if changed.returncode != 0:
        return False
    paths = [line for line in changed.stdout.splitlines() if line]
    return bool(paths) and all(
        path in EVIDENCE_ONLY_PATHS or path.startswith("compat/evidence/")
        for path in paths
    )


def _validate_scenario_semantics(
    metadata_path: str,
    identifier: str,
    captured: dict[str, Any],
    contract: dict[str, Any],
) -> None:
    events = captured.get("events")
    _require(
        isinstance(events, list) and len(events) >= 2,
        f"{metadata_path}#{identifier}: normalized scenario has no event stream",
    )
    cli_parse = events[0]
    terminal = events[-1]
    _require(
        isinstance(cli_parse, dict)
        and cli_parse.get("kind") == "CLI_PARSE"
        and cli_parse.get("result") == "ok"
        and cli_parse.get("argv")
        in [contract["argv"], *contract.get("legacyArgv", [])],
        f"{metadata_path}#{identifier}: CLI command semantics differ from the whitelist",
    )
    _require(
        isinstance(terminal, dict)
        and terminal.get("kind") == "EXIT"
        and terminal.get("code") == 0,
        f"{metadata_path}#{identifier}: scenario does not have a successful terminal event",
    )
    getvars = [
        event.get("name")
        for event in events
        if isinstance(event, dict) and event.get("kind") == "GETVAR"
    ]
    commands = [
        event.get("command")
        for event in events
        if isinstance(event, dict) and event.get("kind") == "COMMAND"
    ]
    data = [
        [event.get("direction"), event.get("size")]
        for event in events
        if isinstance(event, dict) and event.get("kind") == "DATA"
    ]
    responses = [
        [event.get("kind"), event.get("message")]
        for event in events
        if isinstance(event, dict)
        and event.get("kind") in {"INFO", "TEXT", "OKAY", "FAIL"}
    ]
    _require(
        getvars == contract["getvars"]
        and commands == contract["commands"]
        and data == contract["data"]
        and (
            "responses" not in contract or responses == contract["responses"]
        ),
        f"{metadata_path}#{identifier}: protocol event semantics differ from the whitelist",
    )


def _official_differential_coverage(
    root: Path,
    lock: dict[str, Any],
    upstream: dict[str, dict[str, Any]],
) -> tuple[dict[str, list[str]], int, int]:
    index = _read_json(root / OFFICIAL_EVIDENCE_PATH)
    _require(
        set(index) == {"captures", "documentType", "platformToolsVersion", "schemaVersion"},
        "official differential evidence index has unknown or missing fields",
    )
    _require(
        index["documentType"] == "kairosboot.official-fastboot-differential-index"
        and index["schemaVersion"] == 1,
        "unsupported official differential evidence index",
    )
    aosp = lock["aosp"]
    _require(index["platformToolsVersion"] == aosp["platformToolsVersion"],
             "official differential evidence uses a different Platform-Tools version")
    captures = index["captures"]
    _require(isinstance(captures, list), "official differential captures must be a list")
    current_commit = _repository_commit(root)

    mapped: dict[str, list[str]] = {}
    seen_metadata: set[str] = set()
    seen_scenarios: set[tuple[str, str]] = set()
    scenario_count = 0
    uncovered_count = 0
    for raw_metadata_path in captures:
        metadata_file = _repository_file(
            root, raw_metadata_path, "official differential metadata"
        )
        metadata_path = Path(raw_metadata_path).as_posix()
        _require(metadata_path not in seen_metadata,
                 f"duplicate official differential metadata: {metadata_path}")
        seen_metadata.add(metadata_path)
        metadata = _read_json(metadata_file)
        _require(
            set(metadata) == {
                "aospFastboot", "baseline", "captureFiles", "documentType",
                "kairosboot", "result", "scenarios", "schemaVersion",
                "uncoveredScenarios",
            },
            f"{metadata_path}: unknown or missing evidence fields",
        )
        _require(
            metadata["documentType"] ==
            "kairosboot.official-fastboot-differential-evidence"
            and metadata["schemaVersion"] == 1
            and metadata["result"] == "matched",
            f"{metadata_path}: evidence is not a matched v1 capture",
        )
        baseline = metadata["baseline"]
        _require(
            isinstance(baseline, dict)
            and baseline.get("platformToolsVersion") == aosp["platformToolsVersion"]
            and baseline.get("sourceCommit") == aosp["sourceCommit"],
            f"{metadata_path}: baseline does not match the AOSP lock",
        )
        official = metadata["aospFastboot"]
        _require(isinstance(official, dict), f"{metadata_path}: aospFastboot must be an object")
        platform_key = official.get("platform")
        archives = aosp["officialArchives"]
        _require(platform_key in archives,
                 f"{metadata_path}: unknown official Fastboot platform")
        _require(official.get("sha256") == archives[platform_key]["fastbootSha256"],
                 f"{metadata_path}: official Fastboot hash differs from the lock")
        version_output = official.get("versionOutput")
        _require(
            isinstance(version_output, str)
            and aosp["platformToolsVersion"] in version_output,
            f"{metadata_path}: official Fastboot version output is stale",
        )
        kairosboot = metadata["kairosboot"]
        _require(
            isinstance(kairosboot, dict)
            and set(kairosboot) == {
                "releaseArtifactSha256", "sha256", "sourceCommit"
            }
            and re.fullmatch(r"[0-9a-f]{64}", str(kairosboot.get("sha256", "")))
            is not None
            and re.fullmatch(
                r"[0-9a-f]{64}",
                str(kairosboot.get("releaseArtifactSha256", "")),
            )
            is not None
            and kairosboot["releaseArtifactSha256"] == kairosboot["sha256"]
            and re.fullmatch(
                r"[0-9a-f]{40}", str(kairosboot.get("sourceCommit", ""))
            )
            is not None,
            f"{metadata_path}: KairosBoot Release artifact provenance is invalid",
        )
        capture_is_current = _capture_matches_current_repository(
            root,
            kairosboot["sourceCommit"],
            current_commit,
        )

        capture_documents: list[dict[str, Any]] = []
        capture_files = metadata["captureFiles"]
        _require(isinstance(capture_files, dict) and set(capture_files) == {"aosp", "kairosboot"},
                 f"{metadata_path}: captureFiles must name AOSP and KairosBoot")
        for label in ("aosp", "kairosboot"):
            descriptor = capture_files[label]
            _require(isinstance(descriptor, dict) and set(descriptor) == {"path", "sha256"},
                     f"{metadata_path}: invalid {label} capture descriptor")
            capture_path = Path(descriptor["path"])
            _require(not capture_path.is_absolute() and ".." not in capture_path.parts,
                     f"{metadata_path}: {label} capture path escapes its evidence directory")
            capture_file = metadata_file.parent / capture_path
            _require(capture_file.is_file(),
                     f"{metadata_path}: missing {label} capture {capture_path}")
            _require(descriptor["sha256"] == _sha256(capture_file),
                     f"{metadata_path}: {label} capture hash mismatch")
            capture_documents.append(_read_json(capture_file))
        _require(capture_documents[0] == capture_documents[1],
                 f"{metadata_path}: persisted normalized captures do not match")
        captured_scenarios = capture_documents[0].get("scenarios")
        _require(isinstance(captured_scenarios, list) and captured_scenarios,
                 f"{metadata_path}: capture has no scenarios")

        scenarios = metadata["scenarios"]
        _require(isinstance(scenarios, list) and scenarios,
                 f"{metadata_path}: evidence has no scenario mappings")
        _require(
            len(scenarios) == len(captured_scenarios),
            f"{metadata_path}: metadata scenario count differs from the capture",
        )
        _require(
            all(isinstance(item, dict) for item in captured_scenarios),
            f"{metadata_path}: captured scenarios must be objects",
        )
        captured_ids = [item.get("id") for item in captured_scenarios]
        metadata_ids: list[str] = []
        scenario_claims: list[tuple[str, list[str]]] = []
        matched_coverage_ids: set[str] = set()
        for scenario, captured in zip(scenarios, captured_scenarios):
            _require(isinstance(scenario, dict),
                     f"{metadata_path}: scenario mapping must be an object")
            identifier = scenario.get("id")
            coverage_ids = scenario.get("coverageIds")
            contract = MATCHED_SCENARIO_CONTRACTS.get(identifier)
            _require(
                isinstance(identifier, str)
                and scenario.get("result") == "matched"
                and contract is not None
                and scenario.get("transport") == contract["transport"],
                f"{metadata_path}: invalid matched scenario mapping",
            )
            key = (metadata_path, identifier)
            _require(key not in seen_scenarios,
                     f"{metadata_path}: duplicate scenario mapping {identifier}")
            seen_scenarios.add(key)
            _require(
                isinstance(coverage_ids, list)
                and coverage_ids
                and len(coverage_ids) == len(set(coverage_ids)),
                f"{metadata_path}#{identifier}: coverageIds must be unique and non-empty",
            )
            _require(
                coverage_ids == contract["coverageIds"],
                f"{metadata_path}#{identifier}: coverageIds differ from the scenario whitelist",
            )
            _require(
                captured.get("id") == identifier,
                f"{metadata_path}#{identifier}: metadata scenario differs from its capture",
            )
            _validate_scenario_semantics(
                metadata_path, identifier, captured, contract
            )
            for coverage_id in coverage_ids:
                _require(coverage_id in upstream,
                         f"{metadata_path}#{identifier}: unknown coverage id {coverage_id}")
                matched_coverage_ids.add(coverage_id)
            scenario_claims.append((identifier, coverage_ids))
            metadata_ids.append(identifier)
        _require(metadata_ids == captured_ids,
                 f"{metadata_path}: metadata scenarios differ from persisted captures")

        uncovered = metadata["uncoveredScenarios"]
        _require(isinstance(uncovered, list),
                 f"{metadata_path}: uncoveredScenarios must be a list")
        uncovered_ids: set[str] = set()
        uncovered_coverage_ids: set[str] = set()
        for candidate in uncovered:
            _require(isinstance(candidate, dict),
                     f"{metadata_path}: uncovered scenario must be an object")
            candidate_id = candidate.get("id")
            candidate_coverage = candidate.get("coverageIds")
            _require(isinstance(candidate_id, str) and candidate_id not in uncovered_ids,
                     f"{metadata_path}: invalid or duplicate uncovered scenario id")
            uncovered_ids.add(candidate_id)
            _require(isinstance(candidate.get("reason"), str) and candidate["reason"],
                     f"{metadata_path}#{candidate_id}: uncovered reason is required")
            _require(isinstance(candidate_coverage, list) and candidate_coverage,
                     f"{metadata_path}#{candidate_id}: uncovered coverageIds are required")
            _require(
                candidate_id in UNCOVERED_SCENARIO_CONTRACTS
                and candidate_coverage
                == UNCOVERED_SCENARIO_CONTRACTS[candidate_id],
                f"{metadata_path}#{candidate_id}: uncovered coverageIds differ from the scenario whitelist",
            )
            for coverage_id in candidate_coverage:
                _require(coverage_id in upstream,
                         f"{metadata_path}#{candidate_id}: unknown coverage id {coverage_id}")
                uncovered_coverage_ids.add(coverage_id)
        _require(
            matched_coverage_ids.isdisjoint(uncovered_coverage_ids),
            f"{metadata_path}: matched and uncovered coverage declarations overlap",
        )

        if capture_is_current:
            for identifier, coverage_ids in scenario_claims:
                for coverage_id in coverage_ids:
                    mapped.setdefault(coverage_id, []).append(
                        f"{metadata_path}#{identifier}"
                    )
            scenario_count += len(scenarios)
            uncovered_count += len(uncovered)

    return mapped, scenario_count, uncovered_count


def generate(root: Path) -> tuple[dict[str, Any], str]:
    root = root.resolve()
    lock = _read_json(root / LOCK_PATH)
    source = _read_json(root / SOURCE_PATH)
    evidence = _read_json(root / EVIDENCE_PATH)

    _require(lock.get("baselineStatus") == "locked", "AOSP baseline is not locked")
    _require(isinstance(lock.get("claimCompatibility"), bool),
             "claimCompatibility in the AOSP lock must be boolean")
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

    official_mapping, differential_scenarios, uncovered_scenarios = (
        _official_differential_coverage(root, lock, upstream)
    )
    entries = sorted([*expanded.values(), *deviation_entries], key=lambda value: value["id"])
    for entry in entries:
        entry["officialDifferentialEvidence"] = list(
            official_mapping.get(entry["id"], [])
        )
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
    required_with_evidence = sum(
        bool(entry["officialDifferentialEvidence"])
        for entry in required_entries
    )
    coverage_status = (
        "not-run"
        if required_with_evidence == 0
        else "complete"
        if required_with_evidence == len(required_entries)
        else "partial"
    )
    claim_ready = not gaps and required_with_evidence == len(required_entries)
    _require(
        not lock["claimCompatibility"] or claim_ready,
        "claimCompatibility cannot be true before all required entries are complete "
        "and have official differential evidence",
    )
    claim_compatibility = bool(lock["claimCompatibility"] and claim_ready)
    inventory = {
        "baseline": {
            "aospCommit": commit,
            "platformToolsVersion": version,
            "sourceInventoryPath": SOURCE_PATH.as_posix(),
            "sourceInventorySha256": _sha256(root / SOURCE_PATH),
        },
        "claimCompatibility": claim_compatibility,
        "completionRule": (
            "claimCompatibility may become true only after every required entry is "
            "implemented or an approved intentional deviation, every required entry has "
            "official Platform-Tools 37.0.1 differential evidence from a Release artifact "
            "built at the current source commit (or an evidence-only descendant), and no "
            "partial, missing, unknown, or unassessed entry remains"
        ),
        "documentType": "kairosboot.fastboot-compatibility-inventory",
        "entries": entries,
        "generationInputs": {
            "capabilityEvidence": EVIDENCE_PATH.as_posix(),
            "cHeader": C_HEADER.as_posix(),
            "cliRegistry": CLI_SOURCE.as_posix(),
            "officialDifferentialEvidence": OFFICIAL_EVIDENCE_PATH.as_posix(),
        },
        "inventoryStatus": "generated-known-gaps",
        "officialDifferentialCoverage": {
            "requiredEntries": len(required_entries),
            "requiredEntriesWithEvidence": required_with_evidence,
            "matchedScenarios": differential_scenarios,
            "uncoveredCandidateScenarios": uncovered_scenarios,
            "status": coverage_status,
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
    return inventory


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
        inventory = generate(args.repository_root)
        json_text = _canonical_json(inventory)
        if args.check:
            _check_file(args.repository_root / JSON_OUTPUT, json_text)
        else:
            (args.repository_root / JSON_OUTPUT).write_text(json_text, encoding="utf-8")
        summary = inventory["summary"]["byStatus"]
        print(
            "compatibility inventory: "
            + ", ".join(f"{status}={summary[status]}" for status in sorted(summary))
            + f", required-gaps={len(inventory['requiredGaps'])}, "
            + f"official-evidence={inventory['officialDifferentialCoverage']['requiredEntriesWithEvidence']}, "
            + f"claim={'true' if inventory['claimCompatibility'] else 'false'}"
        )
        return 0
    except InventoryError as error:
        print(f"compatibility inventory error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
