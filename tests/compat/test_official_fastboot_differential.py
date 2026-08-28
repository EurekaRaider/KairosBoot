# SPDX-License-Identifier: MIT
"""Fixture-only tests for the opt-in official Fastboot capture runner."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


TOOLING_ROOT = pathlib.Path(__file__).resolve().parents[1] / "tooling"
sys.path.insert(0, str(TOOLING_ROOT))
from json_schema_subset import check_schema, validate  # noqa: E402


class OfficialFastbootDifferentialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = REPOSITORY_ROOT
        path = cls.root / "scripts" / "run_official_fastboot_differential.py"
        spec = importlib.util.spec_from_file_location("official_capture_runner", path)
        if spec is None or spec.loader is None:
            raise AssertionError(f"cannot load {path}")
        cls.runner = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.runner
        spec.loader.exec_module(cls.runner)

    def test_repository_lock_pins_37_0_1_binary_hashes(self) -> None:
        lock = json.loads((self.root / "compat" / "aosp.lock.json").read_text())
        self.assertEqual(lock["aosp"]["platformToolsVersion"], "37.0.1")
        archives = lock["aosp"]["officialArchives"]
        self.assertEqual(set(archives), {"darwin", "linux", "windows"})
        for entry in archives.values():
            self.assertRegex(entry["fastbootSha256"], r"^[0-9a-f]{64}$")

    def _python_lock(self, directory: pathlib.Path, digest: str, version: str) -> pathlib.Path:
        platform_key = self.runner._platform_key()
        lock = {
            "aosp": {
                "platformToolsVersion": version,
                "officialArchives": {platform_key: {"fastbootSha256": digest}},
            }
        }
        path = directory / "lock.json"
        path.write_text(json.dumps(lock), encoding="utf-8")
        return path

    def test_binary_hash_and_version_are_both_enforced(self) -> None:
        binary = pathlib.Path(sys.executable).resolve()
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        version_output = subprocess.run(
            [str(binary), "--version"], check=True, capture_output=True, text=True
        )
        observed = version_output.stdout + version_output.stderr
        version = observed.strip().split()[1]
        with tempfile.TemporaryDirectory() as raw_directory:
            directory = pathlib.Path(raw_directory)
            verified = self.runner.verify_official_fastboot(
                binary, self._python_lock(directory, digest, version)
            )
            self.assertEqual(verified.sha256, digest)

            with self.assertRaisesRegex(
                self.runner.CaptureGateError, "SHA-256 mismatch"
            ):
                self.runner.verify_official_fastboot(
                    binary, self._python_lock(directory, "0" * 64, version)
                )
            with self.assertRaisesRegex(
                self.runner.CaptureGateError, "version mismatch"
            ):
                self.runner.verify_official_fastboot(
                    binary, self._python_lock(directory, digest, "99.99.99")
                )

    def test_missing_opt_in_is_explicit_skip_or_failure(self) -> None:
        arguments = argparse.Namespace(
            repository_root=self.root,
            lock=None,
            fastboot=None,
            kairosboot=None,
            output_dir=None,
            require=False,
        )
        self.assertEqual(
            self.runner._run_capture(arguments), self.runner.SKIP_EXIT_CODE
        )
        arguments.require = True
        with self.assertRaisesRegex(
            self.runner.CaptureGateError, "missing --fastboot, --kairosboot"
        ):
            self.runner._run_capture(arguments)

    def test_capture_provenance_requires_exact_clean_source_commit(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            repository = pathlib.Path(raw_directory)

            def git(*arguments: str) -> str:
                completed = subprocess.run(
                    ["git", "-C", str(repository), *arguments],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                return completed.stdout.strip()

            git("init")
            git("config", "user.name", "KairosBoot Test")
            git("config", "user.email", "kairosboot@example.invalid")
            source = repository / "source.txt"
            source.write_text("tested\n", encoding="utf-8")
            git("add", "source.txt")
            git("commit", "-m", "source under test")
            self.assertEqual(
                self.runner._repository_head(repository),
                git("rev-parse", "HEAD"),
            )

            source.write_text("dirty\n", encoding="utf-8")
            with self.assertRaisesRegex(
                self.runner.CaptureGateError, "clean tracked source checkout"
            ):
                self.runner._repository_head(repository)

    def test_fixture_wire_recorder_captures_download_bytes_not_payload(self) -> None:
        scenario = self.runner.Scenario(
            "fixture-flash",
            "tcp",
            ("flash", "system", "/tmp/system.img"),
            "flash:system",
        )
        recorder = self.runner.WireRecorder(scenario)
        self.assertEqual(
            recorder.handle(b"getvar:max-download-size"), b"OKAY0x00100000"
        )
        self.assertEqual(recorder.handle(b"download:00000004"), b"DATA00000004")
        self.assertEqual(recorder.handle(b"data"), b"OKAYdownloaded")
        self.assertEqual(recorder.handle(b"flash:system"), b"OKAYflashed")
        capture = recorder.capture(0)
        data_event = next(event for event in capture["events"] if event["kind"] == "DATA")
        self.assertEqual(data_event["size"], 4)
        self.assertEqual(data_event["sha256"], hashlib.sha256(b"data").hexdigest())
        self.assertNotIn("payload", data_event)
        self.assertEqual(capture["events"][-1], {"kind": "EXIT", "code": 0})

    def test_real_commands_use_native_aosp_and_kairosboot_selectors(self) -> None:
        scenario = self.runner.Scenario(
            "fixture-getvar", "udp", ("getvar", "product"), "getvar:product"
        )
        self.assertEqual(
            self.runner._aosp_command(pathlib.Path("fastboot"), scenario),
            ["fastboot", "-s", "{endpoint}", "getvar", "product"],
        )
        self.assertEqual(
            self.runner._kairosboot_command(pathlib.Path("kairosboot"), scenario),
            [
                "kairosboot",
                "--device",
                "{endpoint}",
                "--timeout-ms",
                "5000",
                "getvar",
                "product",
            ],
        )

    def test_signature_requires_download_and_records_exact_blob(self) -> None:
        scenario = self.runner.Scenario(
            "fixture-signature",
            "tcp",
            ("signature", "/tmp/signature.bin"),
            "signature",
        )
        recorder = self.runner.WireRecorder(scenario)
        self.assertEqual(recorder.handle(b"download:00000004"), b"DATA00000004")
        self.assertEqual(recorder.handle(b"sig\x00"), b"OKAYdownloaded")
        self.assertEqual(recorder.handle(b"signature"), b"OKAYaccepted")
        capture = recorder.capture(0)
        self.assertEqual(
            capture["deviceState"]["signature"],
            {"size": 4, "sha256": hashlib.sha256(b"sig\x00").hexdigest()},
        )

    def test_committed_capture_is_schema_valid_real_matched_evidence(self) -> None:
        schema = json.loads(
            (
                self.root
                / "tests/compat/official-fastboot-differential-evidence.schema.json"
            ).read_text(encoding="utf-8")
        )
        evidence_root = (
            self.root
            / "compat/evidence/official-differential-37.0.1-darwin"
        )
        metadata = json.loads(
            (evidence_root / "official-capture-metadata.json").read_text(
                encoding="utf-8"
            )
        )
        check_schema(schema)
        validate(metadata, schema)
        self.assertEqual(metadata["result"], "matched")
        self.assertEqual(len(metadata["scenarios"]), 16)
        self.assertEqual(
            metadata["aospFastboot"]["sha256"],
            json.loads((self.root / "compat/aosp.lock.json").read_text())[
                "aosp"
            ]["officialArchives"]["darwin"]["fastbootSha256"],
        )
        self.assertRegex(metadata["kairosboot"]["sourceCommit"], r"^[0-9a-f]{40}$")
        self.assertEqual(
            metadata["kairosboot"]["releaseArtifactSha256"],
            metadata["kairosboot"]["sha256"],
        )

        captures = []
        for label in ("aosp", "kairosboot"):
            descriptor = metadata["captureFiles"][label]
            capture_path = evidence_root / descriptor["path"]
            self.assertEqual(
                hashlib.sha256(capture_path.read_bytes()).hexdigest(),
                descriptor["sha256"],
            )
            captures.append(json.loads(capture_path.read_text(encoding="utf-8")))
        self.assertEqual(captures[0], captures[1])
        self.assertEqual(
            [scenario["id"] for scenario in captures[0]["scenarios"]],
            [scenario["id"] for scenario in metadata["scenarios"]],
        )
        cli_arguments = [
            argument
            for scenario in captures[0]["scenarios"]
            for argument in scenario["events"][0]["argv"]
        ]
        self.assertNotIn("/private/tmp/", "\n".join(cli_arguments))
        uncovered = {item["id"] for item in metadata["uncoveredScenarios"]}
        self.assertTrue(
            {
                "official-scripted-boot-flash-raw",
                "official-scripted-slot-policy",
                "official-scripted-avb-flags",
                "official-scripted-sparse-limit",
                "official-scripted-format",
                "official-scripted-wipe-super",
                "official-scripted-update-flashall",
            }.issubset(uncovered)
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parsed, remaining = parser.parse_known_args()
    REPOSITORY_ROOT = parsed.repository_root.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
