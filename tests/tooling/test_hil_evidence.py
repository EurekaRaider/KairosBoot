#!/usr/bin/env python3
"""Deterministic tests for the release HIL evidence gate."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "validate_hil_evidence", ROOT / "scripts" / "validate_hil_evidence.py"
)
assert SPEC is not None and SPEC.loader is not None
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)
SOAK_SPEC = importlib.util.spec_from_file_location(
    "run_soak_harness", ROOT / "scripts" / "run_soak_harness.py"
)
assert SOAK_SPEC is not None and SOAK_SPEC.loader is not None
SOAK_HARNESS = importlib.util.module_from_spec(SOAK_SPEC)
SOAK_SPEC.loader.exec_module(SOAK_HARNESS)
COMMIT = "a" * 40
RUN_ID = "12345678-1234-4123-8123-123456789abc"


def valid_evidence() -> dict[str, object]:
    device_ids = [hashlib.sha256(f"device-{index}".encode()).hexdigest()
                  for index in range(32)]
    devices = [
        {
            "idHash": identifier,
            "product": "test_product",
            "usbPath": f"1-{index + 1}",
            "controllerId": "xhci-0",
        }
        for index, identifier in enumerate(device_ids)
    ]
    six = lambda value: [value] * 6
    benchmark = {
        "schemaVersion": 1,
        "evidenceKind": "hardware",
        "commit": COMMIT,
        "runId": RUN_ID,
        "recordedAt": "2026-08-28T00:00:00Z",
        "harness": {
            "name": "kairosboot-hil-test-fixture",
            "version": "1",
            "sourceSha256": "b" * 64,
        },
        "build": {
            "configuration": "Release",
            "kairosbootVersion": "0.1.0",
            "kairosbootBinarySha256": "c" * 64,
            "aospFastbootVersion": "37.0.1",
            "aospFastbootBinarySha256": "d" * 64,
            "libusbVersion": "1.0.30",
        },
        "host": {"os": "linux", "arch": "x64", "machineIdHash": "e" * 64},
        "devices": devices,
        "rawBulkCeiling": {
            "deviceIdHash": device_ids[0],
            "samplesBytesPerSecond": six(1000),
        },
        "singleDeviceDownload": {
            "deviceIdHash": device_ids[0],
            "kairosbootSamplesBytesPerSecond": six(900),
            "aospFastbootSamplesBytesPerSecond": six(800),
        },
        "scenarios": [
            {
                "id": "host-bound",
                "weight": 3,
                "kairosbootSamplesBytesPerSecond": six(880),
                "aospFastbootSamplesBytesPerSecond": six(800),
            },
            {
                "id": "ceiling-bound",
                "weight": 1,
                "kairosbootSamplesBytesPerSecond": six(941),
                "aospFastbootSamplesBytesPerSecond": six(970),
            },
        ],
        "fleet": {
            "aospProcessMakespanSamplesMilliseconds": six(1100),
            "kairosbootMakespanSamplesMilliseconds": six(1000),
            "controllers": [
                {
                    "id": "xhci-0",
                    "rawBulkCeilingSamplesBytesPerSecond": six(32000),
                    "kairosbootAggregateSamplesBytesPerSecond": six(28800),
                }
            ],
            "fairnessWindows": [
                {
                    "durationMilliseconds": 5000,
                    "devices": [
                        {"idHash": identifier, "bytesPerSecond": 1000}
                        for identifier in device_ids
                    ],
                }
            ],
        },
    }
    return {
        "schemaVersion": 1,
        "evidenceKind": "hardware",
        "commit": COMMIT,
        "runId": RUN_ID,
        "lab": {"id": "test-lab", "operatorIdHash": "f" * 64},
        "benchmark": benchmark,
        "soak": {
            "requestedDurationSeconds": 86400,
            "completedDurationSeconds": 86400,
            "startedAt": "2026-08-27T00:00:00Z",
            "finishedAt": "2026-08-28T00:00:00Z",
            "sampleIntervalSeconds": 60,
            "cycles": 1440,
            "deadlocks": 0,
            "transferLeaks": 0,
            "handleLeaks": 0,
            "deviceMisrouting": 0,
            "sustainedRssGrowth": False,
        },
    }


class HilEvidenceTests(unittest.TestCase):
    def assert_rejected(self, evidence: dict[str, object]) -> None:
        with self.assertRaises(SystemExit):
            VALIDATOR.validate(evidence, COMMIT)

    def test_accepts_boundary_evidence(self) -> None:
        metrics = VALIDATOR.validate(valid_evidence(), COMMIT)
        self.assertAlmostEqual(metrics["singleDeviceDownloadCeilingUtilization"], 0.9)
        self.assertAlmostEqual(metrics["batchMakespanSpeedup"], 1.1)
        self.assertAlmostEqual(metrics["minimumJainFairness"], 1.0)

    def test_enforces_single_device_ceiling(self) -> None:
        evidence = valid_evidence()
        evidence["benchmark"]["singleDeviceDownload"][
            "kairosbootSamplesBytesPerSecond"
        ] = [899] * 6
        self.assert_rejected(evidence)

    def test_uses_scenario_weights(self) -> None:
        evidence = valid_evidence()
        evidence["benchmark"]["scenarios"] = [
            {
                "id": "dominant",
                "weight": 9,
                "kairosbootSamplesBytesPerSecond": [856] * 6,
                "aospFastbootSamplesBytesPerSecond": [800] * 6,
            },
            {
                "id": "minor",
                "weight": 1,
                "kairosbootSamplesBytesPerSecond": [1040] * 6,
                "aospFastbootSamplesBytesPerSecond": [800] * 6,
            },
        ]
        self.assert_rejected(evidence)

    def test_rejects_significant_ceiling_bound_regression(self) -> None:
        evidence = valid_evidence()
        scenarios = copy.deepcopy(evidence["benchmark"]["scenarios"])
        assert isinstance(scenarios, list)
        scenarios[1]["kairosbootSamplesBytesPerSecond"] = [939] * 6
        evidence["benchmark"]["scenarios"] = scenarios
        self.assert_rejected(evidence)

    def test_rejects_non_release_or_non_hardware_evidence(self) -> None:
        evidence = valid_evidence()
        evidence["benchmark"]["build"]["configuration"] = "Debug"
        self.assert_rejected(evidence)
        evidence = valid_evidence()
        evidence["evidenceKind"] = "synthetic"
        self.assert_rejected(evidence)

    def test_recomputes_controller_and_fairness_gates(self) -> None:
        evidence = valid_evidence()
        evidence["benchmark"]["fleet"]["controllers"][0][
            "kairosbootAggregateSamplesBytesPerSecond"
        ] = [28700] * 6
        self.assert_rejected(evidence)
        evidence = valid_evidence()
        window = evidence["benchmark"]["fleet"]["fairnessWindows"][0]
        for device in window["devices"][:8]:
            device["bytesPerSecond"] = 1
        self.assert_rejected(evidence)

    def test_default_soak_is_24_hours_but_tests_can_be_short(self) -> None:
        evidence = valid_evidence()
        evidence["soak"].update({
            "requestedDurationSeconds": 36,
            "completedDurationSeconds": 36,
            "startedAt": "2026-08-27T00:00:00Z",
            "finishedAt": "2026-08-27T00:00:36Z",
            "sampleIntervalSeconds": 1,
            "cycles": 36,
        })
        self.assert_rejected(evidence)
        VALIDATOR.validate(evidence, COMMIT, minimum_soak_hours=0.01)

    def test_rejects_inventory_or_soak_inconsistency(self) -> None:
        evidence = valid_evidence()
        evidence["benchmark"]["devices"][1]["usbPath"] = "1-1"
        self.assert_rejected(evidence)
        evidence = valid_evidence()
        evidence["soak"]["completedDurationSeconds"] = 90000
        self.assert_rejected(evidence)

    def test_rejects_non_finite_or_extra_fields(self) -> None:
        evidence = valid_evidence()
        evidence["benchmark"]["rawBulkCeiling"]["samplesBytesPerSecond"][0] = float("nan")
        self.assert_rejected(evidence)
        evidence = valid_evidence()
        evidence["inventedPass"] = True
        self.assert_rejected(evidence)


class FakeClock:
    def __init__(self) -> None:
        self.value = 0.0

    def now(self) -> float:
        return self.value

    def sleep(self, seconds: float) -> None:
        self.value += seconds


class SoakHarnessTests(unittest.TestCase):
    def test_short_run_is_raw_observation_not_qualification(self) -> None:
        clock = FakeClock()
        exit_codes = iter([0, 7, 0])
        wall_times = iter(["2026-08-28T00:00:00Z", "2026-08-28T00:00:03Z"])
        result = SOAK_HARNESS.run_soak(
            ["device-cycle"],
            3.0,
            1.0,
            monotonic=clock.now,
            sleep=clock.sleep,
            invoke=lambda _: next(exit_codes),
            wall_time=lambda: next(wall_times),
        )
        self.assertEqual(result["qualificationStatus"], "raw-observation-only")
        self.assertEqual(result["completedDurationSeconds"], 3.0)
        self.assertEqual(result["cycles"], 3)
        self.assertEqual(result["cycleFailures"], 1)
        self.assertEqual(result["exitCodes"], [0, 7, 0])

    def test_rejects_unbounded_or_commandless_run(self) -> None:
        with self.assertRaises(ValueError):
            SOAK_HARNESS.run_soak([], 1.0, 1.0)
        with self.assertRaises(ValueError):
            SOAK_HARNESS.run_soak(["cycle"], 0.0, 1.0)
        with self.assertRaises(ValueError):
            SOAK_HARNESS.run_soak(["cycle"], 1.0, 0.0)


if __name__ == "__main__":
    unittest.main()
