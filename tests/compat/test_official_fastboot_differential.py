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


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]


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
                "-s",
                "{endpoint}",
                "getvar",
                "product",
            ],
        )

    def test_expanded_catalog_covers_simple_commands_and_global_options(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            scenarios = self.runner._scenario_catalog(pathlib.Path(raw_directory))
        by_id = {scenario.identifier: scenario for scenario in scenarios}
        expected = {
            "official-host-devices",
            "official-host-help",
            "official-host-help-short",
            "official-host-version",
            "official-tcp-reboot-recovery",
            "official-tcp-reboot-fastboot",
            "official-tcp-erase",
            "official-tcp-set-active",
            "official-tcp-slot-options",
            "official-tcp-avb-flags",
            "official-tcp-sparse-limit",
            "official-tcp-flashing-lock",
            "official-tcp-flashing-unlock",
            "official-tcp-flashing-lock-critical",
            "official-tcp-flashing-unlock-critical",
            "official-tcp-gsi-disable",
            "official-tcp-gsi-status",
            "official-tcp-snapshot-merge",
            "official-tcp-serial-selector",
            "official-tcp-verbose",
            "official-tcp-get-staged",
            "official-tcp-fetch-chunking",
            "official-tcp-resize-logical-partition",
            "official-tcp-resize-logical-partition-fastbootd",
            "official-tcp-boot-raw-options",
            "official-tcp-flash-force",
            "official-tcp-informational-responses",
        }
        self.assertEqual(len(scenarios), 46)
        self.assertTrue(expected.issubset(by_id))
        super_metadata = self.runner._synthetic_super_metadata()
        self.assertEqual(len(super_metadata), 65_536)
        self.assertEqual(
            hashlib.sha256(super_metadata).hexdigest(),
            "5e1cfe4fe46f85e773d23a339a4e766e24108b6ea8e347a367f459396c7dbd7f",
        )
        self.assertEqual(
            self.runner._aosp_command(
                pathlib.Path("fastboot"), by_id["official-tcp-serial-selector"]
            ),
            ["fastboot", "-s", "{endpoint}", "getvar", "product"],
        )
        self.assertEqual(
            self.runner._aosp_command(
                pathlib.Path("fastboot"), by_id["official-host-version"]
            ),
            ["fastboot", "--version"],
        )
        self.assertEqual(
            self.runner._kairosboot_command(
                pathlib.Path("kairosboot"), by_id["official-tcp-verbose"]
            ),
            [
                "kairosboot",
                "-s",
                "{endpoint}",
                "--verbose",
                "getvar",
                "product",
            ],
        )

    def test_device_to_host_capture_records_exact_payload_and_file(self) -> None:
        with tempfile.TemporaryDirectory() as raw_directory:
            output = pathlib.Path(raw_directory) / "received.bin"
            scenario = self.runner.Scenario(
                "fixture-receive",
                "tcp",
                ("get_staged", "<OUTPUT>/received.bin"),
                "upload",
                receive_payload=b"a\x00b",
                output_path=output,
                output_event_path="<OUTPUT>/received.bin",
            )
            recorder = self.runner.WireRecorder(scenario)
            self.assertEqual(recorder.handle(b"upload"), b"DATA00000003")
            self.assertEqual(
                recorder.take_pending_responses(), [b"a\x00b", b"OKAYuploaded"]
            )
            output.write_bytes(b"a\x00b")
            capture = recorder.capture(0)
        self.assertEqual(
            [event["kind"] for event in capture["events"]],
            ["CLI_PARSE", "COMMAND", "DATA", "OKAY", "FILE", "EXIT"],
        )
        self.assertEqual(capture["events"][2]["direction"], "device-to-host")

    def test_host_capture_validates_and_normalizes_version_output(self) -> None:
        scenario = self.runner.Scenario(
            "fixture-version",
            "host",
            ("--version",),
            "host",
            host_output_kind="version",
        )
        capture = self.runner._capture_host(
            [sys.executable, "-c", "print('fixture 1.2.3')"],
            scenario,
            "fixture",
        )
        self.assertEqual(
            capture["events"],
            [
                {"kind": "CLI_PARSE", "argv": ["--version"], "result": "ok"},
                {"kind": "TEXT", "message": "version"},
                {"kind": "EXIT", "code": 0},
            ],
        )

    def test_informational_response_chain_preserves_info_text_and_okay(self) -> None:
        scenario = self.runner.Scenario(
            "fixture-responses",
            "tcp",
            ("oem", "differential-info"),
            "oem differential-info",
            informational_responses=(("INFO", "one"), ("TEXT", "two")),
        )
        recorder = self.runner.WireRecorder(scenario)
        self.assertEqual(recorder.handle(b"oem differential-info"), b"INFOone")
        self.assertEqual(
            recorder.take_pending_responses(), [b"TEXTtwo", b"OKAYaccepted"]
        )
        capture = recorder.capture(0)
        self.assertEqual(
            [event["kind"] for event in capture["events"]],
            ["CLI_PARSE", "COMMAND", "INFO", "TEXT", "OKAY", "EXIT"],
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

    def test_fragmented_download_and_repeated_terminal_are_normalized(self) -> None:
        scenario = self.runner.Scenario(
            "fixture-multipart",
            "tcp",
            ("flash", "system", "fixture.img"),
            "flash:system",
            terminal_occurrences=2,
        )
        recorder = self.runner.WireRecorder(scenario)
        for payload in (b"abcd", b"efgh"):
            self.assertEqual(recorder.handle(b"download:00000004"), b"DATA00000004")
            self.assertIsNone(recorder.handle(payload[:2]))
            self.assertEqual(recorder.handle(payload[2:]), b"OKAYdownloaded")
            self.assertEqual(recorder.handle(b"flash:system"), b"OKAYflashed")
        capture = recorder.capture(0)
        self.assertEqual(
            [event["size"] for event in capture["events"] if event["kind"] == "DATA"],
            [4, 4],
        )
        self.assertEqual(
            [event["command"] for event in capture["events"]
             if event["kind"] == "COMMAND"],
            ["download:00000004", "flash:system",
             "download:00000004", "flash:system"],
        )

    def test_reboot_fastboot_records_mode_transition_across_connections(self) -> None:
        scenario = self.runner.Scenario(
            "fixture-reboot-fastboot",
            "tcp",
            ("reboot", "fastboot"),
            "getvar:is-userspace",
            variable_sequences=(("is-userspace", ("no", "yes")),),
            reconnect_after_commands=("reboot-fastboot",),
            terminal_occurrences=2,
        )
        recorder = self.runner.WireRecorder(scenario)
        self.assertEqual(recorder.handle(b"getvar:is-userspace"), b"OKAYno")
        self.assertEqual(recorder.handle(b"reboot-fastboot"), b"OKAYaccepted")
        self.assertFalse(recorder.finished)
        self.assertEqual(recorder.handle(b"getvar:is-userspace"), b"OKAYyes")
        self.assertTrue(recorder.finished)
        capture = recorder.capture(0)
        self.assertEqual(
            [event.get("name") for event in capture["events"]
             if event["kind"] == "GETVAR"],
            ["is-userspace", "is-userspace"],
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
        self.assertGreaterEqual(len(metadata["scenarios"]), 19)
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
        scenario_ids = {scenario["id"] for scenario in metadata["scenarios"]}
        self.assertTrue(
            {
                "official-tcp-flash-default-file",
                "official-tcp-boot-raw",
                "official-tcp-flash-raw",
                "official-tcp-reboot-recovery",
                "official-tcp-flashing-lock",
                "official-tcp-flashing-unlock",
                "official-tcp-flashing-lock-critical",
                "official-tcp-flashing-unlock-critical",
                "official-tcp-gsi-disable",
                "official-tcp-gsi-status",
                "official-tcp-snapshot-merge",
                "official-tcp-serial-selector",
                "official-tcp-verbose",
                "official-host-devices",
                "official-host-help",
                "official-host-help-short",
                "official-host-version",
                "official-tcp-get-staged",
                "official-tcp-fetch-chunking",
                "official-tcp-boot-raw-options",
                "official-tcp-flash-force",
                "official-tcp-informational-responses",
                "official-tcp-erase",
                "official-tcp-set-active",
                "official-tcp-slot-options",
                "official-tcp-avb-flags",
                "official-tcp-sparse-limit",
            }.issubset(scenario_ids)
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
                "official-scripted-format",
                "official-scripted-wipe-super",
                "official-scripted-update-flashall",
            }.issubset(uncovered)
        )
        self.assertNotIn("official-scripted-boot-flash-raw", uncovered)
        self.assertNotIn("official-scripted-erase", uncovered)
        self.assertNotIn("official-scripted-slot-options", uncovered)
        self.assertNotIn("official-scripted-avb-flags", uncovered)
        self.assertNotIn("official-scripted-sparse-limit", uncovered)
        self.assertNotIn("official-scripted-fetch-chunking", uncovered)

    def test_committed_evidence_index_covers_every_locked_platform(self) -> None:
        index = json.loads(
            (self.root / "compat/official-differential-evidence.json").read_text()
        )
        metadata_paths = index["captures"]
        self.assertEqual(len(metadata_paths), 3)
        observed_platforms = set()
        for relative in metadata_paths:
            metadata_path = self.root / relative
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            platform_key = metadata["aospFastboot"]["platform"]
            observed_platforms.add(platform_key)
            evidence_root = metadata_path.parent
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
        self.assertEqual(observed_platforms, {"darwin", "linux", "windows"})


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parsed, remaining = parser.parse_known_args()
    REPOSITORY_ROOT = parsed.repository_root.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
