#!/usr/bin/env python3
"""Clean-install smoke for exact KairosBoot native Release archives."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]


def dependency_names(output: str, platform: str) -> set[str]:
    if platform.startswith("windows-"):
        return {
            line.strip().lower()
            for line in output.splitlines()
            if line.strip().lower().endswith(".dll")
        }
    if platform.startswith("macos-"):
        return {
            Path(line.strip().split(" ", 1)[0]).name.lower()
            for line in output.splitlines()[1:]
            if line.strip()
        }
    return {
        match.group(1).lower()
        for match in re.finditer(r"Shared library: \[([^]]+)\]", output)
    }


def is_forbidden_compression_runtime(name: str) -> bool:
    return any(
        re.fullmatch(pattern, name) is not None
        for pattern in (
            r"(?:lib)?miniz[^/\\]*\.(?:dll|dylib)",
            r"libminiz[^/\\]*\.so(?:\.[0-9]+)*",
            r"(?:lib)?zlib[0-9]*\.dll",
            r"libz(?:\.[0-9]+)*\.dylib",
            r"libz\.so(?:\.[0-9]+)*",
        )
    )


def verify_no_compression_runtime_dependency(
    library: Path, platform: str, dumpbin: Path | None, environment: dict[str, str]
) -> None:
    if platform.startswith("windows-"):
        if dumpbin is None:
            raise SystemExit("--dumpbin is required for Windows Release archive smoke")
        tool_name = dumpbin.name.lower()
        if tool_name in {"link", "link.exe", "lld-link", "lld-link.exe"}:
            command = [str(dumpbin), "/dump", "/nologo", "/dependents", str(library)]
        else:
            command = [str(dumpbin), "/nologo", "/dependents", str(library)]
    elif platform.startswith("macos-"):
        command = ["otool", "-L", str(library)]
    else:
        command = ["readelf", "-d", str(library)]
    output = run(command, cwd=ROOT, environment=environment)
    forbidden = sorted(
        name
        for name in dependency_names(output, platform)
        if is_forbidden_compression_runtime(name)
    )
    if forbidden:
        raise SystemExit(
            "forbidden compression runtime dependency: " + ", ".join(forbidden)
        )


def archive_members(path: Path) -> list[str]:
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            corrupt = archive.testzip()
            if corrupt is not None:
                raise SystemExit(f"corrupt ZIP member in {path.name}: {corrupt}")
            return archive.namelist()
    with tarfile.open(path, "r:gz") as archive:
        return [member.name for member in archive.getmembers()]


def validate_member(name: str, expected_root: str) -> None:
    if "\\" in name:
        raise SystemExit(f"archive member uses a non-portable separator: {name}")
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise SystemExit(f"unsafe archive member path: {name}")
    if not path.parts or path.parts[0] != expected_root:
        raise SystemExit(
            f"archive member is outside expected root {expected_root}: {name}"
        )


def extract_archive(path: Path, destination: Path, expected_root: str) -> Path:
    names = archive_members(path)
    if not names:
        raise SystemExit(f"Release archive is empty: {path}")
    for name in names:
        validate_member(name, expected_root)

    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            archive.extractall(destination)
    else:
        with tarfile.open(path, "r:gz") as archive:
            for member in archive.getmembers():
                if member.isdev() or member.isfifo():
                    raise SystemExit(
                        f"unsupported special archive member: {member.name}"
                    )
                if member.issym() or member.islnk():
                    target = PurePosixPath(member.name).parent / member.linkname
                    if target.is_absolute() or ".." in target.parts:
                        raise SystemExit(
                            f"unsafe archive link target: {member.name}"
                        )
            archive.extractall(destination)
    root = destination / expected_root
    if not root.is_dir():
        raise SystemExit(f"archive did not create expected root: {expected_root}")
    return root


def run(command: list[str], *, cwd: Path, environment: dict[str, str]) -> str:
    print("+", " ".join(command), flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.stdout:
        print(completed.stdout, end="")
    return completed.stdout


def prepend_path(environment: dict[str, str], key: str, value: Path) -> None:
    current = environment.get(key, "")
    environment[key] = str(value) if not current else f"{value}{os.pathsep}{current}"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument(
        "--platform",
        choices=(
            "windows-x64",
            "windows-arm64",
            "linux-x64",
            "linux-arm64",
            "macos-x64",
            "macos-arm64",
        ),
        required=True,
    )
    parser.add_argument("--generator", required=True)
    parser.add_argument("--architecture")
    parser.add_argument("--dumpbin", type=Path)
    args = parser.parse_args()

    dist = args.dist.resolve()
    if not dist.is_dir():
        raise SystemExit(f"Release archive directory does not exist: {dist}")
    is_windows = args.platform.startswith("windows-")
    if is_windows and args.dumpbin is None:
        raise SystemExit("--dumpbin is required for Windows Release archive smoke")
    if args.dumpbin is not None and not args.dumpbin.is_file():
        raise SystemExit(f"dumpbin executable does not exist: {args.dumpbin}")
    suffix = ".zip" if is_windows else ".tar.gz"
    base = f"KairosBoot-v{args.version}-{args.platform}"
    archives = {
        kind: dist / f"{base}-{kind}{suffix}"
        for kind in ("sdk", "cli", "symbols")
    }
    missing = [path.name for path in archives.values() if not path.is_file()]
    if missing:
        raise SystemExit(f"missing native Release archives: {', '.join(missing)}")

    with tempfile.TemporaryDirectory(prefix="kairosboot-native-smoke-") as temporary:
        work = Path(temporary)
        sdk_root = extract_archive(archives["sdk"], work / "sdk", f"{base}-sdk")
        cli_root = extract_archive(archives["cli"], work / "cli", f"{base}-cli")
        for archive_root in (sdk_root, cli_root):
            miniz_license = archive_root / "share" / "kairosboot" / "miniz" / "LICENSE"
            if not miniz_license.is_file():
                raise SystemExit(
                    f"Release archive is missing the miniz license: {miniz_license}"
                )
        symbol_names = archive_members(archives["symbols"])
        for name in symbol_names:
            validate_member(name, f"{base}-symbols")
        if not any(not name.endswith("/") for name in symbol_names):
            raise SystemExit("Release symbols archive has no files")

        consumer = work / "consumer"
        configure = [
            "cmake",
            "-S",
            str(ROOT / "tests" / "package_consumer"),
            "-B",
            str(consumer),
            "-G",
            args.generator,
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_PREFIX_PATH={sdk_root}",
        ]
        if args.architecture:
            configure.extend(("-A", args.architecture))

        environment = dict(os.environ)
        if is_windows:
            prepend_path(environment, "PATH", sdk_root / "bin")
        elif args.platform.startswith("macos-"):
            prepend_path(environment, "DYLD_LIBRARY_PATH", sdk_root / "lib")
        else:
            prepend_path(environment, "LD_LIBRARY_PATH", sdk_root / "lib")

        run(configure, cwd=ROOT, environment=environment)
        run(
            ["cmake", "--build", str(consumer), "--config", "Release", "--parallel"],
            cwd=ROOT,
            environment=environment,
        )
        executable_dir = consumer / "Release" if is_windows else consumer
        executable_suffix = ".exe" if is_windows else ""
        for name in ("c_consumer", "cxx_consumer"):
            executable = executable_dir / f"{name}{executable_suffix}"
            if not executable.is_file():
                raise SystemExit(f"consumer executable is missing: {executable}")
            run([str(executable)], cwd=executable_dir, environment=environment)

        cli = cli_root / "bin" / f"kairosboot{executable_suffix}"
        if not cli.is_file():
            raise SystemExit(f"CLI executable is missing from archive: {cli}")
        cli_environment = dict(os.environ)
        if is_windows:
            prepend_path(cli_environment, "PATH", cli_root / "bin")
        elif args.platform.startswith("macos-"):
            prepend_path(cli_environment, "DYLD_LIBRARY_PATH", cli_root / "lib")
        else:
            prepend_path(cli_environment, "LD_LIBRARY_PATH", cli_root / "lib")
        cli_output = run(
            [str(cli), "--version"], cwd=cli.parent, environment=cli_environment
        )
        if args.version not in cli_output:
            raise SystemExit(
                f"CLI version output does not contain {args.version}: {cli_output!r}"
            )

        if is_windows:
            library = sdk_root / "bin" / "kairosboot.dll"
        elif args.platform.startswith("macos-"):
            library = sdk_root / "lib" / f"libkairosboot.{args.version}.dylib"
        else:
            library = sdk_root / "lib" / f"libkairosboot.so.{args.version}"
        if not library.is_file():
            raise SystemExit(f"SDK library is missing: {library}")
        verify_no_compression_runtime_dependency(
            library, args.platform, args.dumpbin, environment
        )
        abi_command = [
            sys.executable,
            str(ROOT / "scripts" / "check_abi.py"),
            "--library",
            str(library),
        ]
        if args.dumpbin is not None:
            abi_command.extend(("--dumpbin", str(args.dumpbin.resolve())))
        run(abi_command, cwd=ROOT, environment=environment)

    print(f"Native Release archive smoke passed: {args.platform}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.returncode) from error
