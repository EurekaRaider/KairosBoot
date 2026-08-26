#!/usr/bin/env python3
"""Regression tests for complete shared-library export inspection."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "kairosboot_check_abi", ROOT / "scripts" / "check_abi.py"
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load scripts/check_abi.py")
CHECK_ABI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK_ABI)


class AbiCheckTests(unittest.TestCase):
    def test_nm_parser_keeps_every_defined_export(self) -> None:
        output = "kb_get_version\nrogue_export\n_ZN4test3runEv\n"
        self.assertEqual(
            CHECK_ABI.parse_nm_symbols(output, strip_macho_prefix=False),
            {"kb_get_version", "rogue_export", "_ZN4test3runEv"},
        )

    def test_macho_parser_removes_only_the_object_format_prefix(self) -> None:
        output = "_kb_get_version\n_rogue_export\n__ZN4test3runEv\n"
        self.assertEqual(
            CHECK_ABI.parse_nm_symbols(output, strip_macho_prefix=True),
            {"kb_get_version", "rogue_export", "_ZN4test3runEv"},
        )

    def test_dumpbin_parser_keeps_named_and_decorated_exports(self) -> None:
        output = """
ordinal hint RVA      name
      1    0 00001000 kb_get_version
      2    1 00002000 rogue_export
      3    2 00003000 ?cpp_export@@YAHXZ
      4    3 00004000 forwarded_export = KERNEL32.forwarded_export

Summary
        1000 .data
"""
        self.assertEqual(
            CHECK_ABI.parse_dumpbin_symbols(output),
            {
                "kb_get_version",
                "rogue_export",
                "?cpp_export@@YAHXZ",
                "forwarded_export",
            },
        )

    def test_link_exe_uses_dump_mode_for_pe_exports(self) -> None:
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="ordinal hint RVA      name\n", stderr=""
        )
        with mock.patch.object(
            CHECK_ABI.subprocess, "run", return_value=completed
        ) as invoked:
            CHECK_ABI.run_symbols(
                Path("kairosboot.dll"),
                "windows",
                Path("C:/toolchain/link.exe"),
            )
        self.assertEqual(
            invoked.call_args.args[0],
            [
                "C:/toolchain/link.exe",
                "/dump",
                "/nologo",
                "/exports",
                "kairosboot.dll",
            ],
        )

    def test_unexpected_rogue_export_is_rejected(self) -> None:
        manifest = CHECK_ABI.manifest_symbols()
        exported = manifest | {"rogue_export"}
        stderr = io.StringIO()
        with mock.patch.object(CHECK_ABI, "library_symbols", return_value=exported):
            with contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised:
                    CHECK_ABI.check_library_exports(
                        Path("unused-test-library"), manifest, "linux"
                    )
        self.assertEqual(raised.exception.code, 1)
        self.assertIn("extra=['rogue_export']", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
