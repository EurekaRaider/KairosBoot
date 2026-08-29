# SPDX-License-Identifier: MIT
"""Contract tests for the generated Platform-Tools compatibility inventory."""

from __future__ import annotations

import importlib.util
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = REPOSITORY_ROOT / "scripts" / "generate_compatibility_inventory.py"
SPEC = importlib.util.spec_from_file_location("compatibility_inventory", GENERATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import {GENERATOR_PATH}")
GENERATOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GENERATOR
SPEC.loader.exec_module(GENERATOR)


class CompatibilityInventoryTests(unittest.TestCase):
    def test_locked_37_0_1_options_do_not_misclassify_verbose_as_vendor_id(
        self,
    ) -> None:
        source = json.loads(
            (REPOSITORY_ROOT / GENERATOR.SOURCE_PATH).read_text(encoding="utf-8")
        )
        entries = {entry["id"]: entry for entry in source["entries"]}
        self.assertNotIn("option.vendor-id", entries)
        self.assertEqual(entries["option.verbose"]["spelling"], "--verbose")

    def test_checked_outputs_are_exact_and_have_no_unknown_state(self) -> None:
        inventory, yaml_text = GENERATOR.generate(REPOSITORY_ROOT)
        self.assertFalse(inventory["claimCompatibility"])
        self.assertEqual(
            set(inventory["statusVocabulary"]), GENERATOR.ALLOWED_STATUSES
        )
        expected_required_gaps: list[str] = []
        self.assertEqual(inventory["requiredGaps"], expected_required_gaps)
        self.assertEqual(
            inventory["officialDifferentialCoverage"]["status"], "partial"
        )
        self.assertEqual(
            inventory["officialDifferentialCoverage"]["requiredEntriesWithEvidence"],
            51,
        )
        self.assertEqual(
            inventory["officialDifferentialCoverage"]["matchedScenarios"], 37
        )
        for identifier in expected_required_gaps:
            self.assertIn(f'  - "{identifier}"\n', yaml_text)
        self.assertIn("requiredGaps: []\n", yaml_text)
        for entry in inventory["entries"]:
            self.assertIn(entry["status"], GENERATOR.ALLOWED_STATUSES)
            self.assertNotIn("unknown", entry["status"])
        self.assertEqual(
            (REPOSITORY_ROOT / GENERATOR.JSON_OUTPUT).read_text(encoding="utf-8"),
            GENERATOR._canonical_json(inventory),
        )
        self.assertEqual(
            (REPOSITORY_ROOT / GENERATOR.YAML_OUTPUT).read_text(encoding="utf-8"),
            yaml_text,
        )

    def test_frozen_image_inventory_matches_independent_cpp_oracle(self) -> None:
        source = json.loads(
            (REPOSITORY_ROOT / GENERATOR.SOURCE_PATH).read_text(encoding="utf-8")
        )
        source_images = {
            (entry["file"], entry["partition"])
            for entry in source["entries"]
            if entry["kind"] == "image"
        }
        oracle = (
            REPOSITORY_ROOT
            / "tests/fastboot/aosp_hardcoded_image_inventory_37_0_1.hpp"
        ).read_text(encoding="utf-8")
        oracle_images = set(
            re.findall(
                r'Image\{"[^"]*",\s*"([^"]+)",\s*"[^"]*",\s*"([^"]+)"',
                oracle,
            )
        )
        self.assertEqual(len(source_images), 27)
        self.assertEqual(source_images, oracle_images)

    def _fixture_root(self, directory: Path) -> Path:
        for path in (
            GENERATOR.SOURCE_PATH,
            GENERATOR.EVIDENCE_PATH,
            GENERATOR.LOCK_PATH,
            GENERATOR.OFFICIAL_EVIDENCE_PATH,
            GENERATOR.C_HEADER,
            GENERATOR.CLI_SOURCE,
        ):
            destination = directory / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPOSITORY_ROOT / path, destination)
        official_index = json.loads(
            (directory / GENERATOR.OFFICIAL_EVIDENCE_PATH).read_text()
        )
        for metadata_value in official_index["captures"]:
            metadata_path = Path(metadata_value)
            destination = directory / metadata_path
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPOSITORY_ROOT / metadata_path, destination)
            metadata = json.loads(destination.read_text())
            for descriptor in metadata["captureFiles"].values():
                source = REPOSITORY_ROOT / metadata_path.parent / descriptor["path"]
                target = directory / metadata_path.parent / descriptor["path"]
                shutil.copy2(source, target)
        evidence = json.loads((directory / GENERATOR.EVIDENCE_PATH).read_text())
        for group in [*evidence["assessments"], *evidence["additionalDeviations"]]:
            for raw_path in group.get("evidence", []):
                path = directory / raw_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch(exist_ok=True)
        return directory

    def _mutate_evidence(self, root: Path, mutation) -> None:
        path = root / GENERATOR.EVIDENCE_PATH
        evidence = json.loads(path.read_text())
        mutation(evidence)
        path.write_text(json.dumps(evidence), encoding="utf-8")

    def test_rejects_unassessed_upstream_entry(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))

            def remove_first_id(evidence) -> None:
                group = next(
                    item for item in evidence["assessments"] if len(item["ids"]) > 1
                )
                group["ids"].pop()

            self._mutate_evidence(root, remove_first_id)
            with self.assertRaisesRegex(GENERATOR.InventoryError, "unassessed"):
                GENERATOR.generate(root)

    def test_rejects_unknown_status(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))

            def introduce_unknown(evidence) -> None:
                evidence["assessments"][0]["status"] = "unknown"

            self._mutate_evidence(root, introduce_unknown)
            with self.assertRaisesRegex(GENERATOR.InventoryError, "forbidden or unknown"):
                GENERATOR.generate(root)

    def test_rejects_stale_public_symbol_and_cli_registry_claims(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))

            def stale_symbol(evidence) -> None:
                group = next(
                    item for item in evidence["assessments"] if item.get("cSymbols")
                )
                group["cSymbols"] = ["kb_absent_symbol"]

            self._mutate_evidence(root, stale_symbol)
            with self.assertRaisesRegex(GENERATOR.InventoryError, "public C symbol"):
                GENERATOR.generate(root)

        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))

            def stale_cli(evidence) -> None:
                group = next(
                    item for item in evidence["assessments"] if item.get("cliCommands")
                )
                group["cliCommands"] = ["absent-command"]

            self._mutate_evidence(root, stale_cli)
            with self.assertRaisesRegex(GENERATOR.InventoryError, "CLI spelling"):
                GENERATOR.generate(root)

    def test_rejects_frozen_source_inventory_hash_drift(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            path = root / GENERATOR.SOURCE_PATH
            source = json.loads(path.read_text())
            source["entries"][0]["required"] = False
            path.write_text(json.dumps(source), encoding="utf-8")
            with self.assertRaisesRegex(GENERATOR.InventoryError, "SHA-256"):
                GENERATOR.generate(root)

    def test_manual_generated_file_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            inventory, yaml_text = GENERATOR.generate(root)
            json_path = root / GENERATOR.JSON_OUTPUT
            yaml_path = root / GENERATOR.YAML_OUTPUT
            json_path.write_text(GENERATOR._canonical_json(inventory), encoding="utf-8")
            yaml_path.write_text(yaml_text, encoding="utf-8")
            GENERATOR._check_file(json_path, GENERATOR._canonical_json(inventory))
            json_path.write_text("{}\n", encoding="utf-8")
            with self.assertRaisesRegex(GENERATOR.InventoryError, "stale"):
                GENERATOR._check_file(json_path, GENERATOR._canonical_json(inventory))

    def test_rejects_differential_trace_mismatch_even_with_updated_hash(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            index = json.loads(
                (root / GENERATOR.OFFICIAL_EVIDENCE_PATH).read_text()
            )
            metadata_path = root / index["captures"][0]
            metadata = json.loads(metadata_path.read_text())
            descriptor = metadata["captureFiles"]["kairosboot"]
            capture_path = metadata_path.parent / descriptor["path"]
            capture = json.loads(capture_path.read_text())
            capture["scenarios"][0]["deviceState"]["product"] = "tampered"
            capture_path.write_text(json.dumps(capture), encoding="utf-8")
            descriptor["sha256"] = GENERATOR._sha256(capture_path)
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(
                GENERATOR.InventoryError, "normalized captures do not match"
            ):
                GENERATOR.generate(root)

    def test_rejects_self_reported_matched_coverage_id(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            index = json.loads(
                (root / GENERATOR.OFFICIAL_EVIDENCE_PATH).read_text()
            )
            metadata_path = root / index["captures"][0]
            metadata = json.loads(metadata_path.read_text())
            metadata["scenarios"][0]["coverageIds"].append("command.erase")
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(
                GENERATOR.InventoryError,
                "coverageIds differ from the scenario whitelist",
            ):
                GENERATOR.generate(root)

    def test_rejects_matched_capture_with_different_command_semantics(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            index = json.loads(
                (root / GENERATOR.OFFICIAL_EVIDENCE_PATH).read_text()
            )
            metadata_path = root / index["captures"][0]
            metadata = json.loads(metadata_path.read_text())
            for descriptor in metadata["captureFiles"].values():
                capture_path = metadata_path.parent / descriptor["path"]
                capture = json.loads(capture_path.read_text())
                capture["scenarios"][0]["events"][0]["argv"] = ["erase", "system"]
                capture_path.write_text(json.dumps(capture), encoding="utf-8")
                descriptor["sha256"] = GENERATOR._sha256(capture_path)
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(
                GENERATOR.InventoryError, "CLI command semantics differ"
            ):
                GENERATOR.generate(root)

    def test_rejects_uncovered_coverage_reclassified_as_matched(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            index = json.loads(
                (root / GENERATOR.OFFICIAL_EVIDENCE_PATH).read_text()
            )
            metadata_path = root / index["captures"][0]
            metadata = json.loads(metadata_path.read_text())
            metadata["uncoveredScenarios"][0]["coverageIds"] = ["command.getvar"]
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(
                GENERATOR.InventoryError,
                "uncovered coverageIds differ from the scenario whitelist",
            ):
                GENERATOR.generate(root)

    def test_release_artifact_provenance_must_match_binary_hash(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            index = json.loads(
                (root / GENERATOR.OFFICIAL_EVIDENCE_PATH).read_text()
            )
            metadata_path = root / index["captures"][0]
            metadata = json.loads(metadata_path.read_text())
            metadata["kairosboot"]["releaseArtifactSha256"] = "0" * 64
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(
                GENERATOR.InventoryError, "Release artifact provenance is invalid"
            ):
                GENERATOR.generate(root)

    def test_only_exact_head_capture_counts_as_official_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            index = json.loads(
                (root / GENERATOR.OFFICIAL_EVIDENCE_PATH).read_text()
            )
            metadata_path = root / index["captures"][0]
            metadata = json.loads(metadata_path.read_text())
            capture_commit = metadata["kairosboot"]["sourceCommit"]

            with mock.patch.object(
                GENERATOR, "_repository_commit", return_value=capture_commit
            ):
                current, _ = GENERATOR.generate(root)
            self.assertEqual(
                current["officialDifferentialCoverage"]["matchedScenarios"], 37
            )
            self.assertEqual(
                current["officialDifferentialCoverage"][
                    "requiredEntriesWithEvidence"
                ],
                51,
            )
            self.assertFalse(current["claimCompatibility"])

            with mock.patch.object(
                GENERATOR, "_repository_commit", return_value="f" * 40
            ):
                stale, _ = GENERATOR.generate(root)
            self.assertEqual(
                stale["officialDifferentialCoverage"]["matchedScenarios"], 0
            )
            self.assertEqual(
                stale["officialDifferentialCoverage"][
                    "requiredEntriesWithEvidence"
                ],
                0,
            )
            self.assertTrue(
                all(not entry["officialDifferentialEvidence"] for entry in stale["entries"])
            )
            self.assertFalse(stale["claimCompatibility"])

    def test_persisted_capture_allows_only_an_evidence_only_descendant(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)

            def git(*arguments: str) -> str:
                completed = subprocess.run(
                    ["git", "-C", str(root), *arguments],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                return completed.stdout.strip()

            git("init")
            git("config", "user.name", "KairosBoot Test")
            git("config", "user.email", "kairosboot@example.invalid")
            (root / "src").mkdir()
            (root / "src" / "product.cpp").write_text("v1\n", encoding="utf-8")
            git("add", "src/product.cpp")
            git("commit", "-m", "source under test")
            capture_commit = git("rev-parse", "HEAD")
            self.assertTrue(
                GENERATOR._capture_matches_current_repository(
                    root, capture_commit, capture_commit
                )
            )

            (root / "compat" / "evidence").mkdir(parents=True)
            (root / "compat" / "evidence" / "capture.json").write_text(
                "{}\n", encoding="utf-8"
            )
            git("add", "compat/evidence/capture.json")
            git("commit", "-m", "persist evidence")
            evidence_commit = git("rev-parse", "HEAD")
            self.assertTrue(
                GENERATOR._capture_matches_current_repository(
                    root, capture_commit, evidence_commit
                )
            )

            (root / "src" / "product.cpp").write_text("v2\n", encoding="utf-8")
            git("add", "src/product.cpp")
            git("commit", "-m", "change product")
            changed_commit = git("rev-parse", "HEAD")
            self.assertFalse(
                GENERATOR._capture_matches_current_repository(
                    root, capture_commit, changed_commit
                )
            )

    def test_mismatched_capture_is_rejected_before_claim_evaluation(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = self._fixture_root(Path(raw))
            index = json.loads(
                (root / GENERATOR.OFFICIAL_EVIDENCE_PATH).read_text()
            )
            metadata_path = root / index["captures"][0]
            metadata = json.loads(metadata_path.read_text())
            metadata["result"] = "mismatched"
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(
                GENERATOR.InventoryError, "evidence is not a matched v1 capture"
            ):
                GENERATOR.generate(root)


if __name__ == "__main__":
    unittest.main()
