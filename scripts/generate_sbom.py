#!/usr/bin/env python3
"""Generate the small release-level SPDX 2.3 SBOM for KairosBoot."""

from __future__ import annotations

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--libusb-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    now = datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"KairosBoot-{args.version}",
        "documentNamespace": f"https://github.com/EurekaRaider/KairosBoot/releases/tag/v{args.version}/spdx",
        "creationInfo": {
            "created": now,
            "creators": ["Tool: KairosBoot-generate_sbom.py"]
        },
        "packages": [
            {
                "name": "KairosBoot",
                "SPDXID": "SPDXRef-Package-KairosBoot",
                "versionInfo": args.version,
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": False,
                "checksums": [{"algorithm": "SHA256", "checksumValue": sha256(args.source_archive)}],
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "copyrightText": "NOASSERTION"
            },
            {
                "name": "libusb",
                "SPDXID": "SPDXRef-Package-libusb",
                "versionInfo": "1.0.30",
                "downloadLocation": "https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2",
                "filesAnalyzed": False,
                "checksums": [{"algorithm": "SHA256", "checksumValue": sha256(args.libusb_source)}],
                "licenseConcluded": "LGPL-2.1-or-later",
                "licenseDeclared": "LGPL-2.1-or-later",
                "copyrightText": "NOASSERTION"
            }
        ],
        "relationships": [
            {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-Package-KairosBoot"},
            {"spdxElementId": "SPDXRef-Package-KairosBoot", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": "SPDXRef-Package-libusb"}
        ]
    }
    args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
