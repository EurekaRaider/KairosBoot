#!/usr/bin/env python3
"""Create deterministic-shape native SDK and CLI release archives."""

from __future__ import annotations

import argparse
import json
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path


def require_release_evidence(install_root: Path, platform: str) -> None:
    path = install_root / "share" / "kairosboot" / "release-build.json"
    if not path.is_file():
        raise SystemExit("install root is missing Release build evidence")
    evidence = json.loads(path.read_text(encoding="utf-8"))
    expected = {
        "documentType": "kairosboot.release-build",
        "schemaVersion": 1,
        "platform": platform,
        "configuration": "Release",
        "optimization": "O2" if platform.startswith("windows-") else "O3",
        "ndebug": True,
    }
    for key, value in expected.items():
        if evidence.get(key) != value:
            raise SystemExit(
                f"invalid Release build evidence field {key}: {evidence.get(key)!r}"
            )
    flags = evidence.get("cmakeFlags")
    if not isinstance(flags, dict) or set(flags) != {
        "CMAKE_C_FLAGS_RELEASE",
        "CMAKE_CXX_FLAGS_RELEASE",
    }:
        raise SystemExit("Release build evidence has invalid CMake flags")
    required_flags = ("/O2", "/DNDEBUG") if platform.startswith("windows-") else ("-O3", "-DNDEBUG")
    for name, value in flags.items():
        if not isinstance(value, str) or any(flag not in value.split() for flag in required_flags):
            raise SystemExit(f"Release build evidence has invalid {name}: {value!r}")


def normalize_tar_mode(member: tarfile.TarInfo, relative: Path) -> tarfile.TarInfo:
    """Give installed command files a portable executable archive mode."""
    if member.isfile() and relative.parts[:1] == ("bin",):
        member.mode = 0o755
    return member


def add_tree_to_tar(archive: tarfile.TarFile, source: Path, prefix: str) -> None:
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        archive.add(
            path,
            arcname=f"{prefix}/{relative}",
            recursive=False,
            filter=lambda member, relative=relative: normalize_tar_mode(member, relative),
        )


def add_tree_to_zip(archive: zipfile.ZipFile, source: Path, prefix: str) -> None:
    for path in sorted(source.rglob("*")):
        if path.is_file():
            archive.write(path, arcname=f"{prefix}/{path.relative_to(source)}")


def make_archive(source: Path, output: Path, prefix: str, use_zip: bool) -> None:
    if use_zip:
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            add_tree_to_zip(archive, source, prefix)
    else:
        with tarfile.open(output, "w:gz", format=tarfile.PAX_FORMAT) as archive:
            add_tree_to_tar(archive, source, prefix)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--install-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--symbols-root", type=Path)
    args = parser.parse_args()

    install_root = args.install_root.resolve()
    if not (install_root / "bin").is_dir() or not (install_root / "include").is_dir():
        raise SystemExit("install root does not contain the expected SDK layout")
    require_release_evidence(install_root, args.platform)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    use_zip = args.platform.startswith("windows-")
    suffix = ".zip" if use_zip else ".tar.gz"
    base = f"KairosBoot-v{args.version}-{args.platform}"
    make_archive(install_root, args.output_dir / f"{base}-sdk{suffix}", f"{base}-sdk", use_zip)

    with tempfile.TemporaryDirectory(prefix="kairosboot-cli-") as temporary:
        cli_root = Path(temporary)
        shutil.copytree(install_root / "bin", cli_root / "bin", symlinks=True)
        if (install_root / "lib").is_dir():
            shutil.copytree(install_root / "lib", cli_root / "lib", symlinks=True)
            shutil.rmtree(cli_root / "lib" / "cmake", ignore_errors=True)
        if (install_root / "share").is_dir():
            shutil.copytree(install_root / "share", cli_root / "share", symlinks=True)
        make_archive(cli_root, args.output_dir / f"{base}-cli{suffix}", f"{base}-cli", use_zip)

    if args.symbols_root is not None:
        symbols_root = args.symbols_root.resolve()
        with tempfile.TemporaryDirectory(prefix="kairosboot-symbols-") as temporary:
            symbols = Path(temporary)
            matches = []
            for path in sorted(symbols_root.rglob("*")):
                name = path.name.lower()
                if path.is_file() and (name.endswith(".pdb") or name.endswith(".debug")):
                    matches.append(path)
                elif path.is_dir() and name.endswith(".dsym"):
                    matches.append(path)
            for source in matches:
                destination = symbols / source.name
                if destination.exists():
                    raise SystemExit(f"duplicate symbol basename: {source.name}")
                if source.is_dir():
                    shutil.copytree(source, destination, symlinks=True)
                else:
                    shutil.copy2(source, destination)
            if not matches:
                raise SystemExit("no KairosBoot symbol files were found")
            make_archive(
                symbols,
                args.output_dir / f"{base}-symbols{suffix}",
                f"{base}-symbols",
                use_zip,
            )


if __name__ == "__main__":
    main()
