#!/usr/bin/env python3
"""Prepare the locked libusb 1.0.30 dynamic dependency for a native build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tarfile
import tempfile
import urllib.request
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_API_VERSION = "0x0100010C"
PE_MACHINES = {"x64": 0x8664, "arm64": 0xAA64}
WINDOWS_LAYOUTS = {
    "x64": Path("VS2022/MS64/dll"),
    "arm64": Path("VS2025/ARM64/dll"),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_sha256(path: Path, expected: str) -> None:
    actual = sha256(path)
    if actual != expected:
        raise SystemExit(f"SHA-256 mismatch for {path.name}: expected {expected}, got {actual}")


def download(url: str, destination: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "KairosBoot-dependency-preparer"})
    with urllib.request.urlopen(request, timeout=60) as response, destination.open("wb") as output:
        if response.status != 200:
            raise SystemExit(f"dependency download returned HTTP {response.status}: {url}")
        shutil.copyfileobj(response, output, length=1024 * 1024)


def obtain_archive(local: Path | None, url: str, expected: str, destination: Path) -> Path:
    if local is None:
        download(url, destination)
        archive = destination
    else:
        archive = local.resolve()
        if not archive.is_file():
            raise SystemExit(f"dependency archive does not exist: {archive}")
    verify_sha256(archive, expected)
    return archive


def safe_extract_tar(archive: Path, destination: Path) -> None:
    with tarfile.open(archive, "r:bz2") as package:
        for member in package.getmembers():
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts:
                raise SystemExit(f"unsafe path in source archive: {member.name}")
            if member.issym() or member.islnk():
                link = PurePosixPath(member.linkname)
                if link.is_absolute() or ".." in link.parts:
                    raise SystemExit(f"unsafe link in source archive: {member.name}")
        package.extractall(destination)


def validate_header(header: Path) -> None:
    text = header.read_text(encoding="utf-8")
    definition = f"#define LIBUSB_API_VERSION {EXPECTED_API_VERSION}"
    if definition not in text:
        raise SystemExit(f"{header} is not the locked libusb 1.0.30 header")


def validate_pe_machine(library: Path, architecture: str) -> None:
    expected = PE_MACHINES[architecture]
    with library.open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise SystemExit(f"not a PE image: {library}")
        stream.seek(0x3C)
        offset_bytes = stream.read(4)
        if len(offset_bytes) != 4:
            raise SystemExit(f"truncated PE image: {library}")
        offset = struct.unpack("<I", offset_bytes)[0]
        stream.seek(offset)
        if stream.read(4) != b"PE\0\0":
            raise SystemExit(f"invalid PE signature: {library}")
        machine_bytes = stream.read(2)
        if len(machine_bytes) != 2:
            raise SystemExit(f"truncated PE machine field: {library}")
        actual = struct.unpack("<H", machine_bytes)[0]
    if actual != expected:
        raise SystemExit(
            f"wrong libusb architecture: expected 0x{expected:04x}, got 0x{actual:04x}"
        )


def ensure_empty_prefix(prefix: Path) -> None:
    if prefix.exists() and any(prefix.iterdir()):
        raise SystemExit(f"refusing to overwrite non-empty dependency prefix: {prefix}")
    prefix.mkdir(parents=True, exist_ok=True)


def copy_license(source_root: Path, prefix: Path) -> None:
    destination = prefix / "share" / "libusb"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_root / "COPYING", destination / "COPYING")


def prepare_windows(binary_archive: Path, source_root: Path, prefix: Path, architecture: str) -> None:
    with tempfile.TemporaryDirectory(prefix="kairosboot-libusb-windows-") as temporary:
        extracted = Path(temporary)
        seven_zip = shutil.which("7z") or shutil.which("7zz")
        if seven_zip is not None:
            subprocess.run(
                [seven_zip, "x", "-y", f"-o{extracted}", str(binary_archive)],
                check=True,
            )
        else:
            subprocess.run(
                ["tar", "-xf", str(binary_archive), "-C", str(extracted)], check=True
            )
        header = extracted / "include" / "libusb.h"
        binary_root = extracted / WINDOWS_LAYOUTS[architecture]
        validate_header(header)
        validate_pe_machine(binary_root / "libusb-1.0.dll", architecture)

        include = prefix / "include" / "libusb-1.0"
        library = prefix / "lib"
        runtime = prefix / "bin"
        symbols = prefix / "symbols"
        for directory in (include, library, runtime, symbols):
            directory.mkdir(parents=True, exist_ok=True)
        shutil.copy2(header, include / "libusb.h")
        shutil.copy2(binary_root / "libusb-1.0.lib", library / "libusb-1.0.lib")
        shutil.copy2(binary_root / "libusb-1.0.dll", runtime / "libusb-1.0.dll")
        shutil.copy2(binary_root / "libusb-1.0.pdb", symbols / "libusb-1.0.pdb")
    copy_license(source_root, prefix)


def prepare_unix(source_root: Path, prefix: Path, jobs: int, platform: str) -> None:
    environment = dict(os.environ)
    subprocess.run(
        [
            str(source_root / "configure"),
            f"--prefix={prefix}",
            "--disable-static",
            "--enable-shared",
            "--disable-udev",
        ],
        cwd=source_root,
        env=environment,
        check=True,
    )
    subprocess.run(["make", f"-j{jobs}"], cwd=source_root, env=environment, check=True)
    subprocess.run(["make", "install"], cwd=source_root, env=environment, check=True)
    validate_header(prefix / "include" / "libusb-1.0" / "libusb.h")
    if platform == "macos":
        macos_library = prefix / "lib" / "libusb-1.0.0.dylib"
        if not macos_library.is_file():
            raise SystemExit("libusb build did not produce the expected macOS dylib")
        subprocess.run(
            [
                "install_name_tool",
                "-id",
                "@rpath/libusb-1.0.0.dylib",
                str(macos_library),
            ],
            check=True,
        )
    dynamic_libraries = [
        path
        for path in (prefix / "lib").glob("libusb-1.0.*")
        if path.suffix in {".so", ".dylib"} or ".so." in path.name
    ]
    if not dynamic_libraries:
        raise SystemExit("libusb build did not produce a dynamic library")
    copy_license(source_root, prefix)


def write_manifest(prefix: Path, lock: dict[str, object], platform: str, architecture: str) -> None:
    libusb = lock["libusb"]
    assert isinstance(libusb, dict)
    manifest = {
        "name": "libusb",
        "version": libusb["requiredVersion"],
        "linkage": "dynamic",
        "platform": platform,
        "architecture": architecture,
        "source": {
            "url": libusb["sourceArchive"],
            "sha256": libusb["sourceArchiveSha256"],
        },
    }
    if platform == "windows":
        manifest["binary"] = {
            "url": libusb["windowsBinaryArchive"],
            "sha256": libusb["windowsBinaryArchiveSha256"],
        }
    destination = prefix / "share" / "libusb"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "kairosboot-libusb.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--platform", choices=("windows", "linux", "macos"), required=True)
    parser.add_argument("--architecture", choices=("x64", "arm64"), required=True)
    parser.add_argument("--source-archive", type=Path)
    parser.add_argument("--windows-archive", type=Path)
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.jobs < 1:
        raise SystemExit("--jobs must be positive")

    lock = json.loads((ROOT / "compat" / "aosp.lock.json").read_text(encoding="utf-8"))
    libusb = lock.get("libusb")
    if not isinstance(libusb, dict) or libusb.get("requiredVersion") != "1.0.30":
        raise SystemExit("compat/aosp.lock.json does not lock libusb 1.0.30")
    prefix = args.prefix.resolve()
    ensure_empty_prefix(prefix)

    with tempfile.TemporaryDirectory(prefix="kairosboot-libusb-source-") as temporary:
        temporary_root = Path(temporary)
        source_archive = obtain_archive(
            args.source_archive,
            str(libusb["sourceArchive"]),
            str(libusb["sourceArchiveSha256"]),
            temporary_root / "libusb-1.0.30.tar.bz2",
        )
        source_extract = temporary_root / "source"
        source_extract.mkdir()
        safe_extract_tar(source_archive, source_extract)
        source_root = source_extract / "libusb-1.0.30"
        if not (source_root / "configure").is_file() or not (source_root / "COPYING").is_file():
            raise SystemExit("libusb source archive has an unexpected layout")

        if args.platform == "windows":
            binary_archive = obtain_archive(
                args.windows_archive,
                str(libusb["windowsBinaryArchive"]),
                str(libusb["windowsBinaryArchiveSha256"]),
                temporary_root / "libusb-1.0.30.7z",
            )
            prepare_windows(binary_archive, source_root, prefix, args.architecture)
        else:
            prepare_unix(source_root, prefix, args.jobs, args.platform)

    write_manifest(prefix, lock, args.platform, args.architecture)
    print(f"prepared libusb 1.0.30 dynamic dependency at {prefix}")


if __name__ == "__main__":
    main()
