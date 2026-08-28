#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import tempfile
import unittest
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "prepare_official_fastboot", ROOT / "scripts" / "prepare_official_fastboot.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PrepareOfficialFastbootTests(unittest.TestCase):
    def _fixture(self, directory: pathlib.Path, member: str = "platform-tools/fastboot"):
        archive = directory / "platform-tools.zip"
        payload = b"locked-fastboot-fixture"
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr(member, payload)
        repository = directory / "repository"
        (repository / "compat").mkdir(parents=True)
        lock = {
            "aosp": {
                "platformToolsVersion": "37.0.1",
                "officialArchives": {
                    "linux": {
                        "url": "https://dl.google.com/android/repository/platform-tools_r37.0.1-linux.zip",
                        "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                        "fastbootSha256": hashlib.sha256(payload).hexdigest(),
                    }
                },
            }
        }
        (repository / "compat" / "aosp.lock.json").write_text(
            json.dumps(lock), encoding="utf-8"
        )
        return repository, archive, payload

    def test_extracts_only_hash_locked_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            repository, archive, payload = self._fixture(root)
            result = MODULE.prepare(repository, "linux", root / "output", archive)
            self.assertEqual(result.read_bytes(), payload)
            if os.name != "nt":
                self.assertTrue(result.stat().st_mode & 0o111)

    def test_rejects_archive_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            repository, archive, _ = self._fixture(root)
            archive.write_bytes(archive.read_bytes() + b"tampered")
            with self.assertRaisesRegex(MODULE.PreparationError, "archive SHA-256 mismatch"):
                MODULE.prepare(repository, "linux", root / "output", archive)

    def test_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            repository, archive, _ = self._fixture(root, "../fastboot")
            with self.assertRaisesRegex(MODULE.PreparationError, "unsafe"):
                MODULE.prepare(repository, "linux", root / "output", archive)


if __name__ == "__main__":
    unittest.main()
