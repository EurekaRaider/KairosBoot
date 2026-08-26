#!/usr/bin/env python3
"""Release tooling contracts that do not require a compiler or network."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def run_script(name: str, *arguments: object, environment: dict[str, str] | None = None) -> None:
    command = ["python3", str(ROOT / "scripts" / name)]
    command.extend(str(argument) for argument in arguments)
    subprocess.run(command, cwd=ROOT, env=environment, check=True)


class ReleaseToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="kairosboot-release-tools-")
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def create_install_tree(self) -> Path:
        install = self.root / "install"
        (install / "bin").mkdir(parents=True)
        (install / "include" / "kairosboot").mkdir(parents=True)
        (install / "lib" / "cmake" / "KairosBoot").mkdir(parents=True)
        (install / "share" / "kairosboot").mkdir(parents=True)
        cli = install / "bin" / "kairosboot"
        cli.write_bytes(b"#!/bin/sh\nexit 0\n")
        cli.chmod(0o755)
        (install / "include" / "kairosboot" / "kairosboot.h").write_text(
            "#pragma once\n", encoding="utf-8"
        )
        (install / "lib" / "libkairosboot.so.0.1.0").write_bytes(b"library")
        (install / "lib" / "cmake" / "KairosBoot" / "KairosBootConfig.cmake").write_text(
            "# test\n", encoding="utf-8"
        )
        (install / "share" / "kairosboot" / "LICENSE").write_text(
            "MIT\n", encoding="utf-8"
        )
        return install

    def create_symbols(self) -> Path:
        symbols = self.root / "symbols"
        dwarf = symbols / "kairosboot.dSYM" / "Contents" / "Resources" / "DWARF"
        dwarf.mkdir(parents=True)
        (dwarf / "kairosboot").write_bytes(b"symbols")
        (symbols / "libkairosboot.debug").write_bytes(b"debug")
        (symbols / "libusb-1.0.debug").write_bytes(b"dependency debug")
        (symbols / "kairosboot.pdb").write_bytes(b"library pdb")
        (symbols / "kairosboot-cli.pdb").write_bytes(b"cli pdb")
        (symbols / "libusb-1.0.pdb").write_bytes(b"dependency pdb")
        return symbols

    def test_native_archives_have_expected_shape_and_modes(self) -> None:
        install = self.create_install_tree()
        symbols = self.create_symbols()
        output = self.root / "dist"
        run_script(
            "package_native.py",
            "--install-root",
            install,
            "--output-dir",
            output,
            "--version",
            "1.2.3",
            "--platform",
            "linux-x64",
            "--symbols-root",
            symbols,
        )

        sdk = output / "KairosBoot-v1.2.3-linux-x64-sdk.tar.gz"
        cli = output / "KairosBoot-v1.2.3-linux-x64-cli.tar.gz"
        symbol_archive = output / "KairosBoot-v1.2.3-linux-x64-symbols.tar.gz"
        for archive in (sdk, cli, symbol_archive):
            self.assertTrue(archive.is_file(), archive)

        with tarfile.open(sdk, "r:gz") as archive:
            names = set(archive.getnames())
            self.assertIn(
                "KairosBoot-v1.2.3-linux-x64-sdk/include/kairosboot/kairosboot.h",
                names,
            )
            self.assertIn(
                "KairosBoot-v1.2.3-linux-x64-sdk/lib/cmake/KairosBoot/KairosBootConfig.cmake",
                names,
            )
        with tarfile.open(cli, "r:gz") as archive:
            member = archive.getmember("KairosBoot-v1.2.3-linux-x64-cli/bin/kairosboot")
            self.assertNotEqual(member.mode & stat.S_IXUSR, 0)
            self.assertNotIn(
                "KairosBoot-v1.2.3-linux-x64-cli/lib/cmake",
                archive.getnames(),
            )
        with tarfile.open(symbol_archive, "r:gz") as archive:
            prefix = "KairosBoot-v1.2.3-linux-x64-symbols"
            names = set(archive.getnames())
            self.assertIn(f"{prefix}/libkairosboot.debug", names)
            self.assertIn(f"{prefix}/libusb-1.0.debug", names)
            self.assertIn(f"{prefix}/kairosboot.pdb", names)
            self.assertIn(f"{prefix}/kairosboot-cli.pdb", names)
            self.assertIn(f"{prefix}/libusb-1.0.pdb", names)
            self.assertIn(
                f"{prefix}/kairosboot.dSYM/Contents/Resources/DWARF/kairosboot",
                names,
            )
            self.assertFalse(any("/00-" in name or "/01-" in name for name in names))

    def test_windows_packages_are_zip_archives(self) -> None:
        install = self.create_install_tree()
        symbols = self.create_symbols()
        output = self.root / "dist"
        run_script(
            "package_native.py",
            "--install-root",
            install,
            "--output-dir",
            output,
            "--version",
            "1.2.3",
            "--platform",
            "windows-x64",
            "--symbols-root",
            symbols,
        )
        archives = sorted(output.glob("*.zip"))
        self.assertEqual(len(archives), 3)
        for archive in archives:
            with zipfile.ZipFile(archive) as package:
                self.assertIsNone(package.testzip())
        symbols_archive = output / "KairosBoot-v1.2.3-windows-x64-symbols.zip"
        with zipfile.ZipFile(symbols_archive) as package:
            prefix = "KairosBoot-v1.2.3-windows-x64-symbols"
            names = set(package.namelist())
            self.assertIn(f"{prefix}/kairosboot.pdb", names)
            self.assertIn(f"{prefix}/kairosboot-cli.pdb", names)
            self.assertIn(f"{prefix}/libusb-1.0.pdb", names)

    def test_duplicate_symbol_basenames_are_rejected(self) -> None:
        install = self.create_install_tree()
        symbols = self.root / "duplicate-symbols"
        (symbols / "one").mkdir(parents=True)
        (symbols / "two").mkdir(parents=True)
        (symbols / "one" / "kairosboot.pdb").write_bytes(b"one")
        (symbols / "two" / "kairosboot.pdb").write_bytes(b"two")
        completed = subprocess.run(
            [
                "python3",
                str(ROOT / "scripts" / "package_native.py"),
                "--install-root",
                str(install),
                "--output-dir",
                str(self.root / "duplicate-dist"),
                "--version",
                "1.2.3",
                "--platform",
                "windows-x64",
                "--symbols-root",
                str(symbols),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("duplicate symbol basename: kairosboot.pdb", completed.stderr)

    def test_sbom_hashes_both_source_inputs(self) -> None:
        source = self.root / "source.tar.gz"
        libusb = self.root / "libusb.tar.bz2"
        output = self.root / "KairosBoot.spdx.json"
        source.write_bytes(b"source")
        libusb.write_bytes(b"libusb")
        run_script(
            "generate_sbom.py",
            "--version",
            "1.2.3",
            "--source-archive",
            source,
            "--libusb-source",
            libusb,
            "--output",
            output,
        )
        document = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(document["spdxVersion"], "SPDX-2.3")
        packages = {package["name"]: package for package in document["packages"]}
        self.assertEqual(
            packages["KairosBoot"]["checksums"][0]["checksumValue"],
            hashlib.sha256(b"source").hexdigest(),
        )
        self.assertEqual(
            packages["libusb"]["checksums"][0]["checksumValue"],
            hashlib.sha256(b"libusb").hexdigest(),
        )
        self.assertEqual(
            packages["Microsoft Visual C++ Runtime"]["supplier"],
            "Organization: Microsoft Corporation",
        )

    def test_provenance_excludes_its_own_output(self) -> None:
        assets = self.root / "assets"
        assets.mkdir()
        (assets / "one.bin").write_bytes(b"one")
        (assets / "two.bin").write_bytes(b"two")
        output = assets / "provenance.intoto.json"
        environment = dict(os.environ)
        environment.update(
            {
                "GITHUB_REPOSITORY": "EurekaRaider/KairosBoot",
                "GITHUB_REF": "refs/tags/v1.2.3",
                "GITHUB_SHA": "b" * 40,
                "GITHUB_RUN_ID": "123",
            }
        )
        run_script(
            "generate_provenance.py",
            "--assets",
            assets,
            "--output",
            output,
            environment=environment,
        )
        statement = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(statement["_type"], "https://in-toto.io/Statement/v1")
        self.assertEqual(
            [subject["name"] for subject in statement["subject"]],
            ["one.bin", "two.bin"],
        )
        dependency = statement["predicate"]["buildDefinition"]["resolvedDependencies"][0]
        self.assertEqual(dependency["digest"]["gitCommit"], "b" * 40)


if __name__ == "__main__":
    unittest.main()
