#!/usr/bin/env python3
"""Validate tag/version and exact-main checkout inputs before Release builds."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


TAG = re.compile(r"^v([0-9]+\.[0-9]+\.[0-9]+)$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")


def fail(message: str) -> None:
    raise SystemExit(f"Release context gate failed: {message}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version-file", type=Path, required=True)
    parser.add_argument("--tag-commit", required=True)
    parser.add_argument("--main-commit", required=True)
    parser.add_argument("--checkout-commit", required=True)
    args = parser.parse_args()

    match = TAG.fullmatch(args.tag)
    if match is None:
        fail("tag must be vMAJOR.MINOR.PATCH")
    version = json.loads(args.version_file.read_text(encoding="utf-8")).get("version")
    if version != match.group(1):
        fail(f"tag {args.tag} does not match version.json {version!r}")
    commits = {
        "tag": args.tag_commit,
        "main": args.main_commit,
        "checkout": args.checkout_commit,
    }
    for name, value in commits.items():
        if COMMIT.fullmatch(value) is None:
            fail(f"{name} commit is not a full lowercase SHA-1")
    if len(set(commits.values())) != 1:
        fail(
            "tag, origin/main, and checked-out commit must be the same exact head; "
            + ", ".join(f"{name}={value}" for name, value in commits.items())
        )
    print(f"Release context gate passed: {args.tag} at {args.tag_commit}")


if __name__ == "__main__":
    main()
