#!/usr/bin/env python3
"""Offline unit tests for the locked libusb dependency preparer."""

from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "prepare_libusb", ROOT / "scripts" / "prepare_libusb.py"
)
assert SPEC is not None and SPEC.loader is not None
PREPARER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARER)


class PrepareLibusbTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="kairosboot-libusb-unit-")
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_sha256_mismatch_is_rejected(self) -> None:
        payload = self.root / "payload"
        payload.write_bytes(b"payload")
        with self.assertRaises(SystemExit):
            PREPARER.verify_sha256(payload, "0" * 64)

    def test_locked_header_api_is_required(self) -> None:
        header = self.root / "libusb.h"
        header.write_text(
            "#define LIBUSB_API_VERSION 0x0100010C\n", encoding="utf-8"
        )
        PREPARER.validate_header(header)
        header.write_text(
            "#define LIBUSB_API_VERSION 0x0100010B\n", encoding="utf-8"
        )
        with self.assertRaises(SystemExit):
            PREPARER.validate_header(header)

    def test_pe_architecture_is_checked(self) -> None:
        image = self.root / "libusb.dll"
        data = bytearray(256)
        data[0:2] = b"MZ"
        data[0x3C:0x40] = struct.pack("<I", 128)
        data[128:132] = b"PE\0\0"
        data[132:134] = struct.pack("<H", 0xAA64)
        image.write_bytes(data)
        PREPARER.validate_pe_machine(image, "arm64")
        with self.assertRaises(SystemExit):
            PREPARER.validate_pe_machine(image, "x64")

    def test_non_empty_prefix_is_never_overwritten(self) -> None:
        prefix = self.root / "prefix"
        prefix.mkdir()
        (prefix / "owned-by-user").write_text("keep", encoding="utf-8")
        with self.assertRaises(SystemExit):
            PREPARER.ensure_empty_prefix(prefix)
        self.assertEqual((prefix / "owned-by-user").read_text(encoding="utf-8"), "keep")

    def test_unix_dependency_build_is_release_optimized_with_split_symbols(self) -> None:
        self.assertEqual(PREPARER.UNIX_RELEASE_CFLAGS, "-O3 -DNDEBUG -g")


if __name__ == "__main__":
    unittest.main()
