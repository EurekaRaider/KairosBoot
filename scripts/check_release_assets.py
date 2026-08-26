#!/usr/bin/env python3
"""Generate and validate the explicit KairosBoot Release asset contract."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "release" / "expected-assets.json"
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")


def expected_assets(version: str) -> list[str]:
    if not VERSION.fullmatch(version):
        raise SystemExit("version must be MAJOR.MINOR.PATCH")
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    if contract.get("schemaVersion") != 1:
        raise SystemExit("unsupported Release asset contract schema")

    names: list[str] = []
    kinds = contract.get("nativeKinds")
    platforms = contract.get("platforms")
    additional = contract.get("additionalAssets")
    if not isinstance(kinds, list) or not isinstance(platforms, list) or not isinstance(additional, list):
        raise SystemExit("Release asset contract has invalid collection fields")

    for platform in platforms:
        if not isinstance(platform, dict):
            raise SystemExit("Release asset platform entry must be an object")
        identifier = platform.get("id")
        extension = platform.get("archiveExtension")
        if not isinstance(identifier, str) or not isinstance(extension, str):
            raise SystemExit("Release asset platform id/extension must be strings")
        for kind in kinds:
            if not isinstance(kind, str):
                raise SystemExit("Release native asset kind must be a string")
            names.append(
                f"KairosBoot-v{version}-{identifier}-{kind}.{extension}"
            )

    for template in additional:
        if not isinstance(template, str):
            raise SystemExit("additional Release asset name must be a string")
        names.append(template.format(version=version))
    if len(names) != len(set(names)):
        raise SystemExit("Release asset contract expands to duplicate names")
    return sorted(names)


def manifest_payload(version: str, names: list[str]) -> dict[str, object]:
    return {
        "documentType": "kairosboot.release-assets",
        "schemaVersion": 1,
        "version": version,
        "assets": names,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--write-manifest", action="store_true")
    parser.add_argument("--validate", action="store_true")
    args = parser.parse_args()
    if not args.write_manifest and not args.validate:
        parser.error("at least one of --write-manifest or --validate is required")

    assets = args.assets.resolve()
    if not assets.is_dir():
        raise SystemExit(f"Release asset directory does not exist: {assets}")
    names = expected_assets(args.version)
    manifest_name = f"KairosBoot-v{args.version}-assets.json"
    manifest = assets / manifest_name
    payload = manifest_payload(args.version, names)

    if args.write_manifest:
        manifest.write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    if args.validate:
        if not manifest.is_file():
            raise SystemExit(f"Release asset manifest is missing: {manifest_name}")
        actual_payload = json.loads(manifest.read_text(encoding="utf-8"))
        if actual_payload != payload:
            raise SystemExit("Release asset manifest does not match the frozen contract")
        actual = sorted(path.name for path in assets.iterdir() if path.is_file())
        missing = sorted(set(names) - set(actual))
        extra = sorted(set(actual) - set(names))
        if missing or extra:
            raise SystemExit(
                "Release asset set differs from the frozen contract; "
                f"missing={missing}, extra={extra}"
            )
        print(f"Release asset contract passed ({len(names)} files)")


if __name__ == "__main__":
    main()
