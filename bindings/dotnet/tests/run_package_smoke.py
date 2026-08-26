#!/usr/bin/env python3
"""Restore and run KairosBoot from an actual local-feed NuGet package."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
SMOKE_SOURCE = ROOT / "tests" / "PackageSmoke"
WINDOWS_APP_LOCAL_RUNTIME = (
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
)
NATIVE_FILES = {
    "win-x64": ("kairosboot_native.dll", "libusb-1.0.dll", *WINDOWS_APP_LOCAL_RUNTIME),
    "win-arm64": ("kairosboot_native.dll", "libusb-1.0.dll", *WINDOWS_APP_LOCAL_RUNTIME),
    "linux-x64": ("libkairosboot_native.so", "libusb-1.0.so.0"),
    "linux-arm64": ("libkairosboot_native.so", "libusb-1.0.so.0"),
    "osx-x64": ("libkairosboot_native.dylib", "libusb-1.0.0.dylib"),
    "osx-arm64": ("libkairosboot_native.dylib", "libusb-1.0.0.dylib"),
}


def run(command: list[str], *, cwd: Path, environment: dict[str, str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, env=environment, check=True)


def package_version(package: Path) -> str:
    with zipfile.ZipFile(package) as archive:
        nuspecs = [name for name in archive.namelist() if name.endswith(".nuspec")]
        if len(nuspecs) != 1:
            raise SystemExit(f"expected one nuspec in {package}, found {len(nuspecs)}")
        root = ElementTree.fromstring(archive.read(nuspecs[0]))
    version = next((node.text for node in root.iter() if node.tag.endswith("version")), None)
    if not version:
        raise SystemExit(f"package has no version metadata: {package}")
    return version


def assert_package_licenses(package: Path) -> None:
    with zipfile.ZipFile(package) as archive:
        names = set(archive.namelist())
    required = {
        "licenses/boost/LICENSE_1_0.txt",
        "licenses/libusb/COPYING",
        "licenses/microsoft-vc-runtime/NOTICE.txt",
    }
    missing = sorted(required - names)
    if missing:
        raise SystemExit(f"package is missing dependency license files: {', '.join(missing)}")


def assert_runtime_output(output: Path, rid: str) -> None:
    missing = [name for name in NATIVE_FILES[rid] if not (output / name).is_file()]
    if missing:
        raise SystemExit(f"package did not deploy native runtime files: {', '.join(missing)}")
    polluted = [
        path
        for path in output.rglob("*")
        if path.is_file()
        and path.name in {"COPYING", "LICENSE_1_0.txt", "kairosboot-libusb.json"}
    ]
    if polluted:
        raise SystemExit(f"license metadata leaked into application output: {polluted}")


def run_sdk_smoke(
    package: Path,
    framework: str,
    rid: str,
    expected_native_version: str,
    work: Path,
    environment: dict[str, str],
) -> None:
    project = work / "sdk"
    shutil.copytree(SMOKE_SOURCE, project, ignore=shutil.ignore_patterns("*.template"))
    version = package_version(package)
    feed = work / "feed"
    feed.mkdir()
    shutil.copy2(package, feed / package.name)
    output = work / "sdk-output"

    restore = [
        "dotnet",
        "restore",
        str(project / "KairosBoot.PackageSmoke.csproj"),
        f"/p:KairosBootPackageVersion={version}",
        f"/p:TargetFrameworks={framework}",
        "/p:SelfContained=false",
        "/p:EnableRuntimePackDownload=false",
        "/p:DisableTransitiveFrameworkReferenceDownloads=true",
        "--source",
        str(feed),
    ]
    build = [
        "dotnet",
        "build",
        str(project / "KairosBoot.PackageSmoke.csproj"),
        "--configuration",
        "Release",
        "--framework",
        framework,
        "--no-restore",
        "--output",
        str(output),
        f"/p:KairosBootPackageVersion={version}",
        f"/p:TargetFrameworks={framework}",
        "/p:EnableRuntimePackDownload=false",
        "/p:DisableTransitiveFrameworkReferenceDownloads=true",
    ]
    if framework == "net10.0":
        restore.extend(("--runtime", rid))
        build.extend(("--runtime", rid, "--self-contained", "false"))

    run(restore, cwd=project, environment=environment)
    if framework == "net48":
        invalid = subprocess.run(
            [*build, "/p:PlatformTarget=AnyCPU"],
            cwd=project,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if invalid.returncode == 0 or "KB.NET48.X64" not in invalid.stdout:
            raise SystemExit("net48 package did not fail fast for a non-x64 consumer")
        build.append("/p:PlatformTarget=x64")

    run(build, cwd=project, environment=environment)
    assert_runtime_output(output, rid)
    if framework == "net48":
        run(
            [str(output / "KairosBoot.PackageSmoke.exe"), expected_native_version],
            cwd=output,
            environment=environment,
        )
    else:
        run(
            ["dotnet", str(output / "KairosBoot.PackageSmoke.dll"), expected_native_version],
            cwd=output,
            environment=environment,
        )


def run_classic_smoke(
    package: Path,
    expected_native_version: str,
    work: Path,
    environment: dict[str, str],
) -> None:
    if os.name != "nt":
        raise SystemExit("--classic requires Windows")
    nuget = shutil.which("nuget") or shutil.which("nuget.exe")
    msbuild = shutil.which("msbuild") or shutil.which("msbuild.exe")
    if not nuget or not msbuild:
        raise SystemExit("--classic requires nuget.exe and full-framework MSBuild.exe on PATH")

    version = package_version(package)
    feed = work / "classic-feed"
    feed.mkdir()
    shutil.copy2(package, feed / package.name)
    project = work / "classic"
    project.mkdir()
    shutil.copy2(SMOKE_SOURCE / "Program.cs", project / "Program.cs")
    for source_name, destination_name in (
        ("ClassicSmoke.csproj.template", "ClassicSmoke.csproj"),
        ("packages.config.template", "packages.config"),
    ):
        text = (SMOKE_SOURCE / source_name).read_text(encoding="utf-8")
        (project / destination_name).write_text(
            text.replace("__PACKAGE_VERSION__", version), encoding="utf-8"
        )

    packages = work / "packages"
    output = work / "classic-output"
    run(
        [
            nuget,
            "restore",
            str(project / "packages.config"),
            "-PackagesDirectory",
            str(packages),
            "-Source",
            str(feed),
            "-NonInteractive",
        ],
        cwd=project,
        environment=environment,
    )
    run(
        [
            msbuild,
            str(project / "ClassicSmoke.csproj"),
            "/nologo",
            "/m:1",
            "/p:Configuration=Release",
            "/p:Platform=x64",
            "/p:PlatformTarget=x64",
            f"/p:OutDir={output}{os.sep}",
        ],
        cwd=project,
        environment=environment,
    )
    assert_runtime_output(output, "win-x64")
    run(
        [str(output / "KairosBoot.PackageSmoke.Classic.exe"), expected_native_version],
        cwd=output,
        environment=environment,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--framework", choices=("net48", "net10.0"), required=True)
    parser.add_argument("--rid", choices=tuple(NATIVE_FILES), required=True)
    parser.add_argument("--expected-native-version")
    parser.add_argument("--classic", action="store_true")
    args = parser.parse_args()
    package = args.package.resolve()
    if not package.is_file() or package.suffix != ".nupkg":
        raise SystemExit(f"NuGet package does not exist: {package}")
    assert_package_licenses(package)
    if args.framework == "net48" and args.rid != "win-x64":
        raise SystemExit("net48 package smoke supports only win-x64")
    if args.framework == "net48" and os.name != "nt":
        raise SystemExit("net48 package smoke requires Windows")
    if args.classic and args.framework != "net48":
        raise SystemExit("--classic is valid only with --framework net48")
    expected_native_version = args.expected_native_version
    if not expected_native_version:
        version_file = REPOSITORY_ROOT / "version.json"
        if not version_file.is_file():
            raise SystemExit("--expected-native-version is required outside the repository")
        expected_native_version = str(
            json.loads(version_file.read_text(encoding="utf-8"))["version"]
        )

    with tempfile.TemporaryDirectory(prefix="kairosboot-package-smoke-") as temporary:
        work = Path(temporary)
        environment = dict(os.environ)
        environment["NUGET_PACKAGES"] = str(work / "global-packages")
        run_sdk_smoke(
            package,
            args.framework,
            args.rid,
            expected_native_version,
            work,
            environment,
        )
        if args.classic:
            run_classic_smoke(package, expected_native_version, work, environment)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
