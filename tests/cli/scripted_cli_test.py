# SPDX-License-Identifier: MIT
"""End-to-end CLI checks against a minimal Fastboot-over-TCP device."""

from __future__ import annotations

import argparse
import base64
import json
import os
import pathlib
import signal
import socket
import struct
import subprocess
import tempfile
import time
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
    timeout_ms: int = 5000,
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
            str(timeout_ms),
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


def invoke_without_connection(
    cli: pathlib.Path,
    arguments: Sequence[str],
    expected_exit: int,
    expected_status: str,
) -> dict[str, object]:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        completed = subprocess.run(
            [
                str(cli),
                "--device",
                f"tcp:127.0.0.1:{port}",
                "--json",
                *arguments,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=10,
            check=False,
        )

        listener.settimeout(0.2)
        try:
            connection, _ = listener.accept()
        except (TimeoutError, socket.timeout):
            connection = None
        if connection is not None:
            connection.close()
            raise AssertionError(
                f"invalid CLI arguments opened a transport: {arguments!r}"
            )

    if completed.returncode != expected_exit or completed.stderr:
        raise AssertionError(
            f"unexpected local rejection for {arguments!r}: "
            f"exit={completed.returncode}, stdout={completed.stdout!r}, "
            f"stderr={completed.stderr!r}"
        )
    if completed.stdout.count(b"\n") != 1 or not completed.stdout.endswith(b"\n"):
        raise AssertionError(
            f"local rejection was not one-line JSON: {completed.stdout!r}"
        )
    document = json.loads(completed.stdout)
    if document.get("ok") is not False or document.get("status") != expected_status:
        raise AssertionError(f"unexpected local rejection JSON: {document!r}")
    return document


def require_peer_close(connection: socket.socket) -> None:
    try:
        remaining = connection.recv(1)
    except socket.timeout as error:
        raise AssertionError("CLI did not close the cancelled transport") from error
    except OSError:
        return
    if remaining:
        raise AssertionError(f"CLI sent bytes after cancellation: {remaining!r}")


def invoke_cancelled(
    cli: pathlib.Path,
    arguments: Sequence[str] = ("snapshot-update", "merge"),
    expected_command: bytes = b"snapshot-update:merge",
) -> tuple[bytes, bytes]:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        port = listener.getsockname()[1]
        creation_flags = (
            subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        )
        process = subprocess.Popen(
            [
                str(cli),
                "--device",
                f"tcp:127.0.0.1:{port}",
                "--timeout-ms",
                "5000",
                "--json",
                *arguments,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=creation_flags,
        )
        connection, _ = listener.accept()
        with connection:
            connection.settimeout(10)
            handshake(connection)
            assert receive_frame(connection) == expected_command
            if os.name == "nt":
                process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                process.send_signal(signal.SIGINT)
            require_peer_close(connection)

        try:
            stdout, stderr = process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            raise AssertionError(
                f"cancelled CLI timed out: stdout={stdout!r}, stderr={stderr!r}"
            )
        if process.returncode != 4:
            raise AssertionError(
                f"cancelled CLI exit {process.returncode}: "
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


def parse_failure_json(
    stdout: bytes, stderr: bytes, expected_status: str
) -> dict[str, object]:
    if stderr:
        raise AssertionError(f"JSON failure wrote stderr: {stderr!r}")
    if stdout.count(b"\n") != 1 or not stdout.endswith(b"\n"):
        raise AssertionError(f"JSON failure is not exactly one line: {stdout!r}")
    decoded = json.loads(stdout)
    if decoded.get("ok") is not False or decoded.get("status") != expected_status:
        raise AssertionError(f"unexpected JSON failure: {decoded!r}")
    return decoded


def assert_no_temporary_outputs(directory: pathlib.Path) -> None:
    leftovers = list(directory.glob(".kairosboot-*.tmp"))
    if leftovers:
        raise AssertionError(f"temporary outputs were not cleaned: {leftovers!r}")


def make_update_package(
    directory: pathlib.Path, name: str, fastboot_info: str
) -> pathlib.Path:
    package = directory / name
    package.mkdir()
    (package / "android-info.txt").write_bytes(b"")
    (package / "fastboot-info.txt").write_bytes(fastboot_info.encode("utf-8"))
    return package


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

        management_commands = [
            (["flashing", "lock"], b"flashing lock"),
            (["flashing", "unlock"], b"flashing unlock"),
            (["flashing", "lock-critical"], b"flashing lock_critical"),
            (["flashing", "unlock-critical"], b"flashing unlock_critical"),
            (
                ["flashing", "get-unlock-ability"],
                b"flashing get_unlock_ability",
            ),
            (["gsi", "wipe"], b"gsi:wipe"),
            (["gsi", "disable"], b"gsi:disable"),
            (["gsi", "status"], b"gsi:status"),
            (["snapshot-update", "cancel"], b"snapshot-update:cancel"),
            (["snapshot-update", "merge"], b"snapshot-update:merge"),
            (
                ["create-logical-partition", "system_ext", "0"],
                b"create-logical-partition:system_ext:0",
            ),
            (
                ["delete-logical-partition", "system_ext"],
                b"delete-logical-partition:system_ext",
            ),
            (
                [
                    "resize-logical-partition",
                    "system_ext",
                    "18446744073709551615",
                ],
                b"resize-logical-partition:system_ext:18446744073709551615",
            ),
        ]
        for command_index, (arguments, wire_command) in enumerate(
            management_commands
        ):

            def management_success(
                connection: socket.socket,
                expected: bytes = wire_command,
                binary_result: bool = command_index == 0,
            ) -> None:
                assert receive_frame(connection) == expected
                if binary_result:
                    send_frame(connection, b"INFOi\x00\xff")
                    send_frame(connection, b"TEXTt\x00\xfe")
                    send_frame(connection, b"OKAYm\x00\xfd")
                else:
                    send_frame(connection, b"OKAYdone")

            stdout, stderr = invoke(
                cli, ["--json", *arguments], management_success
            )
            document = parse_success_json(stdout, stderr)
            assert document["command"] == arguments[0]
            if command_index == 0:
                assert document["terminal"] == {
                    "base64": base64.b64encode(b"m\x00\xfd").decode("ascii"),
                    "bytes": 3,
                }
                assert document["messages"] == [
                    {
                        "kind": "INFO",
                        "base64": base64.b64encode(b"i\x00\xff").decode(
                            "ascii"
                        ),
                        "bytes": 3,
                    },
                    {
                        "kind": "TEXT",
                        "base64": base64.b64encode(b"t\x00\xfe").decode(
                            "ascii"
                        ),
                        "bytes": 3,
                    },
                ]

        def management_failure(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"gsi:status"
            send_frame(connection, b"INFOw\x00\xfc")
            send_frame(connection, b"TEXTh\x00\xfb")
            send_frame(connection, b"FAILe\x00\xfa")

        stdout, stderr = invoke(
            cli,
            ["--json", "gsi", "status"],
            management_failure,
            expected_exit=4,
        )
        failure = parse_failure_json(stdout, stderr, "device_fail")
        assert failure["deviceMessage"] == {
            "base64": base64.b64encode(b"e\x00\xfa").decode("ascii"),
            "bytes": 3,
        }
        assert failure["messages"] == [
            {
                "kind": "INFO",
                "base64": base64.b64encode(b"w\x00\xfc").decode("ascii"),
                "bytes": 3,
            },
            {
                "kind": "TEXT",
                "base64": base64.b64encode(b"h\x00\xfb").decode("ascii"),
                "bytes": 3,
            },
        ]

        def management_timeout(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"snapshot-update:cancel"
            require_peer_close(connection)

        stdout, stderr = invoke(
            cli,
            ["--json", "snapshot-update", "cancel"],
            management_timeout,
            expected_exit=4,
            timeout_ms=20,
        )
        parse_failure_json(stdout, stderr, "timeout")

        stdout, stderr = invoke_cancelled(cli)
        cancellation_failure = parse_failure_json(stdout, stderr, "cancelled")
        assert cancellation_failure["sessionPoisoned"] is True

        update_package = make_update_package(
            directory, "更新 包", "version 1\nerase cache\n"
        )

        def update_success(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"erase:cache"
            send_frame(connection, b"OKAYerased")

        stdout, stderr = invoke(
            cli, ["--json", "update", str(update_package)], update_success
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "update",
            "package": str(update_package),
            "wipe": False,
        }

        stdout, stderr = invoke(
            cli, ["update", str(update_package)], update_success
        )
        if (
            b"Updated from " not in stdout
            or str(update_package).encode() not in stdout
        ):
            raise AssertionError(f"unexpected text update output: {stdout!r}")
        if (
            b"update: preflight" not in stderr
            or b"update: complete" not in stderr
        ):
            raise AssertionError(f"text update did not report progress: {stderr!r}")

        wipe_package = make_update_package(
            directory, "wipe-package", "version 1\nif-wipe erase userdata\n"
        )

        def wipe_success(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"erase:userdata"
            send_frame(connection, b"OKAYwiped")

        stdout, stderr = invoke(
            cli,
            ["--json", "update", str(wipe_package), "--wipe"],
            wipe_success,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "update"
        assert document["package"] == str(wipe_package)
        assert document["wipe"] is True

        failed_update_package = make_update_package(
            directory, "failed-update", "version 1\nerase metadata\n"
        )

        def update_failure(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"erase:metadata"
            send_frame(connection, b"FAILpartition locked")

        stdout, stderr = invoke(
            cli,
            ["--json", "update", str(failed_update_package)],
            update_failure,
            expected_exit=4,
        )
        failure = parse_failure_json(stdout, stderr, "device_fail")
        assert failure["deviceMessage"] == {
            "base64": base64.b64encode(b"partition locked").decode("ascii"),
            "bytes": len(b"partition locked"),
        }

        timeout_update_package = make_update_package(
            directory, "timeout-update", "version 1\nerase misc\n"
        )

        def update_timeout(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"erase:misc"
            require_peer_close(connection)

        stdout, stderr = invoke(
            cli,
            ["--json", "update", str(timeout_update_package)],
            update_timeout,
            expected_exit=4,
            timeout_ms=20,
        )
        parse_failure_json(stdout, stderr, "timeout")

        cancelled_update_package = make_update_package(
            directory, "cancelled-update", "version 1\nerase system\n"
        )
        stdout, stderr = invoke_cancelled(
            cli,
            ("update", str(cancelled_update_package)),
            b"erase:system",
        )
        cancelled = parse_failure_json(stdout, stderr, "cancelled")
        assert cancelled["sessionPoisoned"] is True

        cumulative_timeout_package = make_update_package(
            directory,
            "cumulative-timeout-update",
            "version 1\nerase cache\nerase metadata\nerase misc\n",
        )

        def cumulative_update_timeout(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"erase:cache"
            time.sleep(1.5)
            send_frame(connection, b"OKAYcache")
            assert receive_frame(connection) == b"erase:metadata"
            time.sleep(1.5)
            send_frame(connection, b"OKAYmetadata")
            assert receive_frame(connection) == b"erase:misc"
            time.sleep(1.5)
            try:
                send_frame(connection, b"OKAYmisc")
            except OSError:
                # The shared whole-operation deadline normally closes the
                # transport before this third, individually timely response.
                pass

        stdout, stderr = invoke(
            cli,
            ["--json", "update", str(cumulative_timeout_package)],
            cumulative_update_timeout,
            expected_exit=4,
            timeout_ms=4000,
        )
        parse_failure_json(stdout, stderr, "timeout")

        invoke_without_connection(
            cli, ["flashing", "sideways"], 2, "invalid_argument"
        )
        invoke_without_connection(
            cli,
            [
                "create-logical-partition",
                "system_ext",
                "18446744073709551616",
            ],
            2,
            "invalid_argument",
        )
        invoke_without_connection(
            cli,
            ["delete-logical-partition", "bad:name"],
            4,
            "invalid_argument",
        )

        stage_file = directory / "阶段-镜像.bin"
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

        upload_file = directory / "上传-结果.bin"
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

        fetch_file = directory / "分区-结果.bin"
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
