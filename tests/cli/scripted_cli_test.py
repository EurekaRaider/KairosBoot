# SPDX-License-Identifier: MIT
"""End-to-end CLI checks against a minimal Fastboot-over-TCP device."""

from __future__ import annotations

import argparse
import base64
import json
import pathlib
import socket
import struct
import subprocess
import tempfile
from collections.abc import Callable, Sequence
from typing import Optional


def receive_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise AssertionError("CLI closed the TCP connection early")
        result.extend(chunk)
    return bytes(result)


def receive_frame(connection: socket.socket) -> bytes:
    (length,) = struct.unpack(">Q", receive_exact(connection, 8))
    if length > 4 * 1024 * 1024:
        raise AssertionError(f"unexpectedly large CLI frame: {length}")
    return receive_exact(connection, length)


def send_frame(connection: socket.socket, payload: bytes) -> None:
    connection.sendall(struct.pack(">Q", len(payload)) + payload)


def handshake(connection: socket.socket) -> None:
    hello = receive_exact(connection, 4)
    if hello != b"FB01":
        raise AssertionError(f"unexpected TCP handshake: {hello!r}")
    connection.sendall(hello)


def invoke(
    cli: pathlib.Path,
    arguments: Sequence[str],
    device: Callable[[socket.socket], None],
    expected_exit: int = 0,
) -> tuple[bytes, bytes]:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        port = listener.getsockname()[1]
        command = [
            str(cli),
            "--device",
            f"tcp:127.0.0.1:{port}",
            "--timeout-ms",
            "5000",
            *arguments,
        ]
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        device_error: Optional[BaseException] = None
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(10)
                handshake(connection)
                device(connection)
        except BaseException as error:  # Report the scripted-device cause first.
            device_error = error

        try:
            stdout, stderr = process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            raise AssertionError(
                f"CLI timed out: stdout={stdout!r}, stderr={stderr!r}"
            )

        if device_error is not None:
            raise device_error
        if process.returncode != expected_exit:
            raise AssertionError(
                f"CLI exit {process.returncode}, expected {expected_exit}: "
                f"stdout={stdout!r}, stderr={stderr!r}"
            )
        return stdout, stderr


def parse_success_json(stdout: bytes, stderr: bytes) -> dict[str, object]:
    if stderr:
        raise AssertionError(f"JSON command wrote stderr: {stderr!r}")
    if stdout.count(b"\n") != 1 or not stdout.endswith(b"\n"):
        raise AssertionError(f"JSON output is not exactly one line: {stdout!r}")
    decoded = json.loads(stdout)
    if decoded.get("ok") is not True:
        raise AssertionError(f"JSON command did not succeed: {decoded!r}")
    return decoded


def assert_no_temporary_outputs(directory: pathlib.Path) -> None:
    leftovers = list(directory.glob(".kairosboot-*.tmp"))
    if leftovers:
        raise AssertionError(f"temporary outputs were not cleaned: {leftovers!r}")


