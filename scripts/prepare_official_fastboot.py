#!/usr/bin/env python3
"""Prepare one immutable official Platform-Tools Fastboot binary for CI."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import stat
import sys
import tempfile
import urllib.request
import zipfile


MAX_ARCHIVE_BYTES = 256 * 1024 * 1024


class PreparationError(RuntimeError):
    pass


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _download(url: str, destination: pathlib.Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "KairosBoot-CI/1"})
    with urllib.request.urlopen(request, timeout=60) as response, destination.open("wb") as output:
        declared = response.headers.get("Content-Length")
        if declared is not None and int(declared) > MAX_ARCHIVE_BYTES:
            raise PreparationError("official Platform-Tools archive exceeds size limit")
        total = 0
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > MAX_ARCHIVE_BYTES:
                raise PreparationError("official Platform-Tools archive exceeds size limit")
            output.write(chunk)


def _safe_member_path(root: pathlib.Path, member: str) -> pathlib.Path:
    relative = pathlib.PurePosixPath(member)
    if relative.is_absolute() or ".." in relative.parts:
        raise PreparationError(f"unsafe Platform-Tools archive member: {member!r}")
    destination = root.joinpath(*relative.parts)
    try:
        destination.resolve().relative_to(root.resolve())
    except ValueError as error:
        raise PreparationError(f"unsafe Platform-Tools archive member: {member!r}") from error
    return destination


def _extract_archive(archive: pathlib.Path, output_dir: pathlib.Path) -> None:
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            destination = _safe_member_path(output_dir, member.filename)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise PreparationError(
                    f"symlinks are forbidden in Platform-Tools archive: {member.filename!r}"
                )
            if member.is_dir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            with package.open(member) as source, destination.open("wb") as target:
                shutil.copyfileobj(source, target)


def prepare(
    repository_root: pathlib.Path,
    platform_key: str,
    output_dir: pathlib.Path,
    archive_override: pathlib.Path | None,
) -> pathlib.Path:
    lock_path = repository_root / "compat" / "aosp.lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    try:
        version = lock["aosp"]["platformToolsVersion"]
        entry = lock["aosp"]["officialArchives"][platform_key]
        url = entry["url"]
        expected_archive = entry["sha256"]
        expected_fastboot = entry["fastbootSha256"]
    except (KeyError, TypeError) as error:
        raise PreparationError(f"invalid official Platform-Tools lock: {lock_path}") from error
    if version != "37.0.1" or not url.startswith("https://dl.google.com/android/repository/"):
        raise PreparationError("official Platform-Tools lock is not the immutable 37.0.1 baseline")

    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="kairosboot-platform-tools-") as temporary:
        archive = pathlib.Path(temporary) / "platform-tools.zip"
        if archive_override is None:
            _download(url, archive)
        else:
            shutil.copyfile(archive_override, archive)
        observed_archive = _sha256(archive)
        if observed_archive != expected_archive:
            raise PreparationError(
                "official Platform-Tools archive SHA-256 mismatch: "
                f"expected {expected_archive}, observed {observed_archive}"
            )
        _extract_archive(archive, output_dir)

    executable = "fastboot.exe" if platform_key == "windows" else "fastboot"
    fastboot = output_dir / "platform-tools" / executable
    if not fastboot.is_file():
        raise PreparationError(f"official archive is missing platform-tools/{executable}")
    observed_fastboot = _sha256(fastboot)
    if observed_fastboot != expected_fastboot:
        raise PreparationError(
            "official Fastboot SHA-256 mismatch: "
            f"expected {expected_fastboot}, observed {observed_fastboot}"
        )
    if platform_key != "windows":
        fastboot.chmod(fastboot.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return fastboot


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--platform", choices=("linux", "windows", "darwin"), required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--archive", type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        fastboot = prepare(
            arguments.repository_root.resolve(),
            arguments.platform,
            arguments.output_dir,
            arguments.archive,
        )
    except (OSError, ValueError, zipfile.BadZipFile, PreparationError) as error:
        print(f"official Fastboot preparation failed: {error}", file=sys.stderr)
        return 2
    print(fastboot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
