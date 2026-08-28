#!/usr/bin/env python3
"""Generate and validate the explicit KairosBoot Release asset contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "release" / "expected-assets.json"
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
SIGNING_MODES = ("off",)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


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


def write_checksums(assets: Path, names: list[str]) -> None:
    checksum_name = "SHA256SUMS"
    lines = [
        f"{sha256(assets / name)}  {name}"
        for name in names
        if name != checksum_name
    ]
    (assets / checksum_name).write_text("\n".join(lines) + "\n", encoding="utf-8")


def validate_checksums(assets: Path, names: list[str]) -> None:
    checksum_path = assets / "SHA256SUMS"
    entries: dict[str, str] = {}
    for line in checksum_path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", line)
        if match is None:
            raise SystemExit(f"invalid SHA256SUMS line: {line!r}")
        digest, name = match.groups()
        if name in entries:
            raise SystemExit(f"duplicate SHA256SUMS entry: {name}")
        entries[name] = digest
    expected = set(names) - {"SHA256SUMS"}
    if set(entries) != expected:
        raise SystemExit(
            "SHA256SUMS asset set differs from the Release contract; "
            f"missing={sorted(expected - set(entries))}, "
            f"extra={sorted(set(entries) - expected)}"
        )
    for name, digest in entries.items():
        actual = sha256(assets / name)
        if digest != actual:
            raise SystemExit(
                f"SHA256SUMS mismatch for {name}: expected {digest}, got {actual}"
            )


def validate_signing(assets: Path, version: str, signing_mode: str) -> None:
    if signing_mode not in SIGNING_MODES:
        raise SystemExit(
            f"unsupported SIGNING_MODE {signing_mode!r}; credentialed signing is not configured"
        )
    status = json.loads((assets / "signing-status.json").read_text(encoding="utf-8"))
    expected = {
        "schemaVersion": 1,
        "mode": "off",
        "authenticode": False,
        "developerId": False,
        "notarized": False,
        "checksumSignature": False,
    }
    if status != expected:
        raise SystemExit("signing-status.json does not match SIGNING_MODE=off")
    unsigned = assets / f"KairosBoot-v{version}-UNSIGNED.txt"
    if "unsigned" not in unsigned.read_text(encoding="utf-8").lower():
        raise SystemExit("UNSIGNED.txt does not declare that native artifacts are unsigned")


def validate_sbom(assets: Path, version: str) -> None:
    document = json.loads((assets / "KairosBoot.spdx.json").read_text(encoding="utf-8"))
    if document.get("spdxVersion") != "SPDX-2.3":
        raise SystemExit("Release SBOM must use SPDX-2.3")
    packages = {
        package.get("name"): package
        for package in document.get("packages", [])
        if isinstance(package, dict)
    }
    expected = {
        "KairosBoot": (version, f"KairosBoot-v{version}-source.tar.gz"),
        "libusb": ("1.0.30", "libusb-1.0.30.tar.bz2"),
    }
    for name, (package_version, asset_name) in expected.items():
        package = packages.get(name)
        if not isinstance(package, dict) or package.get("versionInfo") != package_version:
            raise SystemExit(f"Release SBOM has the wrong {name} version")
        checksums = package.get("checksums")
        if not isinstance(checksums, list) or len(checksums) != 1:
            raise SystemExit(f"Release SBOM has invalid {name} checksums")
        checksum = checksums[0]
        if (
            not isinstance(checksum, dict)
            or checksum.get("algorithm") != "SHA256"
            or checksum.get("checksumValue") != sha256(assets / asset_name)
        ):
            raise SystemExit(f"Release SBOM checksum mismatch for {name}")


def validate_provenance(
    assets: Path, names: list[str], signing_mode: str, expected_commit: str | None
) -> None:
    provenance = json.loads(
        (assets / "provenance.intoto.json").read_text(encoding="utf-8")
    )
    if provenance.get("_type") != "https://in-toto.io/Statement/v1":
        raise SystemExit("Release provenance has the wrong statement type")
    build_definition = provenance.get("predicate", {}).get("buildDefinition", {})
    if build_definition.get("internalParameters", {}).get("signingMode") != signing_mode:
        raise SystemExit("Release provenance signing mode does not match")
    if expected_commit is not None:
        dependencies = build_definition.get("resolvedDependencies")
        if not isinstance(dependencies, list) or len(dependencies) != 1:
            raise SystemExit("Release provenance must name one source dependency")
        if dependencies[0].get("digest", {}).get("gitCommit") != expected_commit:
            raise SystemExit("Release provenance commit does not match the exact Release head")
    subjects = provenance.get("subject")
    if not isinstance(subjects, list):
        raise SystemExit("Release provenance subjects must be a list")
    actual: dict[str, str] = {}
    for subject in subjects:
        if not isinstance(subject, dict) or not isinstance(subject.get("digest"), dict):
            raise SystemExit("Release provenance contains an invalid subject")
        name = subject.get("name")
        digest = subject["digest"].get("sha256")
        if not isinstance(name, str) or not isinstance(digest, str) or not SHA256.fullmatch(digest):
            raise SystemExit("Release provenance contains an invalid subject digest")
        if name in actual:
            raise SystemExit(f"Release provenance contains a duplicate subject: {name}")
        actual[name] = digest
    expected = set(names) - {"SHA256SUMS", "provenance.intoto.json"}
    if set(actual) != expected:
        raise SystemExit(
            "Release provenance subject set differs from the asset contract; "
            f"missing={sorted(expected - set(actual))}, extra={sorted(set(actual) - expected)}"
        )
    for name, digest in actual.items():
        if digest != sha256(assets / name):
            raise SystemExit(f"Release provenance digest mismatch for {name}")


def validate_contents(
    assets: Path,
    version: str,
    names: list[str],
    signing_mode: str,
    expected_commit: str | None,
) -> None:
    for name in names:
        if (assets / name).stat().st_size == 0:
            raise SystemExit(f"Release asset is empty: {name}")
    validate_checksums(assets, names)
    validate_signing(assets, version, signing_mode)
    validate_sbom(assets, version)
    validate_provenance(assets, names, signing_mode, expected_commit)
    if (assets / "libusb-1.0.30-COPYING").stat().st_size == 0:
        raise SystemExit("libusb 1.0.30 COPYING asset is empty")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--write-manifest", action="store_true")
    parser.add_argument("--write-checksums", action="store_true")
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--signing-mode", default="off")
    parser.add_argument("--commit")
    args = parser.parse_args()
    if not args.write_manifest and not args.write_checksums and not args.validate:
        parser.error(
            "at least one of --write-manifest, --write-checksums, or --validate is required"
        )
    if args.signing_mode not in SIGNING_MODES:
        raise SystemExit(
            f"unsupported SIGNING_MODE {args.signing_mode!r}; credentialed signing is not configured"
        )
    if args.commit is not None and re.fullmatch(r"[0-9a-f]{40}", args.commit) is None:
        raise SystemExit("release commit must be a full lowercase SHA-1")

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

    if args.write_checksums:
        missing = sorted(
            name for name in names if name != "SHA256SUMS" and not (assets / name).is_file()
        )
        if missing:
            raise SystemExit(f"cannot write SHA256SUMS; missing={missing}")
        write_checksums(assets, names)

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
        validate_contents(
            assets, args.version, names, args.signing_mode, args.commit
        )
        print(f"Release asset contract passed ({len(names)} files)")


if __name__ == "__main__":
    main()