def run(cli: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="kairosboot-cli-") as raw_directory:
        directory = pathlib.Path(raw_directory)

        def binary_getvar(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:binary"
            send_frame(connection, b"INFOi\x00\xff")
            send_frame(connection, b"TEXTt\x00\xff")
            send_frame(connection, b"OKAYv\x00\xff")

        stdout, stderr = invoke(
            cli, ["--json", "getvar", "binary"], binary_getvar
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "getvar"
        assert document["terminal"] == {
            "base64": base64.b64encode(b"v\x00\xff").decode("ascii"),
            "bytes": 3,
        }
        assert document["messages"] == [
            {
                "kind": "INFO",
                "base64": base64.b64encode(b"i\x00\xff").decode("ascii"),
                "bytes": 3,
            },
            {
                "kind": "TEXT",
                "base64": base64.b64encode(b"t\x00\xff").decode("ascii"),
                "bytes": 3,
            },
        ]

        stdout, stderr = invoke(cli, ["getvar", "binary"], binary_getvar)
        if stderr or b"\x00" in stdout or b"\xff" in stdout:
            raise AssertionError(
                f"human output exposed raw binary: stdout={stdout!r}, stderr={stderr!r}"
            )
        if b"i\\x00\\xff" not in stdout or b"v\\x00\\xff" not in stdout:
            raise AssertionError(f"human output did not escape binary: {stdout!r}")

        stage_file = directory / "stage.bin"
        stage_payload = bytes(range(16))
        stage_file.write_bytes(stage_payload)

        def staged_download(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"download:00000010"
            send_frame(connection, b"DATA00000010")
            assert receive_frame(connection) == stage_payload
            send_frame(connection, b"OKAYstaged")

        stdout, stderr = invoke(
            cli, ["--json", "stage", str(stage_file)], staged_download
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "stage"
        assert document["terminal"]["base64"] == base64.b64encode(
            b"staged"
        ).decode("ascii")

        upload_file = directory / "upload.bin"
        upload_payload = b"d\x00\xff"
        upload_file.write_bytes(b"previous-complete-output")

        def uploaded_data(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"upload"
            send_frame(connection, b"DATA00000003")
            send_frame(connection, upload_payload)
            send_frame(connection, b"OKAYuploaded")

        stdout, stderr = invoke(
            cli, ["--json", "upload", str(upload_file)], uploaded_data
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "upload"
        assert document["dataBytes"] == 3
        assert document["output"] == str(upload_file)
        assert "data" not in document
        assert upload_file.read_bytes() == upload_payload
        assert_no_temporary_outputs(directory)

        fetch_file = directory / "fetch.bin"
        fetch_payload = b"f\x00\xff"

        def fetched_data(connection: socket.socket) -> None:
            assert (
                receive_frame(connection)
                == b"fetch:vendor:0x00000002:0x00000003"
            )
            send_frame(connection, b"INFOfetching")
            send_frame(connection, b"DATA00000003")
            send_frame(connection, fetch_payload)
            send_frame(connection, b"OKAYfetched")

        stdout, stderr = invoke(
            cli,
            [
                "--json",
                "fetch",
                "vendor",
                str(fetch_file),
                "--offset",
                "2",
                "--size",
                "3",
            ],
            fetched_data,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "fetch"
        assert document["dataBytes"] == 3
        assert document["output"] == str(fetch_file)
        assert "data" not in document
        assert fetch_file.read_bytes() == fetch_payload
        assert_no_temporary_outputs(directory)

        failed_output = directory / "failed-upload.bin"
        failed_output.write_bytes(b"previous-complete-output")

        def interrupted_upload(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"upload"
            send_frame(connection, b"DATA00000003")
            send_frame(connection, b"bad")
            # Closing without a terminal response makes the operation fail.

        stdout, stderr = invoke(
            cli,
            ["--json", "upload", str(failed_output)],
            interrupted_upload,
            expected_exit=4,
        )
        failure = json.loads(stdout)
        assert failure["ok"] is False
        assert stderr == b""
        assert failed_output.read_bytes() == b"previous-complete-output"
        assert_no_temporary_outputs(directory)

        def binary_failure(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"erase:userdata"
            send_frame(connection, b"INFOw\x00\xff")
            send_frame(connection, b"FAILe\x00\xff")

        stdout, stderr = invoke(
            cli,
            ["--json", "erase", "userdata"],
            binary_failure,
            expected_exit=4,
        )
        failure = json.loads(stdout)
        assert stderr == b""
        assert failure["status"] == "device_fail"
        assert failure["deviceMessage"] == {
            "base64": base64.b64encode(b"e\x00\xff").decode("ascii"),
            "bytes": 3,
        }
        assert failure["messages"] == [
            {
                "kind": "INFO",
                "base64": base64.b64encode(b"w\x00\xff").decode("ascii"),
                "bytes": 3,
            }
        ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    run(arguments.cli.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
