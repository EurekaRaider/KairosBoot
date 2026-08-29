#!/usr/bin/env python3
"""Validate one platform's real-USB and fault-injection HIL evidence."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from performance_evidence import (  # noqa: E402
    GIT_SHA,
    RUN_ID,
    SUPPORTED_ARCHES,
    SUPPORTED_HOSTS,
    require,
    require_exact_keys,
    require_sha256,
    require_string,
    require_utc,
)


DOCUMENT_KEYS = {
    "schemaVersion",
    "evidenceKind",
    "commit",
    "runId",
    "recordedAt",
    "harness",
    "lab",
    "host",
    "devices",
    "scenarios",
}
HARNESS_KEYS = {"name", "version", "sourceSha256"}
LAB_KEYS = {"id", "operatorIdHash"}
HOST_KEYS = {"os", "arch", "machineIdHash"}
DEVICE_KEYS = {"idHash", "product", "usbPath", "controllerId"}
SCENARIO_KEYS = {"id", "attempts", "successes", "deviceIds", "evidenceSha256"}

REQUIRED_SCENARIOS = frozenset(
    {
        "device-side-download-hash",
        "duplicate-serial",
        "error-response",
        "fastbootd-reenumeration",
        "hub-reset",
        "nak-backpressure",
        "out-of-order-callback",
        "partial-io",
        "process-exit-cleanup",
        "short-packet",
        "stall",
        "timeout",
        "unplug",
        "zlp",
    }
)


def validate(data: object, expected_commit: str, expected_os: str) -> None:
    document = require_exact_keys(data, DOCUMENT_KEYS, "USB HIL evidence")
    require(GIT_SHA.fullmatch(expected_commit) is not None, "invalid expected commit")
    require(expected_os in SUPPORTED_HOSTS, "unsupported expected host OS")
    require(document["schemaVersion"] == 1, "unsupported USB HIL evidence schema")
    require(document["evidenceKind"] == "hardware", "USB HIL evidence is not hardware evidence")
    require(document["commit"] == expected_commit, "USB HIL evidence commit does not match")
    require(
        isinstance(document["runId"], str)
        and RUN_ID.fullmatch(document["runId"]) is not None,
        "invalid USB HIL runId",
    )
    require_utc(document["recordedAt"], "USB HIL recordedAt")

    harness = require_exact_keys(document["harness"], HARNESS_KEYS, "USB HIL harness")
    require_string(harness["name"], "USB HIL harness name")
    require_string(harness["version"], "USB HIL harness version")
    require_sha256(harness["sourceSha256"], "USB HIL harness sourceSha256")

    lab = require_exact_keys(document["lab"], LAB_KEYS, "USB HIL lab")
    require_string(lab["id"], "USB HIL lab id")
    require_sha256(lab["operatorIdHash"], "USB HIL operatorIdHash")

    host = require_exact_keys(document["host"], HOST_KEYS, "USB HIL host")
    require(host["os"] == expected_os, "USB HIL host OS does not match the matrix job")
    require(host["arch"] in SUPPORTED_ARCHES, "unsupported USB HIL host architecture")
    require_sha256(host["machineIdHash"], "USB HIL machineIdHash")

    devices = document["devices"]
    require(isinstance(devices, list) and bool(devices), "USB HIL has no real devices")
    device_ids: set[str] = set()
    usb_paths: set[str] = set()
    for index, raw_device in enumerate(devices):
        device = require_exact_keys(raw_device, DEVICE_KEYS, f"USB HIL device {index}")
        identifier = require_sha256(device["idHash"], f"USB HIL device {index} idHash")
        require(identifier not in device_ids, "USB HIL contains a duplicate device identity")
        device_ids.add(identifier)
        require_string(device["product"], f"USB HIL device {index} product")
        usb_path = require_string(device["usbPath"], f"USB HIL device {index} usbPath")
        require(usb_path not in usb_paths, "USB HIL contains a duplicate physical USB path")
        usb_paths.add(usb_path)
        require_string(device["controllerId"], f"USB HIL device {index} controllerId")

    scenarios = document["scenarios"]
    require(isinstance(scenarios, list), "USB HIL scenarios must be a list")
    observed: set[str] = set()
    for index, raw_scenario in enumerate(scenarios):
        scenario = require_exact_keys(raw_scenario, SCENARIO_KEYS, f"USB HIL scenario {index}")
        identifier = require_string(scenario["id"], f"USB HIL scenario {index} id")
        require(identifier in REQUIRED_SCENARIOS, f"unknown USB HIL scenario: {identifier}")
        require(identifier not in observed, f"duplicate USB HIL scenario: {identifier}")
        observed.add(identifier)
        attempts = scenario["attempts"]
        successes = scenario["successes"]
        require(
            isinstance(attempts, int)
            and not isinstance(attempts, bool)
            and attempts > 0,
            f"USB HIL scenario {identifier} has no attempts",
        )
        require(
            isinstance(successes, int)
            and not isinstance(successes, bool)
            and successes == attempts,
            f"USB HIL scenario {identifier} did not pass every attempt",
        )
        scenario_devices = scenario["deviceIds"]
        require(
            isinstance(scenario_devices, list)
            and bool(scenario_devices)
            and len(scenario_devices) == len(set(scenario_devices)),
            f"USB HIL scenario {identifier} has an invalid device set",
        )
        require(
            all(device in device_ids for device in scenario_devices),
            f"USB HIL scenario {identifier} names an unknown device",
        )
        require_sha256(scenario["evidenceSha256"], f"USB HIL scenario {identifier} evidenceSha256")

    require(observed == REQUIRED_SCENARIOS, "USB HIL evidence is missing required scenarios")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--os", required=True, choices=sorted(SUPPORTED_HOSTS))
    args = parser.parse_args()
    validate(
        json.loads(args.evidence.read_text(encoding="utf-8")),
        args.commit,
        args.os,
    )
    print(f"USB HIL evidence satisfies all {args.os} real-device fault gates")


if __name__ == "__main__":
    main()
