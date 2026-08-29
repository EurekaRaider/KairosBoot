#!/usr/bin/env python3
"""Release tooling contracts that do not require a compiler or network."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SMOKE_SPEC = importlib.util.spec_from_file_location(
    "kairosboot_smoke_native_archive", ROOT / "scripts" / "smoke_native_archive.py"
)
if SMOKE_SPEC is None or SMOKE_SPEC.loader is None:
    raise RuntimeError("unable to load native archive smoke module")
SMOKE_MODULE = importlib.util.module_from_spec(SMOKE_SPEC)
SMOKE_SPEC.loader.exec_module(SMOKE_MODULE)


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

    def create_install_tree(self, platform: str = "linux-x64") -> Path:
        install = self.root / f"install-{platform}-{len(list(self.root.glob('install-*')))}"
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
        (install / "share" / "kairosboot" / "miniz").mkdir()
        (install / "share" / "kairosboot" / "miniz" / "LICENSE").write_text(
            "miniz MIT\n", encoding="utf-8"
        )
        windows = platform.startswith("windows-")
        (install / "share" / "kairosboot" / "release-build.json").write_text(
            json.dumps(
                {
                    "documentType": "kairosboot.release-build",
                    "schemaVersion": 1,
                    "platform": platform,
                    "configuration": "Release",
                    "optimization": "O2" if windows else "O3",
                    "ndebug": True,
                    "cmakeFlags": {
                        "CMAKE_C_FLAGS_RELEASE": "/O2 /DNDEBUG"
                        if windows
                        else "-O3 -DNDEBUG",
                        "CMAKE_CXX_FLAGS_RELEASE": "/O2 /DNDEBUG"
                        if windows
                        else "-O3 -DNDEBUG",
                    },
                },
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
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

    def create_release_assets(self, version: str = "1.2.3") -> Path:
        assets = self.root / f"release-assets-{len(list(self.root.glob('release-assets-*')))}"
        assets.mkdir()
        run_script(
            "check_release_assets.py",
            "--assets",
            assets,
            "--version",
            version,
            "--write-manifest",
        )
        manifest = assets / f"KairosBoot-v{version}-assets.json"
        names = json.loads(manifest.read_text(encoding="utf-8"))["assets"]
        for name in names:
            path = assets / name
            if not path.exists() and name not in {
                "KairosBoot.spdx.json",
                "provenance.intoto.json",
                "SHA256SUMS",
                "signing-status.json",
            }:
                path.write_bytes(f"fixture:{name}\n".encode())

        source_name = f"KairosBoot-v{version}-source.tar.gz"
        sbom = {
            "spdxVersion": "SPDX-2.3",
            "packages": [
                {
                    "name": "KairosBoot",
                    "versionInfo": version,
                    "checksums": [
                        {
                            "algorithm": "SHA256",
                            "checksumValue": hashlib.sha256(
                                (assets / source_name).read_bytes()
                            ).hexdigest(),
                        }
                    ],
                },
                {
                    "name": "libusb",
                    "versionInfo": "1.0.30",
                    "checksums": [
                        {
                            "algorithm": "SHA256",
                            "checksumValue": hashlib.sha256(
                                (assets / "libusb-1.0.30.tar.bz2").read_bytes()
                            ).hexdigest(),
                        }
                    ],
                },
            ],
        }
        (assets / "KairosBoot.spdx.json").write_text(
            json.dumps(sbom, sort_keys=True) + "\n", encoding="utf-8"
        )
        (assets / "signing-status.json").write_text(
            json.dumps(
                {
                    "schemaVersion": 1,
                    "mode": "off",
                    "authenticode": False,
                    "developerId": False,
                    "notarized": False,
                    "checksumSignature": False,
                },
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        self.refresh_release_integrity(assets, version)
        return assets

    def refresh_release_integrity(self, assets: Path, version: str) -> None:
        (assets / "provenance.intoto.json").unlink(missing_ok=True)
        (assets / "SHA256SUMS").unlink(missing_ok=True)
        run_script(
            "generate_provenance.py",
            "--assets",
            assets,
            "--output",
            assets / "provenance.intoto.json",
            "--signing-mode",
            "off",
            "--commit",
            "a" * 40,
        )
        run_script(
            "check_release_assets.py",
            "--assets",
            assets,
            "--version",
            version,
            "--write-checksums",
        )

    def write_cmake_cache(
        self, build_type: str, flags: str, *, windows: bool = False
    ) -> Path:
        cache = self.root / f"CMakeCache-{build_type}-{len(list(self.root.glob('CMakeCache-*')))}.txt"
        lines = [] if windows else [f"CMAKE_BUILD_TYPE:STRING={build_type}"]
        lines.extend(
            (
                f"CMAKE_C_FLAGS_RELEASE:STRING={flags}",
                f"CMAKE_CXX_FLAGS_RELEASE:STRING={flags}",
            )
        )
        cache.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return cache

    def test_repository_release_fixtures_cover_distribution_contract(self) -> None:
        environment = json.loads(
            (
                ROOT / ".github" / "environments" / "github-release.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(environment["wait_timer"], 0)
        self.assertFalse(environment["prevent_self_review"])
        self.assertEqual(
            environment["reviewers"], [{"type": "User", "id": 299445952}]
        )
        self.assertEqual(
            environment["deployment_branch_policy"],
            {"protected_branches": False, "custom_branch_policies": True},
        )

        policies = json.loads(
            (
                ROOT
                / ".github"
                / "environments"
                / "github-release-deployment-policies.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(
            policies,
            {
                "schemaVersion": 1,
                "policies": [{"name": "v*", "type": "tag"}],
            },
        )

        main_ruleset = json.loads(
            (ROOT / ".github" / "rulesets" / "main.json").read_text(
                encoding="utf-8"
            )
        )
        main_rules = {entry["type"]: entry for entry in main_ruleset["rules"]}
        self.assertTrue(
            main_rules["pull_request"]["parameters"][
                "require_extra_approval_for_unattributed_changes"
            ]
        )
        self.assertEqual(
            main_rules["required_status_checks"]["parameters"],
            {
                "strict_required_status_checks_policy": True,
                "do_not_enforce_on_create": False,
                "required_status_checks": [
                    {"context": "ci-required", "integration_id": 15368},
                    {"context": "policy", "integration_id": 15368},
                ],
            },
        )

        tag_ruleset = json.loads(
            (ROOT / ".github" / "rulesets" / "release-tags.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(
            tag_ruleset["conditions"]["ref_name"]["include"], ["refs/tags/v*"]
        )
        self.assertEqual(
            {entry["type"] for entry in tag_ruleset["rules"]},
            {"creation", "update", "deletion", "non_fast_forward"},
        )

        contract = json.loads(
            (ROOT / "release" / "expected-assets.json").read_text(encoding="utf-8")
        )
        self.assertEqual(contract["nativeKinds"], ["cli", "sdk", "symbols"])
        self.assertEqual(
            {entry["id"]: entry["archiveExtension"] for entry in contract["platforms"]},
            {
                "linux-arm64": "tar.gz",
                "linux-x64": "tar.gz",
                "macos-arm64": "tar.gz",
                "macos-x64": "tar.gz",
                "windows-arm64": "zip",
                "windows-x64": "zip",
            },
        )
        self.assertTrue(
            {
                "KairosBoot.{version}.nupkg",
                "KairosBoot.{version}.snupkg",
                "KairosBoot.spdx.json",
                "KairosBoot-v{version}-UNSIGNED.txt",
                "KairosBoot-v{version}-source.tar.gz",
                "SHA256SUMS",
                "libusb-1.0.30-COPYING",
                "libusb-1.0.30.tar.bz2",
                "provenance.intoto.json",
                "signing-status.json",
            }.issubset(set(contract["additionalAssets"]))
        )

        workflow = (ROOT / ".github" / "workflows" / "release.yml").read_text(
            encoding="utf-8"
        )
        for platform in (
            "linux-arm64",
            "linux-x64",
            "macos-arm64",
            "macos-x64",
            "windows-arm64",
            "windows-x64",
        ):
            self.assertIn(f"id: {platform}", workflow)
        for marker in (
            "-DCMAKE_BUILD_TYPE=Release",
            "cmake --build build --config Release",
            "/p:Configuration=Release",
            "environment: github-release",
            "SIGNING_MODE: off",
            "require_success ci.yml",
            "require_success policy.yml",
            "require_success hil.yml",
        ):
            self.assertIn(marker, workflow)
        self.assertNotIn("dotnet nuget push", workflow.lower())

    def test_native_archives_have_expected_shape_and_modes(self) -> None:
        install = self.create_install_tree()
        # The archive contract must not depend on Unix mode bits being available
        # on the host filesystem (notably when this test runs on Windows).
        (install / "bin" / "kairosboot").chmod(0o644)
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
            self.assertIn(
                "KairosBoot-v1.2.3-linux-x64-sdk/share/kairosboot/miniz/LICENSE",
                names,
            )
        with tarfile.open(cli, "r:gz") as archive:
            member = archive.getmember("KairosBoot-v1.2.3-linux-x64-cli/bin/kairosboot")
            self.assertEqual(member.mode & 0o777, 0o755)
            self.assertNotIn(
                "KairosBoot-v1.2.3-linux-x64-cli/lib/cmake",
                archive.getnames(),
            )
            self.assertIn(
                "KairosBoot-v1.2.3-linux-x64-cli/share/kairosboot/miniz/LICENSE",
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

    def test_release_build_gate_accepts_only_optimized_release(self) -> None:
        cases = (
            ("linux-x64", "-O3 -DNDEBUG", False),
            ("macos-arm64", "-O3 -DNDEBUG", False),
            ("windows-x64", "/O2 /DNDEBUG", True),
        )
        for platform, flags, windows in cases:
            with self.subTest(platform=platform):
                install = self.create_install_tree(platform)
                cache = self.write_cmake_cache("Release", flags, windows=windows)
                run_script(
                    "check_release_build.py",
                    "--cache",
                    cache,
                    "--install-root",
                    install,
                    "--platform",
                    platform,
                    "--configuration",
                    "Release",
                    "--write-evidence",
                )

    def test_release_build_gate_rejects_debug_relwithdebinfo_and_missing_flags(self) -> None:
        install = self.create_install_tree()
        cases = (
            ("Debug", "Release", "-O3 -DNDEBUG", "CMAKE_BUILD_TYPE"),
            ("Release", "RelWithDebInfo", "-O3 -DNDEBUG", "configuration must be Release"),
            ("Release", "Release", "-DNDEBUG", "lacks required flags"),
        )
        for build_type, configuration, flags, message in cases:
            with self.subTest(build_type=build_type, configuration=configuration):
                cache = self.write_cmake_cache(build_type, flags)
                completed = subprocess.run(
                    [
                        "python3",
                        str(ROOT / "scripts" / "check_release_build.py"),
                        "--cache",
                        str(cache),
                        "--install-root",
                        str(install),
                        "--platform",
                        "linux-x64",
                        "--configuration",
                        configuration,
                    ],
                    cwd=ROOT,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn(message, completed.stderr + completed.stdout)

    def test_release_context_requires_tag_version_and_exact_main_head(self) -> None:
        version_file = self.root / "version.json"
        version_file.write_text('{"version":"1.2.3"}\n', encoding="utf-8")
        commit = "a" * 40
        run_script(
            "check_release_context.py",
            "--tag",
            "v1.2.3",
            "--version-file",
            version_file,
            "--tag-commit",
            commit,
            "--main-commit",
            commit,
            "--checkout-commit",
            commit,
        )
        completed = subprocess.run(
            [
                "python3",
                str(ROOT / "scripts" / "check_release_context.py"),
                "--tag",
                "v1.2.3",
                "--version-file",
                str(version_file),
                "--tag-commit",
                commit,
                "--main-commit",
                "b" * 40,
                "--checkout-commit",
                commit,
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("same exact head", completed.stderr + completed.stdout)

    def test_windows_packages_are_zip_archives(self) -> None:
        install = self.create_install_tree("windows-x64")
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
        install = self.create_install_tree("windows-x64")
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

    def test_windows_archive_smoke_requires_explicit_dumpbin(self) -> None:
        dist = self.root / "windows-dist"
        dist.mkdir()
        completed = subprocess.run(
            [
                "python3",
                str(ROOT / "scripts" / "smoke_native_archive.py"),
                "--dist",
                str(dist),
                "--version",
                "1.2.3",
                "--platform",
                "windows-x64",
                "--generator",
                "Visual Studio 17 2022",
                "--architecture",
                "x64",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(
            "--dumpbin is required for Windows Release archive smoke",
            completed.stderr + completed.stdout,
        )

    def test_sbom_hashes_all_source_inputs(self) -> None:
        source = self.root / "source.tar.gz"
        libusb = self.root / "libusb.tar.bz2"
        boost = self.root / "boost.tar.xz"
        miniz = self.root / "miniz.zip"
        output = self.root / "KairosBoot.spdx.json"
        source.write_bytes(b"source")
        libusb.write_bytes(b"libusb")
        boost.write_bytes(b"boost")
        miniz.write_bytes(b"miniz")
        run_script(
            "generate_sbom.py",
            "--version",
            "1.2.3",
            "--source-archive",
            source,
            "--libusb-source",
            libusb,
            "--boost-source",
            boost,
            "--miniz-source",
            miniz,
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
            packages["Boost"]["checksums"][0]["checksumValue"],
            hashlib.sha256(b"boost").hexdigest(),
        )
        self.assertEqual(packages["Boost"]["licenseDeclared"], "BSL-1.0")
        self.assertEqual(
            packages["miniz"]["checksums"][0]["checksumValue"],
            hashlib.sha256(b"miniz").hexdigest(),
        )
        self.assertEqual(packages["miniz"]["licenseDeclared"], "MIT")
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

    def test_release_asset_contract_is_explicit_and_exact(self) -> None:
        version = "1.2.3"
        assets = self.create_release_assets(version)
        manifest = assets / f"KairosBoot-v{version}-assets.json"
        document = json.loads(manifest.read_text(encoding="utf-8"))
        expected = document["assets"]
        self.assertEqual(expected, sorted(set(expected)))
        self.assertIn(f"KairosBoot.{version}.nupkg", expected)
        self.assertIn(f"KairosBoot.{version}.snupkg", expected)
        self.assertIn("libusb-1.0.30-COPYING", expected)
        self.assertIn("miniz-3.1.2.zip", expected)
        self.assertIn("miniz-3.1.2-LICENSE", expected)
        run_script(
            "check_release_assets.py",
            "--assets",
            assets,
            "--version",
            version,
            "--validate",
        )

        missing = assets / f"KairosBoot-v{version}-linux-x64-sdk.tar.gz"
        missing.unlink()
        failed = subprocess.run(
            [
                "python3",
                str(ROOT / "scripts" / "check_release_assets.py"),
                "--assets",
                str(assets),
                "--version",
                version,
                "--validate",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn(missing.name, failed.stderr + failed.stdout)

    def test_release_assets_reject_bad_hash_wrong_libusb_and_signing_mode(self) -> None:
        version = "1.2.3"
        assets = self.create_release_assets(version)
        target = assets / f"KairosBoot-v{version}-linux-x64-cli.tar.gz"
        target.write_bytes(target.read_bytes() + b"tampered")
        bad_hash = subprocess.run(
            [
                "python3",
                str(ROOT / "scripts" / "check_release_assets.py"),
                "--assets",
                str(assets),
                "--version",
                version,
                "--validate",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(bad_hash.returncode, 0)
        self.assertIn("SHA256SUMS mismatch", bad_hash.stderr + bad_hash.stdout)

        assets = self.root / "wrong-libusb"
        shutil.copytree(self.create_release_assets(version), assets)
        sbom_path = assets / "KairosBoot.spdx.json"
        sbom = json.loads(sbom_path.read_text(encoding="utf-8"))
        next(package for package in sbom["packages"] if package["name"] == "libusb")[
            "versionInfo"
        ] = "1.0.29"
        sbom_path.write_text(json.dumps(sbom) + "\n", encoding="utf-8")
        self.refresh_release_integrity(assets, version)
        wrong_libusb = subprocess.run(
            [
                "python3",
                str(ROOT / "scripts" / "check_release_assets.py"),
                "--assets",
                str(assets),
                "--version",
                version,
                "--validate",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(wrong_libusb.returncode, 0)
        self.assertIn("wrong libusb version", wrong_libusb.stderr + wrong_libusb.stdout)

        invalid_mode = subprocess.run(
            [
                "python3",
                str(ROOT / "scripts" / "check_release_assets.py"),
                "--assets",
                str(assets),
                "--version",
                version,
                "--signing-mode",
                "required",
                "--validate",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(invalid_mode.returncode, 0)
        self.assertIn("unsupported SIGNING_MODE", invalid_mode.stderr + invalid_mode.stdout)

    def test_compression_runtime_dependency_gate_is_platform_specific(self) -> None:
        fixtures = {
            "linux-x64": """
                0x0000000000000001 (NEEDED) Shared library: [libusb-1.0.so.0]
                0x0000000000000001 (NEEDED) Shared library: [libz.so.1]
            """,
            "macos-arm64": """
                library.dylib:
                /usr/lib/libSystem.B.dylib (compatibility version 1.0.0)
                /usr/lib/libz.1.dylib (compatibility version 1.0.0)
            """,
            "windows-x64": """
                Image has the following dependencies:
                    KERNEL32.dll
                    zlib1.dll
            """,
        }
        for platform, output in fixtures.items():
            with self.subTest(platform=platform):
                dependencies = SMOKE_MODULE.dependency_names(output, platform)
                forbidden = {
                    name
                    for name in dependencies
                    if SMOKE_MODULE.is_forbidden_compression_runtime(name)
                }
                self.assertEqual(len(forbidden), 1)

        self.assertFalse(SMOKE_MODULE.is_forbidden_compression_runtime("libzstd.so.1"))


if __name__ == "__main__":
    unittest.main()
