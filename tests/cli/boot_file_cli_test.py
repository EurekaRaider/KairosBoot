# SPDX-License-Identifier: MIT
"""Fastboot-over-TCP contract for payload boot success, failure and cancel."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import tempfile
from collections.abc import Callable


def receive_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise AssertionError("CLI closed the TCP connection early")
        result.extend(chunk)
    return bytes(result)


def receive_frame(connection: socket.socket) -> bytes:
    (size,) = struct.unpack(">Q", receive_exact(connection, 8))
    if size > 4 * 1024 * 1024:
        raise AssertionError(f"unexpected frame size: {size}")
    return receive_exact(connection, size)


def send_frame(connection: socket.socket, payload: bytes) -> None:
    connection.sendall(struct.pack(">Q", len(payload)) + payload)


def invoke(
    cli: pathlib.Path,
    image: pathlib.Path,
    device: Callable[[socket.socket, subprocess.Popen[bytes]], None],
    expected_exit: int,
    command: list[str] | None = None,
) -> dict[str, object]:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        port = listener.getsockname()[1]
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        arguments = [
            str(cli),
            "-s",
            f"tcp:127.0.0.1:{port}",
        ]
        arguments.extend(command if command is not None else ["boot", str(image)])
        process = subprocess.Popen(
            arguments,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=creation_flags,
            env={
                **os.environ,
                "KAIROSBOOT_INTERNAL_TEST_JSON": "1",
                "KAIROSBOOT_INTERNAL_TEST_TIMEOUT_MS": "5000",
            },
        )
        connection, _ = listener.accept()
        with connection:
            connection.settimeout(10)
            if receive_exact(connection, 4) != b"FB01":
                raise AssertionError("unexpected Fastboot TCP handshake")
            connection.sendall(b"FB01")
            device(connection, process)
        stdout, stderr = process.communicate(timeout=15)

    if process.returncode != expected_exit or stderr:
        raise AssertionError(
            f"unexpected CLI result: exit={process.returncode}, "
            f"stdout={stdout!r}, stderr={stderr!r}"
        )
    if stdout.count(b"\n") != 1 or not stdout.endswith(b"\n"):
        raise AssertionError(f"CLI JSON is not exactly one line: {stdout!r}")
    return json.loads(stdout)


def expect_download(connection: socket.socket, image: bytes) -> None:
    expected = f"download:{len(image):08x}".encode("ascii")
    if receive_frame(connection) != expected:
        raise AssertionError("unexpected download command")
    send_frame(connection, b"DATA" + f"{len(image):08x}".encode("ascii"))
    if receive_frame(connection) != image:
        raise AssertionError("download payload differs from boot image")
    send_frame(connection, b"OKAYdownloaded")


def legacy_boot_image(
    kernel: bytes, ramdisk: bytes, second: bytes, command_line: bytes
) -> bytes:
    page_size = 4096
    header = bytearray(page_size)
    header[:8] = b"ANDROID!"
    struct.pack_into(
        "<10I",
        header,
        8,
        len(kernel),
        0x20001000,
        len(ramdisk),
        0x20002000,
        len(second),
        0x20F00000,
        0x20004000,
        page_size,
        0,
        0,
    )
    header[64 : 64 + len(command_line)] = command_line
    struct.pack_into("<Q", header, 1652, 0x01100000)

    def padded(payload: bytes) -> bytes:
        return payload + bytes((-len(payload)) % page_size)

    return bytes(header) + padded(kernel) + padded(ramdisk) + padded(second)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kairosboot-boot-file-") as temporary:
        image = b"ANDROID!" + bytes(2040)
        image_path = pathlib.Path(temporary) / "boot.img"
        image_path.write_bytes(image)

        def success(connection: socket.socket, _: subprocess.Popen[bytes]) -> None:
            expect_download(connection, image)
            if receive_frame(connection) != b"boot":
                raise AssertionError("boot command did not follow download")
            send_frame(connection, b"OKAYbooting")

        succeeded = invoke(arguments.cli, image_path, success, 0)
        if succeeded != {
            "ok": True,
            "command": "boot",
            "image": str(image_path),
        }:
            raise AssertionError(f"unexpected success JSON: {succeeded!r}")

        kernel = bytes(index & 0xFF for index in range(2000))
        ramdisk = b"ramdisk"
        second = b"second"
        command_line = b"console=ttyS0"
        kernel_path = pathlib.Path(temporary) / "kernel"
        ramdisk_path = pathlib.Path(temporary) / "ramdisk"
        second_path = pathlib.Path(temporary) / "second"
        kernel_path.write_bytes(kernel)
        ramdisk_path.write_bytes(ramdisk)
        second_path.write_bytes(second)
        raw_image = legacy_boot_image(kernel, ramdisk, second, command_line)

        def raw_success(connection: socket.socket, _: subprocess.Popen[bytes]) -> None:
            expect_download(connection, raw_image)
            if receive_frame(connection) != b"boot":
                raise AssertionError("boot command did not follow constructed image")
            send_frame(connection, b"OKAYbooting")

        raw_result = invoke(
            arguments.cli,
            kernel_path,
            raw_success,
            0,
            [
                "--cmdline",
                command_line.decode("ascii"),
                "--base",
                "0x20000000",
                "--page-size",
                "4096",
                "--kernel-offset",
                "0x1000",
                "--ramdisk-offset",
                "0x2000",
                "--tags-offset",
                "0x4000",
                "boot",
                str(kernel_path),
                str(ramdisk_path),
                str(second_path),
            ],
        )
        if raw_result != {
            "ok": True,
            "command": "boot",
            "image": str(kernel_path),
        }:
            raise AssertionError(f"unexpected raw boot JSON: {raw_result!r}")

        def rejected(connection: socket.socket, _: subprocess.Popen[bytes]) -> None:
            expect_download(connection, image)
            if receive_frame(connection) != b"boot":
                raise AssertionError("boot command did not follow download")
            send_frame(connection, b"INFOboot policy")
            send_frame(connection, b"FAILboot denied")

        failed = invoke(arguments.cli, image_path, rejected, 4)
        if (
            failed.get("ok") is not False
            or failed.get("status") != "device_fail"
            or failed.get("transferState") != "fully_transferred"
            or failed.get("deviceMessage", {}).get("base64") != "Ym9vdCBkZW5pZWQ="
        ):
            raise AssertionError(f"unexpected boot failure JSON: {failed!r}")

        def cancelled(
            connection: socket.socket, process: subprocess.Popen[bytes]
        ) -> None:
            expected = f"download:{len(image):08x}".encode("ascii")
            if receive_frame(connection) != expected:
                raise AssertionError("unexpected cancellation command")
            if os.name == "nt":
                process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                process.send_signal(signal.SIGINT)
            try:
                remaining = connection.recv(1)
            except OSError:
                return
            if remaining:
                raise AssertionError(f"CLI sent data after cancellation: {remaining!r}")

        stopped = invoke(arguments.cli, image_path, cancelled, 4)
        if (
            stopped.get("ok") is not False
            or stopped.get("status") != "cancelled"
            or stopped.get("transferState") != "not_sent"
        ):
            raise AssertionError(f"unexpected cancellation JSON: {stopped!r}")

    print("PASS: payload boot CLI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
