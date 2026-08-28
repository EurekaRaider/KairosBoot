#!/usr/bin/env python3
"""Generate an in-toto statement for assets that are also GitHub-attested."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--signing-mode", choices=("off",), default="off")
    parser.add_argument("--commit")
    args = parser.parse_args()

    commit = args.commit or os.environ.get("GITHUB_SHA", "")
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise SystemExit("provenance requires a full lowercase release commit SHA-1")

    subjects = []
    for path in sorted(args.assets.iterdir()):
        if path.is_file() and path.resolve() != args.output.resolve():
            subjects.append({"name": path.name, "digest": {"sha256": sha256(path)}})
    statement = {
        "_type": "https://in-toto.io/Statement/v1",
        "subject": subjects,
        "predicateType": "https://slsa.dev/provenance/v1",
        "predicate": {
            "buildDefinition": {
                "buildType": "https://github.com/EurekaRaider/KairosBoot/.github/workflows/release.yml",
                "externalParameters": {"ref": os.environ.get("GITHUB_REF", "")},
                "internalParameters": {"signingMode": args.signing_mode},
                "resolvedDependencies": [{"uri": os.environ.get("GITHUB_SERVER_URL", "https://github.com") + "/" + os.environ.get("GITHUB_REPOSITORY", "EurekaRaider/KairosBoot"), "digest": {"gitCommit": commit}}]
            },
            "runDetails": {
                "builder": {"id": os.environ.get("GITHUB_SERVER_URL", "https://github.com") + "/" + os.environ.get("GITHUB_REPOSITORY", "EurekaRaider/KairosBoot") + "/actions/runs/" + os.environ.get("GITHUB_RUN_ID", "")},
                "metadata": {"invocationId": os.environ.get("GITHUB_RUN_ID", "")}
            }
        }
    }
    args.output.write_text(json.dumps(statement, separators=(",", ":")) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
