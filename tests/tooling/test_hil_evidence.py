#!/usr/bin/env python3
"""Deterministic tests for the release HIL evidence gate."""

from __future__ import annotations

import copy
import importlib.util
import math
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "validate_hil_evidence", ROOT / "scripts" / "validate_hil_evidence.py"
)
assert SPEC is not None and SPEC.loader is not None
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)
COMMIT = "a" * 40


def valid_evidence() -> dict[str, object]:
    return {
        "schemaVersion": 1,
        "commit": COMMIT,
        "deviceCount": 32,
        "soakHours": 24,
        "singleDeviceDownloadCeilingUtilization": 0.90,
        "minimumJainFairness": 0.95,
        "batchMakespanSpeedup": 1.10,
        "controllers": [{"id": "xhci-0", "ceilingUtilization": 0.90}],
        "scenarios": [
            {
                "id": "large-image",
                "weight": 3,
                "fastbootCeilingUtilization": 0.80,
                "relativeDelta": 0.12,
                "statisticallySignificant": True,
            },
            {
                "id": "device-bound",
                "weight": 1,
                "fastbootCeilingUtilization": 0.97,
                "relativeDelta": -0.03,
                "statisticallySignificant": True,
            },
        ],
        "deadlocks": 0,
        "transferLeaks": 0,
        "handleLeaks": 0,
        "deviceMisrouting": 0,
        "sustainedRssGrowth": False,
    }


class HilEvidenceTests(unittest.TestCase):
    def assert_rejected(self, evidence: dict[str, object]) -> None:
        with self.assertRaises(SystemExit):
            VALIDATOR.validate(evidence, COMMIT)

    def test_accepts_boundary_evidence(self) -> None:
        VALIDATOR.validate(valid_evidence(), COMMIT)

    def test_enforces_single_device_ceiling(self) -> None:
        evidence = valid_evidence()
        evidence["singleDeviceDownloadCeilingUtilization"] = 0.899
        self.assert_rejected(evidence)

    def test_uses_scenario_weights(self) -> None:
        evidence = valid_evidence()
        evidence["scenarios"] = [
            {
                "id": "dominant",
                "weight": 9,
                "fastbootCeilingUtilization": 0.80,
                "relativeDelta": 0.07,
                "statisticallySignificant": True,
            },
            {
                "id": "minor",
                "weight": 1,
                "fastbootCeilingUtilization": 0.80,
                "relativeDelta": 0.30,
                "statisticallySignificant": True,
            },
        ]
        unweighted = math.sqrt(1.07 * 1.30)
        self.assertGreater(unweighted, 1.10)
        self.assert_rejected(evidence)

    def test_rejects_significant_ceiling_bound_regression(self) -> None:
        evidence = valid_evidence()
        scenarios = copy.deepcopy(evidence["scenarios"])
        assert isinstance(scenarios, list)
        scenarios[1]["relativeDelta"] = -0.031
        evidence["scenarios"] = scenarios
        self.assert_rejected(evidence)

    def test_rejects_non_finite_and_extra_fields(self) -> None:
        evidence = valid_evidence()
        evidence["soakHours"] = float("nan")
        self.assert_rejected(evidence)
        evidence = valid_evidence()
        evidence["inventedPass"] = True
        self.assert_rejected(evidence)


if __name__ == "__main__":
    unittest.main()
