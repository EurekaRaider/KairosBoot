#!/usr/bin/env python3
"""Create deterministic-shape native SDK and CLI release archives."""

from __future__ import annotations

import argparse
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path


def add_tree_to_tar(archive: tarfile.TarFile, source: Path, prefix: str) -> None:
    for path in sorted(source.rglob("*")):
        archive.add(path, arcname=f"{prefix}/{path.relative_to(source)}", recursive=False)


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
        with tempfile.TemporaryDirectory(prefix="kairosboot-symbols-") as temporary:
            symbols = Path(temporary)
            matches = []
            for path in sorted(args.symbols_root.rglob("*")):
                name = path.name.lower()
                if "kairosboot" not in name:
                    continue
                if path.is_file() and (name.endswith(".pdb") or name.endswith(".debug")):
                    matches.append(path)
                elif path.is_dir() and name.endswith(".dsym"):
                    matches.append(path)
            for index, source in enumerate(matches):
                destination = symbols / f"{index:02d}-{source.name}"
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
