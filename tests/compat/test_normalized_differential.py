# SPDX-License-Identifier: MIT
"""Contract tests for the AOSP/KairosBoot normalized trace comparator."""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


REFERENCE_TEMP = "/private/tmp/kairosboot-scripted-reference"
ACTUAL_TEMP = "/tmp/kairosboot-scripted-actual"
REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]


def replace_text(value: object, old: str, new: str) -> object:
    if isinstance(value, str):
        return value.replace(old, new)
    if isinstance(value, list):
        return [replace_text(item, old, new) for item in value]
    if isinstance(value, dict):
        return {key: replace_text(item, old, new) for key, item in value.items()}
    return value


class NormalizedDifferentialTests(unittest.TestCase):
    root: pathlib.Path
    comparator_path: pathlib.Path
    schema_path: pathlib.Path
    fixture_path: pathlib.Path
    comparator: object
    fixture: dict[str, object]

    @classmethod
    def setUpClass(cls) -> None:
        cls.root = REPOSITORY_ROOT
        cls.comparator_path = cls.root / "scripts" / "compare_fastboot_traces.py"
        cls.schema_path = (
            cls.root / "tests" / "compat" / "normalized-fastboot-trace.schema.json"
        )
        cls.fixture_path = (
            cls.root / "tests" / "compat" / "fixtures" /
            "scripted-normalized-traces.json"
        )
        spec = importlib.util.spec_from_file_location(
            "compare_fastboot_traces", cls.comparator_path
        )
        if spec is None or spec.loader is None:
            raise AssertionError("unable to load normalized differential comparator")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        cls.comparator = module
        cls.fixture = module.load_capture(cls.fixture_path, cls.schema_path)

    def actual_capture(self) -> dict[str, object]:
        result = replace_text(copy.deepcopy(self.fixture), REFERENCE_TEMP, ACTUAL_TEMP)
        assert isinstance(result, dict)
        for scenario_index, scenario in enumerate(result["scenarios"]):
            for event_index, event in enumerate(scenario["events"]):
                if "timestampNs" in event:
                    event["timestampNs"] += 1_000_000 + scenario_index * 1000 + event_index
                event["elapsedMs"] = scenario_index * 10 + event_index
        return result

    def test_scripted_fixture_covers_required_event_families(self) -> None:
        scenarios = {item["id"]: item for item in self.fixture["scenarios"]}
        self.assertEqual(
            set(scenarios),
            {
                "success-getvar",
                "success-download-flash-info-text",
                "device-fail",
                "success-upload-file",
                "success-fetch-file",
                "cli-parse-error",
            },
        )
        kinds = {
            event["kind"]
            for scenario in self.fixture["scenarios"]
            for event in scenario["events"]
        }
        self.assertTrue(
            {"CLI_PARSE", "COMMAND", "GETVAR", "DATA", "INFO", "TEXT",
             "OKAY", "FAIL", "FILE", "EXIT"}.issubset(kinds)
        )
        for scenario in self.fixture["scenarios"]:
            self.assertEqual(scenario["events"][0]["kind"], "CLI_PARSE")
            self.assertEqual(scenario["events"][-1]["kind"], "EXIT")
            for event in scenario["events"]:
                if event["kind"] in {"DATA", "FILE"}:
                    self.assertIn("size", event)
                    self.assertIn("sha256", event)
                    self.assertNotIn("payload", event)

    def test_only_time_and_declared_temporary_roots_are_normalized(self) -> None:
        difference = self.comparator.compare_captures(
            self.fixture,
            self.actual_capture(),
            [REFERENCE_TEMP],
            [ACTUAL_TEMP],
        )
        self.assertIsNone(difference)

    def test_device_state_timing_named_fields_remain_semantic(self) -> None:
        expected = copy.deepcopy(self.fixture)
        expected["scenarios"][0]["deviceState"]["timestampNs"] = 1
        actual = self.actual_capture()
        actual["scenarios"][0]["deviceState"]["timestampNs"] = 2
        difference = self.comparator.compare_captures(
            expected, actual, [REFERENCE_TEMP], [ACTUAL_TEMP]
        )
        self.assertEqual(
            difference.path, "$.scenarios[0].deviceState.timestampNs"
        )

    def test_event_order_is_not_normalized(self) -> None:
        actual = self.actual_capture()
        events = actual["scenarios"][1]["events"]
        events[7], events[8] = events[8], events[7]
        difference = self.comparator.compare_captures(
            self.fixture, actual, [REFERENCE_TEMP], [ACTUAL_TEMP]
        )
        self.assertIsNotNone(difference)
        self.assertEqual(difference.path, "$.scenarios[1].events[7].kind")

    def test_first_semantic_mismatch_reports_expected_and_actual(self) -> None:
        actual = self.actual_capture()
        actual["scenarios"][1]["events"][3]["command"] = "download:00000012"
        actual["scenarios"][1]["deviceState"]["lateDifference"] = True
        difference = self.comparator.compare_captures(
            self.fixture, actual, [REFERENCE_TEMP], [ACTUAL_TEMP]
        )
        self.assertIsNotNone(difference)
        self.assertEqual(
            difference.path, "$.scenarios[1].events[3].command"
        )
        self.assertEqual(difference.expected, "download:00000011")
        self.assertEqual(difference.actual, "download:00000012")

    def test_payload_size_hash_and_temporary_tail_remain_semantic(self) -> None:
        actual = self.actual_capture()
        actual["scenarios"][1]["events"][4]["size"] = 18
        difference = self.comparator.compare_captures(
            self.fixture, actual, [REFERENCE_TEMP], [ACTUAL_TEMP]
        )
        self.assertEqual(difference.path, "$.scenarios[1].events[4].size")

        actual = self.actual_capture()
        actual["scenarios"][4]["events"][5]["path"] = (
            ACTUAL_TEMP + "/output/system.img"
        )
        difference = self.comparator.compare_captures(
            self.fixture, actual, [REFERENCE_TEMP], [ACTUAL_TEMP]
        )
        self.assertEqual(difference.path, "$.scenarios[4].events[5].path")

    def test_cli_arguments_exit_code_and_device_state_are_not_normalized(self) -> None:
        actual = self.actual_capture()
        actual["scenarios"][0]["events"][0]["argv"][1] = "serialno"
        difference = self.comparator.compare_captures(
            self.fixture, actual, [REFERENCE_TEMP], [ACTUAL_TEMP]
        )
        self.assertEqual(
            difference.path, "$.scenarios[0].events[0].argv[1]"
        )

        actual = self.actual_capture()
        actual["scenarios"][0]["events"][-1]["code"] = 1
        difference = self.comparator.compare_captures(
            self.fixture, actual, [REFERENCE_TEMP], [ACTUAL_TEMP]
        )
        self.assertEqual(difference.path, "$.scenarios[0].events[3].code")

        actual = self.actual_capture()
        actual["scenarios"][0]["deviceState"]["product"] = "product_b"
        difference = self.comparator.compare_captures(
            self.fixture, actual, [REFERENCE_TEMP], [ACTUAL_TEMP]
        )
        self.assertEqual(
            difference.path, "$.scenarios[0].deviceState.product"
        )

    def test_schema_rejects_payload_bytes_and_unknown_timing_fields(self) -> None:
        invalid = copy.deepcopy(self.fixture)
        invalid["scenarios"][1]["events"][4]["payload"] = "secret bytes"
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "invalid.json"
            path.write_text(json.dumps(invalid), encoding="utf-8")
            with self.assertRaises(self.comparator.TraceContractError):
                self.comparator.load_capture(path, self.schema_path)

        invalid = copy.deepcopy(self.fixture)
        invalid["scenarios"][0]["events"][0]["duration"] = 3
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "invalid.json"
            path.write_text(json.dumps(invalid), encoding="utf-8")
            with self.assertRaises(self.comparator.TraceContractError):
                self.comparator.load_capture(path, self.schema_path)

    def test_cli_mismatch_is_concise_and_contains_no_payload_bytes(self) -> None:
        actual = self.actual_capture()
        actual["scenarios"][1]["events"][4]["sha256"] = "0" * 64
        with tempfile.TemporaryDirectory() as directory:
            actual_path = pathlib.Path(directory) / "actual.json"
            actual_path.write_text(json.dumps(actual), encoding="utf-8")
            completed = subprocess.run(
                [
                    sys.executable,
                    str(self.comparator_path),
                    "--schema",
                    str(self.schema_path),
                    "--expected",
                    str(self.fixture_path),
                    "--actual",
                    str(actual_path),
                    "--expected-temp-root",
                    REFERENCE_TEMP,
                    "--actual-temp-root",
                    ACTUAL_TEMP,
                ],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(completed.returncode, 1)
        self.assertIn("$.scenarios[1].events[4].sha256", completed.stderr)
        self.assertIn("expected:", completed.stderr)
        self.assertIn("actual:", completed.stderr)
        self.assertNotIn("kairos-boot-image", completed.stderr)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    return parser.parse_args()


if __name__ == "__main__":
    arguments = parse_arguments()
    REPOSITORY_ROOT = arguments.repository_root.resolve()
    unittest.main(argv=[sys.argv[0]])
