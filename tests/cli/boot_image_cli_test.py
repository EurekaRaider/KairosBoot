# SPDX-License-Identifier: MIT
"""Scripted CLI contract for bounded legacy Android boot image construction."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import tempfile


def pad(payload: bytes, page_size: int) -> bytes:
    return payload + bytes((-len(payload)) % page_size)


def expected_image(
    kernel: bytes,
    ramdisk: bytes,
    second: bytes,
    cmdline: bytes,
    base: int,
    page_size: int,
    kernel_offset: int,
    ramdisk_offset: int,
    second_offset: int,
    tags_offset: int,
) -> bytes:
    header = bytearray(page_size)
    header[:8] = b"ANDROID!"
    fields = (
        len(kernel),
        base + kernel_offset,
        len(ramdisk),
        base + ramdisk_offset,
        len(second),
        base + second_offset,
        base + tags_offset,
        page_size,
        0,
        0,
    )
    struct.pack_into("<10I", header, 8, *fields)
    header[64 : 64 + min(511, len(cmdline))] = cmdline[:511]
    header[608 : 608 + max(0, len(cmdline) - 511)] = cmdline[511:]
    struct.pack_into("<Q", header, 1652, 0x01100000)
    return bytes(header) + pad(kernel, page_size) + pad(ramdisk, page_size) + pad(
        second, page_size
    )


def run(cli: pathlib.Path, arguments: list[str], expected_exit: int) -> dict[str, object]:
    completed = subprocess.run(
        [str(cli), "--json", *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=20,
        check=False,
    )
    if completed.returncode != expected_exit or completed.stderr:
        raise AssertionError(
            f"unexpected CLI result: exit={completed.returncode}, "
            f"stdout={completed.stdout!r}, stderr={completed.stderr!r}"
        )
    if completed.stdout.count(b"\n") != 1 or not completed.stdout.endswith(b"\n"):
        raise AssertionError(f"CLI JSON is not exactly one line: {completed.stdout!r}")
    return json.loads(completed.stdout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=pathlib.Path, required=True)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="kairosboot-boot-image-") as temporary:
        directory = pathlib.Path(temporary)
        kernel = b"kernel-payload\x00\xff"
        ramdisk = b"ramdisk-payload"
        second = b"second-stage"
        cmdline = b"console=ttyS0 androidboot.hardware=kairos"
        kernel_path = directory / "kernel"
        ramdisk_path = directory / "ramdisk"
        second_path = directory / "second"
        output = directory / "boot.img"
        kernel_path.write_bytes(kernel)
        ramdisk_path.write_bytes(ramdisk)
        second_path.write_bytes(second)

        document = run(
            arguments.cli,
            [
                "make-boot-image",
                str(output),
                str(kernel_path),
                str(ramdisk_path),
                str(second_path),
                "--cmdline",
                cmdline.decode("ascii"),
                "--base",
                "0x20000000",
                "--page-size",
                "4096",
                "--kernel-offset",
                "0x1000",
                "--ramdisk-offset",
                "0x2000",
                "--second-offset",
                "0x3000",
                "--tags-offset",
                "0x4000",
            ],
            0,
        )
        actual = output.read_bytes()
        expected = expected_image(
            kernel,
            ramdisk,
            second,
            cmdline,
            0x20000000,
            4096,
            0x1000,
            0x2000,
            0x3000,
            0x4000,
        )
        if actual != expected:
            raise AssertionError("constructed image does not match canonical v0 bytes")
        digest = hashlib.sha256(actual).hexdigest()
        if digest != "14cf666e232e204d51de0060ff41141c582527fe798807d6166c9e6c4a0140cb":
            raise AssertionError(f"unexpected boot image SHA-256: {digest}")
        if document != {
            "ok": True,
            "command": "make-boot-image",
            "output": str(output),
            "bytes": len(actual),
            "headerVersion": 0,
            "vendorBoot": False,
        }:
            raise AssertionError(f"unexpected success document: {document!r}")

        modern_output = directory / "boot-v3.img"
        modern_document = run(
            arguments.cli,
            [
                "make-boot-image",
                str(modern_output),
                str(kernel_path),
                str(ramdisk_path),
                "--header-version",
                "3",
                "--os-version",
                "14.1.2",
                "--os-patch-level",
                "2024-12-05",
                "--cmdline",
                "console=modern",
            ],
            0,
        )
        modern = modern_output.read_bytes()
        if len(modern) != 12288:
            raise AssertionError(f"unexpected v3 image size: {len(modern)}")
        if struct.unpack_from("<I", modern, 8)[0] != len(kernel):
            raise AssertionError("v3 kernel size was not encoded")
        if struct.unpack_from("<I", modern, 12)[0] != len(ramdisk):
            raise AssertionError("v3 ramdisk size was not encoded")
        expected_os = (14 << 25) | (1 << 18) | (2 << 11) | (24 << 4) | 12
        if struct.unpack_from("<I", modern, 16)[0] != expected_os:
            raise AssertionError("v3 OS version/patch field was not encoded")
        if struct.unpack_from("<II", modern, 20) != (1580, 0):
            raise AssertionError("v3 header size/reserved fields are invalid")
        if struct.unpack_from("<I", modern, 40)[0] != 3:
            raise AssertionError("v3 header version was not encoded")
        if modern[44 : 44 + len(b"console=modern")] != b"console=modern":
            raise AssertionError("v3 command line was not encoded")
        if modern[4096 : 4096 + len(kernel)] != kernel:
            raise AssertionError("v3 kernel layout is invalid")
        if modern[8192 : 8192 + len(ramdisk)] != ramdisk:
            raise AssertionError("v3 ramdisk layout is invalid")
        if modern_document["headerVersion"] != 3:
            raise AssertionError(f"unexpected v3 document: {modern_document!r}")

        rejected = directory / "rejected.img"
        failure = run(
            arguments.cli,
            [
                "make-boot-image",
                str(rejected),
                str(kernel_path),
                "--page-size",
                "1024",
            ],
            4,
        )
        if failure.get("ok") is not False or failure.get("status") != "invalid_argument":
            raise AssertionError(f"unexpected boundary failure: {failure!r}")
        if rejected.exists() or list(directory.glob(".kairosboot-*.tmp")):
            raise AssertionError("failed construction published an output")

    print("PASS: boot image CLI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
