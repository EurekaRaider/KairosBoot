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
    parser.add_argument("--boost-source", type=Path, required=True)
    parser.add_argument("--miniz-source", type=Path, required=True)
    parser.add_argument("--yaml-cpp-source", type=Path, required=True)
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
            },
            {
                "name": "Boost",
                "SPDXID": "SPDXRef-Package-Boost",
                "versionInfo": "1.92.0",
                "downloadLocation": "https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz",
                "filesAnalyzed": False,
                "checksums": [{"algorithm": "SHA256", "checksumValue": sha256(args.boost_source)}],
                "licenseConcluded": "BSL-1.0",
                "licenseDeclared": "BSL-1.0",
                "copyrightText": "NOASSERTION",
                "comment": "Boost.Asio and its transitive Boost header dependencies are fetched at configure time."
            },
            {
                "name": "miniz",
                "SPDXID": "SPDXRef-Package-miniz",
                "versionInfo": "3.1.2",
                "downloadLocation": "https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip",
                "filesAnalyzed": False,
                "checksums": [{"algorithm": "SHA256", "checksumValue": sha256(args.miniz_source)}],
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "copyrightText": "NOASSERTION",
                "comment": "miniz is built as a private static archive dependency with CRC validation enabled."
            },
            {
                "name": "yaml-cpp",
                "SPDXID": "SPDXRef-Package-yaml-cpp",
                "versionInfo": "0.9.0",
                "downloadLocation": "https://github.com/jbeder/yaml-cpp/releases/download/yaml-cpp-0.9.0/yaml-cpp-yaml-cpp-0.9.0.tar.gz",
                "filesAnalyzed": False,
                "checksums": [{"algorithm": "SHA256", "checksumValue": sha256(args.yaml_cpp_source)}],
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "copyrightText": "NOASSERTION",
                "comment": "yaml-cpp is built as a private static Fleet manifest parser dependency."
            },
            {
                "name": "Microsoft Visual C++ Runtime",
                "SPDXID": "SPDXRef-Package-MSVC-Runtime",
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": False,
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "NOASSERTION",
                "supplier": "Organization: Microsoft Corporation",
                "copyrightText": "NOASSERTION",
                "comment": "Bundled app-local only in Windows native and NuGet assets."
            }
        ],
        "relationships": [
            {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-Package-KairosBoot"},
            {"spdxElementId": "SPDXRef-Package-KairosBoot", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": "SPDXRef-Package-libusb"},
            {"spdxElementId": "SPDXRef-Package-KairosBoot", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": "SPDXRef-Package-Boost"},
            {"spdxElementId": "SPDXRef-Package-KairosBoot", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": "SPDXRef-Package-miniz"},
            {"spdxElementId": "SPDXRef-Package-KairosBoot", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": "SPDXRef-Package-yaml-cpp"},
            {"spdxElementId": "SPDXRef-Package-KairosBoot", "relationshipType": "DEPENDS_ON", "relatedSpdxElement": "SPDXRef-Package-MSVC-Runtime", "comment": "Windows distributions only."}
        ]
    }
    args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
