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


def normalize_cli_arguments(arguments: Sequence[str]) -> tuple[list[str], bool]:
    """Strip the private JSON fixture marker before invoking the public CLI."""
    normalized: list[str] = []
    json_requested = False
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--json":
            json_requested = True
            index += 1
            continue
        normalized.append(argument)
        index += 1
    return normalized, json_requested


def cli_test_environment(
    environment: Optional[dict[str, str]], json_requested: bool, timeout_ms: int
) -> dict[str, str]:
    result = dict(os.environ if environment is None else environment)
    if json_requested:
        result["KAIROSBOOT_INTERNAL_TEST_JSON"] = "1"
    result["KAIROSBOOT_INTERNAL_TEST_TIMEOUT_MS"] = str(timeout_ms)
    return result


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


def make_vendor_boot_v4() -> bytes:
    page = 4096
    entry_size = 112
    table_size = 2 * entry_size
    image = bytearray([0x6C] * (7 * page))
    image[0:8] = b"VNDRBOOT"
    for offset, value in (
        (8, 4),
        (12, page),
        (24, 7),
        (2096, 2128),
        (2100, 3),
        (2112, table_size),
        (2116, 2),
        (2120, entry_size),
        (2124, 4),
    ):
        struct.pack_into("<I", image, offset, value)
    image[page : page + 7] = b"oneTWO2"
    image[page + 7 : 2 * page] = bytes(page - 7)
    image[2 * page : 2 * page + 3] = b"dtb"
    image[2 * page + 3 : 3 * page] = bytes(page - 3)

    table = 3 * page
    struct.pack_into("<III", image, table, 3, 0, 1)
    image[table + 12 : table + 44] = bytes(32)
    image[table + 12 : table + 17] = b"alpha"
    image[table + 108] = 0xD1
    second = table + entry_size
    struct.pack_into("<III", image, second, 4, 3, 3)
    image[second + 12 : second + 44] = bytes(32)
    image[second + 12 : second + 16] = b"beta"
    image[second + 44] = 0xB7
    image[second + 108] = 0xE1
    image[table + table_size : 4 * page] = bytes(page - table_size)
    image[4 * page : 4 * page + 4] = b"boot"
    image[4 * page + 4 : 5 * page] = bytes(page - 4)
    image[6 * page + 11 : 6 * page + 15] = b"TAIL"
    return bytes(image)


def handshake(connection: socket.socket) -> None:
    hello = receive_exact(connection, 4)
    if hello != b"FB01":
        raise AssertionError(f"unexpected TCP handshake: {hello!r}")
    connection.sendall(hello)


def udp_packet(
    packet_id: int, sequence: int, payload: bytes = b"", flags: int = 0
) -> bytes:
    return struct.pack(">BBH", packet_id, flags, sequence) + payload


def serve_udp_flash(
    server: socket.socket, image: bytes, partition: bytes = b"system"
) -> None:
    def receive(
        expected: bytes, peer: tuple[str, int] | None = None
    ) -> tuple[str, int]:
        datagram, observed_peer = server.recvfrom(8192)
        if datagram != expected:
            raise AssertionError(
                f"unexpected Fastboot UDP datagram: {datagram!r}, "
                f"expected {expected!r}"
            )
        if peer is not None and observed_peer != peer:
            raise AssertionError(
                f"Fastboot UDP peer changed: {observed_peer!r} != {peer!r}"
            )
        return observed_peer

    peer = receive(udp_packet(1, 0))
    server.sendto(udp_packet(1, 0, struct.pack(">H", 100)), peer)
    receive(udp_packet(2, 100, struct.pack(">HH", 1, 8192)), peer)
    server.sendto(udp_packet(2, 100, struct.pack(">HH", 1, 512)), peer)

    sequence = 101

    def exchange(request: bytes, response: bytes) -> None:
        nonlocal sequence
        receive(udp_packet(3, sequence, request), peer)
        server.sendto(udp_packet(3, sequence), peer)
        sequence += 1
        receive(udp_packet(3, sequence), peer)
        server.sendto(udp_packet(3, sequence, response), peer)
        sequence += 1

    exchange(b"getvar:is-userspace", b"OKAYno")
    exchange(b"getvar:has-slot:" + partition, b"OKAYno")
    exchange(b"getvar:is-logical:" + partition, b"OKAYno")
    exchange(b"getvar:max-download-size", b"OKAY0x00100000")
    encoded_size = f"{len(image):08x}".encode("ascii")
    exchange(b"download:" + encoded_size, b"DATA" + encoded_size)
    exchange(image, b"OKAYdownloaded")
    exchange(b"flash:" + partition, b"OKAYflashed")


def invoke_udp(
    cli: pathlib.Path,
    arguments: Sequence[str],
    device: Callable[[socket.socket], None],
    expected_exit: int = 0,
    timeout_ms: int = 5000,
) -> tuple[bytes, bytes]:
    normalized, json_requested = normalize_cli_arguments(arguments)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
        server.bind(("127.0.0.1", 0))
        server.settimeout(10)
        port = server.getsockname()[1]
        process = subprocess.Popen(
            [
                str(cli),
                "-s",
                f"udp:127.0.0.1:{port}",
                *normalized,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=cli_test_environment(None, json_requested, timeout_ms),
        )
        device_error: Optional[BaseException] = None
        try:
            device(server)
        except BaseException as error:
            device_error = error

        try:
            stdout, stderr = process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            raise AssertionError(
                f"UDP CLI timed out: stdout={stdout!r}, stderr={stderr!r}"
            )

        if device_error is not None:
            raise device_error
        if process.returncode != expected_exit:
            raise AssertionError(
                f"UDP CLI exit {process.returncode}, expected {expected_exit}: "
                f"stdout={stdout!r}, stderr={stderr!r}"
            )
        return stdout, stderr


def invoke(
    cli: pathlib.Path,
    arguments: Sequence[str],
    device: Callable[[socket.socket], None],
    expected_exit: int = 0,
    timeout_ms: int = 5000,
    environment: Optional[dict[str, str]] = None,
    working_directory: Optional[pathlib.Path] = None,
) -> tuple[bytes, bytes]:
    normalized, json_requested = normalize_cli_arguments(arguments)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        port = listener.getsockname()[1]
        command = [
            str(cli),
            "-s",
            f"tcp:127.0.0.1:{port}",
            *normalized,
        ]
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=cli_test_environment(environment, json_requested, timeout_ms),
            cwd=working_directory,
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
    environment: Optional[dict[str, str]] = None,
) -> dict[str, object]:
    normalized, _ = normalize_cli_arguments(arguments)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        completed = subprocess.run(
            [
                str(cli),
                "-s",
                f"tcp:127.0.0.1:{port}",
                *normalized,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=cli_test_environment(environment, True, 5000),
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


def invoke_flash_without_server(
    cli: pathlib.Path,
    scheme: str,
    image: pathlib.Path,
    timeout_ms: int = 100,
) -> dict[str, object]:
    normalized, _ = normalize_cli_arguments(
        ["flash", "system", str(image)]
    )
    socket_type = socket.SOCK_STREAM if scheme == "tcp" else socket.SOCK_DGRAM
    with socket.socket(socket.AF_INET, socket_type) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    completed = subprocess.run(
        [
            str(cli),
            "-s",
            f"{scheme}:127.0.0.1:{port}",
            *normalized,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=cli_test_environment(None, True, timeout_ms),
        timeout=10,
        check=False,
    )
    if completed.returncode != 4 or completed.stderr:
        raise AssertionError(
            f"unavailable {scheme} flash target produced an unexpected result: "
            f"exit={completed.returncode}, stdout={completed.stdout!r}, "
            f"stderr={completed.stderr!r}"
        )
    if completed.stdout.count(b"\n") != 1 or not completed.stdout.endswith(
        b"\n"
    ):
        raise AssertionError(
            f"unavailable {scheme} failure was not one-line JSON: "
            f"{completed.stdout!r}"
        )
    document = json.loads(completed.stdout)
    if document.get("ok") is not False:
        raise AssertionError(
            f"unavailable {scheme} target did not fail: {document!r}"
        )
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
    normalized, _ = normalize_cli_arguments(arguments)
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
                "-s",
                f"tcp:127.0.0.1:{port}",
                *normalized,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=creation_flags,
            env=cli_test_environment(None, True, 5000),
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
            cli,
            ["--json", "getvar", "binary"],
            binary_getvar,
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
            (["flashing", "lock_critical"], b"flashing lock_critical"),
            (["flashing", "unlock_critical"], b"flashing unlock_critical"),
            (
                ["flashing", "get_unlock_ability"],
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
                resize: bool = arguments[0] == "resize-logical-partition",
            ) -> None:
                if resize:
                    assert receive_frame(connection) == b"getvar:is-userspace"
                    send_frame(connection, b"OKAYyes")
                    assert receive_frame(connection) == b"getvar:has-slot:system_ext"
                    send_frame(connection, b"OKAYno")
                    assert receive_frame(connection) == b"getvar:is-logical:system_ext"
                    send_frame(connection, b"OKAYyes")
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
            cli,
            [
                "--json",
                "update",
                str(update_package),
                "--skip-reboot",
                "--disable-super-optimization",
            ],
            update_success,
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "update",
            "package": str(update_package),
            "wipe": False,
            "skipReboot": True,
            "skipSecondary": False,
            "excludeDynamicPartitions": False,
            "disableFastbootInfo": False,
            "disableSuperOptimization": True,
        }

        stdout, stderr = invoke(
            cli, ["update", str(update_package), "--skip-reboot"], update_success
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

        flashall_environment = os.environ.copy()
        flashall_environment.pop("ANDROID_PRODUCT_OUT", None)
        missing_product_out = invoke_without_connection(
            cli,
            ["flashall"],
            4,
            "invalid_argument",
            flashall_environment,
        )
        assert missing_product_out["message"] == (
            "flashall requires non-empty ANDROID_PRODUCT_OUT"
        )

        flashall_environment["ANDROID_PRODUCT_OUT"] = str(update_package)
        stdout, stderr = invoke(
            cli,
            ["--json", "flashall", "--skip-reboot"],
            update_success,
            environment=flashall_environment,
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "flashall",
            "package": str(update_package),
            "wipe": False,
            "skipReboot": True,
            "skipSecondary": False,
            "excludeDynamicPartitions": False,
            "disableFastbootInfo": False,
            "disableSuperOptimization": False,
        }

        stdout, stderr = invoke(
            cli,
            ["flashall", "--skip-reboot"],
            update_success,
            environment=flashall_environment,
        )
        if (
            b"Flashed all from " not in stdout
            or str(update_package).encode() not in stdout
        ):
            raise AssertionError(f"unexpected text flashall output: {stdout!r}")
        if (
            b"flashall: preflight" not in stderr
            or b"flashall: complete" not in stderr
        ):
            raise AssertionError(
                f"text flashall did not report progress: {stderr!r}"
            )

        wipe_package = make_update_package(
            directory, "wipe-package", "version 1\nif-wipe erase userdata\n"
        )

        def wipe_success(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"erase:userdata"
            send_frame(connection, b"OKAYwiped")

        stdout, stderr = invoke(
            cli,
            ["--json", "update", str(wipe_package), "-w", "--skip-reboot"],
            wipe_success,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "update"
        assert document["package"] == str(wipe_package)
        assert document["wipe"] is True

        flashall_environment["ANDROID_PRODUCT_OUT"] = str(wipe_package)
        stdout, stderr = invoke(
            cli,
            ["--json", "flashall", "-w", "--skip-reboot"],
            wipe_success,
            environment=flashall_environment,
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "flashall",
            "package": str(wipe_package),
            "wipe": True,
            "skipReboot": True,
            "skipSecondary": False,
            "excludeDynamicPartitions": False,
            "disableFastbootInfo": False,
            "disableSuperOptimization": False,
        }

        super_empty_image = directory / "custom-super-empty.img"
        super_empty_payload = b"immutable-empty-super"
        super_empty_image.write_bytes(super_empty_payload)

        def wipe_super_success(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYyes")
            assert receive_frame(connection) == b"getvar:super-partition-name"
            send_frame(connection, b"OKAYsuper_main")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYyes")
            encoded_size = f"{len(super_empty_payload):08x}".encode("ascii")
            assert receive_frame(connection) == b"download:" + encoded_size
            send_frame(connection, b"DATA" + encoded_size)
            assert receive_frame(connection) == super_empty_payload
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"update-super:super_main:wipe"
            send_frame(connection, b"OKAYwiped")

        stdout, stderr = invoke(
            cli,
            ["--json", "wipe-super", str(super_empty_image)],
            wipe_super_success,
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "wipe-super",
            "image": str(super_empty_image),
        }

        stdout, stderr = invoke(
            cli, ["wipe-super", str(super_empty_image)], wipe_super_success
        )
        if b"Wiped super using " not in stdout:
            raise AssertionError(f"unexpected wipe-super output: {stdout!r}")
        if (
            b"wipe-super: preflight" not in stderr
            or b"wipe-super: complete" not in stderr
        ):
            raise AssertionError(
                f"wipe-super did not report progress: {stderr!r}"
            )

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
            timeout_ms=1000,
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

        def flashed_over_tcp(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:system"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:system"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"download:00000010"
            send_frame(connection, b"DATA00000010")
            assert receive_frame(connection) == stage_payload
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:system"
            send_frame(connection, b"OKAYflashed")

        stdout, stderr = invoke(
            cli,
            ["--json", "flash", "system", str(stage_file)],
            flashed_over_tcp,
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "flash",
            "partition": "system",
            "file": str(stage_file),
        }

        default_product_out = directory / "default-product-out"
        default_product_out.mkdir()
        default_boot = default_product_out / "boot.img"
        default_boot_payload = bytes([0xA5]) * 16
        default_boot.write_bytes(default_boot_payload)
        ambiguous_working_directory = directory / "ambiguous-working-directory"
        ambiguous_working_directory.mkdir()
        (ambiguous_working_directory / "boot.img").write_bytes(bytes([0x5A]) * 16)
        default_environment = os.environ.copy()
        default_environment["ANDROID_PRODUCT_OUT"] = str(default_product_out)

        def flashed_default_boot(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:boot"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:boot"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"download:00000010"
            send_frame(connection, b"DATA00000010")
            assert receive_frame(connection) == default_boot_payload
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:boot"
            send_frame(connection, b"OKAYflashed")

        stdout, stderr = invoke(
            cli,
            ["--json", "flash", "boot"],
            flashed_default_boot,
            environment=default_environment,
            working_directory=ambiguous_working_directory,
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "flash",
            "partition": "boot",
            "file": str(default_boot),
        }

        # Platform-Tools classifies these images as Extra and deliberately
        # excludes them from flashall. Exercise their independent default-file
        # flash path so every frozen image-table entry still has direct wire
        # execution coverage.
        extra_default_images = {
            "bootloader": "bootloader.img",
            "cache": "cache.img",
            "radio": "radio.img",
            "super": "super.img",
            "userdata": "userdata.img",
        }
        for index, (partition, filename) in enumerate(
            extra_default_images.items(), start=1
        ):
            payload = bytes([0xB0 + index]) * 16
            image = default_product_out / filename
            image.write_bytes(payload)

            def flashed_default_extra(
                connection: socket.socket,
                expected_partition: str = partition,
                expected_payload: bytes = payload,
            ) -> None:
                encoded_partition = expected_partition.encode("ascii")
                assert receive_frame(connection) == b"getvar:is-userspace"
                send_frame(connection, b"OKAYno")
                assert receive_frame(connection) == b"getvar:has-slot:" + encoded_partition
                send_frame(connection, b"OKAYno")
                assert receive_frame(connection) == b"getvar:is-logical:" + encoded_partition
                send_frame(connection, b"OKAYno")
                assert receive_frame(connection) == b"getvar:max-download-size"
                send_frame(connection, b"OKAY0x00100000")
                assert receive_frame(connection) == b"download:00000010"
                send_frame(connection, b"DATA00000010")
                assert receive_frame(connection) == expected_payload
                send_frame(connection, b"OKAYdownloaded")
                assert receive_frame(connection) == b"flash:" + encoded_partition
                send_frame(connection, b"OKAYflashed")

            stdout, stderr = invoke(
                cli,
                ["--json", "flash", partition],
                flashed_default_extra,
                environment=default_environment,
            )
            document = parse_success_json(stdout, stderr)
            assert document == {
                "ok": True,
                "command": "flash",
                "partition": partition,
                "file": str(image),
            }

        def flashed_default_boot_slot_a(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:slot-count"
            send_frame(connection, b"OKAY2")
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:boot"
            send_frame(connection, b"OKAYyes")
            assert receive_frame(connection) == b"getvar:is-logical:boot_a"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"download:00000010"
            send_frame(connection, b"DATA00000010")
            assert receive_frame(connection) == default_boot_payload
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:boot_a"
            send_frame(connection, b"OKAYflashed")

        stdout, stderr = invoke(
            cli,
            ["--slot", "a", "--json", "flash", "boot"],
            flashed_default_boot_slot_a,
            environment=default_environment,
        )
        document = parse_success_json(stdout, stderr)
        assert document["file"] == str(default_boot)

        default_vendor_boot = default_product_out / "vendor_boot.img"
        default_vendor_boot.write_bytes(stage_payload)

        def flashed_default_vendor_boot(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:vendor_boot"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:vendor_boot"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"download:00000010"
            send_frame(connection, b"DATA00000010")
            assert receive_frame(connection) == stage_payload
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:vendor_boot"
            send_frame(connection, b"OKAYflashed")

        stdout, stderr = invoke(
            cli,
            ["--json", "flash", "vendor_boot"],
            flashed_default_vendor_boot,
            environment=default_environment,
        )
        document = parse_success_json(stdout, stderr)
        assert document["file"] == str(default_vendor_boot)

        missing_environment = os.environ.copy()
        missing_environment.pop("ANDROID_PRODUCT_OUT", None)
        missing_product_out = invoke_without_connection(
            cli, ["flash", "boot"], 4, "invalid_argument", missing_environment
        )
        assert missing_product_out["message"] == "ANDROID_PRODUCT_OUT not set"
        unknown_without_product_out = invoke_without_connection(
            cli,
            ["flash", "unknown"],
            4,
            "invalid_argument",
            missing_environment,
        )
        assert unknown_without_product_out["message"] == (
            "cannot determine image filename for 'unknown'"
        )

        missing_image_product_out = directory / "missing-image-product-out"
        missing_image_product_out.mkdir()
        missing_image_environment = os.environ.copy()
        missing_image_environment["ANDROID_PRODUCT_OUT"] = str(
            missing_image_product_out
        )
        missing_image = invoke_without_connection(
            cli, ["flash", "boot"], 4, "io", missing_image_environment
        )
        assert missing_image["message"] == "image file does not exist"

        for unresolved_partition in ("unknown", "boot_a", "vendor_boot:alpha"):
            unresolved = invoke_without_connection(
                cli,
                ["flash", unresolved_partition],
                4,
                "invalid_argument",
                default_environment,
            )
            assert unresolved["message"] == (
                f"cannot determine image filename for '{unresolved_partition}'"
            )

        def rejected_logical_flash(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:system"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:system"
            send_frame(connection, b"OKAYyes")

        stdout, stderr = invoke(
            cli,
            ["--json", "flash", "system", str(stage_file)],
            rejected_logical_flash,
            expected_exit=4,
        )
        parse_failure_json(stdout, stderr, "not_supported")

        def forced_logical_flash(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:system"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:system"
            send_frame(connection, b"OKAYyes")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"download:00000010"
            send_frame(connection, b"DATA00000010")
            assert receive_frame(connection) == stage_payload
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:system"
            send_frame(connection, b"OKAYflashed")

        stdout, stderr = invoke(
            cli,
            ["--force", "--json", "flash", "system", str(stage_file)],
            forced_logical_flash,
        )
        parse_success_json(stdout, stderr)

        sparse_limited_image = directory / "sparse-limited-raw.img"
        sparse_limited_image.write_bytes(
            bytes((index * 17 + 3) % 251 for index in range(3 * 4096))
        )

        def sparse_limited_flash(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:system"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:system"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:system"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:partition-size:system"
            send_frame(connection, b"OKAY0x3000")
            for _ in range(3):
                command = receive_frame(connection)
                assert command.startswith(b"download:")
                encoded_size = int(command[9:], 16)
                assert 0 < encoded_size <= 4200
                send_frame(connection, b"DATA" + command[9:])
                payload = receive_frame(connection)
                assert len(payload) == encoded_size
                assert payload[:4] == b"\x3a\xff\x26\xed"
                send_frame(connection, b"OKAYdownloaded")
                assert receive_frame(connection) == b"flash:system"
                send_frame(connection, b"OKAYflashed")

        stdout, stderr = invoke(
            cli,
            ["-S", "4200", "--json", "flash", "system",
             str(sparse_limited_image)],
            sparse_limited_flash,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "flash"
        assert document["partition"] == "system"

        vendor_boot_image = make_vendor_boot_v4()
        replacement_ramdisk = directory / "vendor-ramdisk.bin"
        replacement_dtb = directory / "vendor-dtb.bin"
        replacement_ramdisk.write_bytes(b"FIRST")
        replacement_dtb.write_bytes(b"new-dtb")

        def repacked_vendor_boot(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:vendor_boot"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:vendor_boot"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:max-fetch-size"
            send_frame(connection, b"OKAY0x00002000")
            assert receive_frame(connection) == b"getvar:partition-size:vendor_boot"
            send_frame(connection, b"OKAY0x00007000")
            for offset, size in ((0, 0x2000), (0x2000, 0x2000),
                                 (0x4000, 0x2000), (0x6000, 0x1000)):
                expected = (
                    f"fetch:vendor_boot:0x{offset:08x}:0x{size:08x}"
                ).encode("ascii")
                assert receive_frame(connection) == expected
                send_frame(connection, f"DATA{size:08x}".encode("ascii"))
                send_frame(connection, vendor_boot_image[offset : offset + size])
                send_frame(connection, b"OKAYfetched")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"download:00007000"
            send_frame(connection, b"DATA00007000")
            image = receive_frame(connection)
            assert len(image) == len(vendor_boot_image)
            assert image[:8] == b"VNDRBOOT"
            assert int.from_bytes(image[24:28], "little") == 9
            assert int.from_bytes(image[2100:2104], "little") == 7
            assert image[4096:4105] == b"FIRSTTWO2"
            assert image[8192:8199] == b"new-dtb"
            table = 3 * 4096
            assert int.from_bytes(image[table : table + 4], "little") == 5
            assert int.from_bytes(image[table + 4 : table + 8], "little") == 0
            assert int.from_bytes(image[table + 112 : table + 116], "little") == 4
            assert int.from_bytes(image[table + 116 : table + 120], "little") == 5
            assert image[table + 108] == 0xD1
            assert image[table + 112 + 44] == 0xB7
            assert image[table + 112 + 108] == 0xE1
            assert image[6 * 4096 + 11 : 6 * 4096 + 15] == b"TAIL"
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:vendor_boot"
            send_frame(connection, b"OKAYflashed")

        stdout, stderr = invoke(
            cli,
            [
                "--json",
                "--dtb",
                str(replacement_dtb),
                "flash",
                "vendor_boot:alpha",
                str(replacement_ramdisk),
            ],
            repacked_vendor_boot,
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "flash",
            "partition": "vendor_boot",
            "vendorRamdisk": "alpha",
            "file": str(replacement_ramdisk),
            "dtb": str(replacement_dtb),
        }

        vbmeta_file = directory / "vbmeta.img"
        vbmeta_payload = bytearray([0x5A] * 256)
        vbmeta_payload[0:4] = b"AVB0"
        vbmeta_payload[123] = 0x40
        vbmeta_file.write_bytes(vbmeta_payload)

        def flashed_vbmeta(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:vbmeta"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:vbmeta"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:is-logical:vbmeta"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:partition-size:vbmeta"
            send_frame(connection, b"OKAY0x1000")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"download:00000100"
            send_frame(connection, b"DATA00000100")
            transferred = receive_frame(connection)
            expected = bytearray(vbmeta_payload)
            expected[123] = 0x43
            assert transferred == expected
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:vbmeta"
            send_frame(connection, b"OKAYflashed")
        def flashed_all_slots(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:slot-count"
            send_frame(connection, b"OKAY2")
            assert receive_frame(connection) == b"getvar:slot-count"
            send_frame(connection, b"OKAY2")
            assert receive_frame(connection) == b"getvar:current-slot"
            send_frame(connection, b"OKAYa")
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:system"
            send_frame(connection, b"OKAYyes")
            assert receive_frame(connection) == b"getvar:is-logical:system_a"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            for slot in (b"a", b"b"):
                assert receive_frame(connection) == b"download:00000010"
                send_frame(connection, b"DATA00000010")
                assert receive_frame(connection) == stage_payload
                send_frame(connection, b"OKAYdownloaded")
                assert receive_frame(connection) == b"flash:system_" + slot
                send_frame(connection, b"OKAYflashed")
            assert receive_frame(connection) == b"set_active:b"
            send_frame(connection, b"OKAYactive")

        stdout, stderr = invoke(
            cli,
            [
                "--disable-verity",
                "--disable-verification",
                "--json",
                "flash",
                "vbmeta",
                str(vbmeta_file),
            ],
            flashed_vbmeta,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "flash"
        assert document["partition"] == "vbmeta"

        corrupt_vbmeta = directory / "corrupt-vbmeta.img"
        corrupt_vbmeta.write_bytes(bytes(256))
        rejected = invoke_without_connection(
            cli,
            [
                "--disable-verity",
                "flash",
                "vbmeta",
                str(corrupt_vbmeta),
            ],
            4,
            "invalid_argument",
        )
        assert "AVB0 magic" in str(rejected["message"])

        update_vbmeta_package = make_update_package(
            directory,
            "update-vbmeta",
            "version 1\nflash --apply-vbmeta vbmeta vbmeta.img\n",
        )
        (update_vbmeta_package / "vbmeta.img").write_bytes(vbmeta_payload)

        def updated_vbmeta(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            assert receive_frame(connection) == b"download:00000100"
            send_frame(connection, b"DATA00000100")
            transferred = receive_frame(connection)
            expected = bytearray(vbmeta_payload)
            expected[123] = 0x43
            assert transferred == expected
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:vbmeta"
            send_frame(connection, b"OKAYflashed")
            assert receive_frame(connection) == b"reboot"
            send_frame(connection, b"OKAYrebooting")

        stdout, stderr = invoke(
            cli,
            [
                "--slot",
                "all",
                "--set-active=other",
                "--json",
                "flash",
                "system",
                str(stage_file),
            ],
            flashed_all_slots,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "flash"

        def ambiguous_other_slot(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:slot-count"
            send_frame(connection, b"OKAY3")

        stdout, stderr = invoke(
            cli,
            ["--slot", "other", "--json", "flash", "system", str(stage_file)],
            ambiguous_other_slot,
            expected_exit=4,
        )
        parse_failure_json(stdout, stderr, "invalid_argument")

        def unsupported_slot(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:slot-count"
            send_frame(connection, b"OKAY2")
            assert receive_frame(connection) == b"getvar:is-userspace"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:has-slot:system"
            send_frame(connection, b"OKAYno")

        stdout, stderr = invoke(
            cli,
            ["--slot", "a", "--json", "flash", "system", str(stage_file)],
            unsupported_slot,
            expected_exit=4,
        )
        parse_failure_json(stdout, stderr, "not_supported")

        slotted_update = make_update_package(
            directory, "slotted-update", "version 1\nflash boot boot.img\n"
        )
        (slotted_update / "boot.img").write_bytes(stage_payload)

        def update_all_slots(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:has-slot:boot"
            send_frame(connection, b"OKAYyes")
            assert receive_frame(connection) == b"getvar:slot-count"
            send_frame(connection, b"OKAY2")
            assert receive_frame(connection) == b"getvar:slot-count"
            send_frame(connection, b"OKAY2")
            assert receive_frame(connection) == b"set_active:a"
            send_frame(connection, b"OKAYactive")
            assert receive_frame(connection) == b"getvar:max-download-size"
            send_frame(connection, b"OKAY0x00100000")
            for slot in (b"a", b"b"):
                assert receive_frame(connection) == b"download:00000010"
                send_frame(connection, b"DATA00000010")
                assert receive_frame(connection) == stage_payload
                send_frame(connection, b"OKAYdownloaded")
                assert receive_frame(connection) == b"flash:boot_" + slot
                send_frame(connection, b"OKAYflashed")
            assert receive_frame(connection) == b"reboot"
            send_frame(connection, b"OKAYrebooting")

        stdout, stderr = invoke(
            cli,
            [
                "--disable-verity",
                "--disable-verification",
                "--json",
                "update",
                str(update_vbmeta_package),
            ],
            updated_vbmeta,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "update"

        stdout, stderr = invoke(
            cli,
            [
                "--slot",
                "all",
                "-a",
                "--json",
                "update",
                str(slotted_update),
            ],
            update_all_slots,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "update"

        raw_kernel = directory / "raw-kernel.bin"
        raw_ramdisk = directory / "raw-ramdisk.bin"
        raw_second = directory / "raw-second.bin"
        raw_dtb = directory / "raw-board.dtb"
        raw_kernel_payload = bytes(index & 0xFF for index in range(2048))
        raw_ramdisk_payload = b"r\x00d"
        raw_second_payload = b"second"
        raw_dtb_payload = b"dtb"
        raw_kernel.write_bytes(raw_kernel_payload)
        raw_ramdisk.write_bytes(raw_ramdisk_payload)
        raw_second.write_bytes(raw_second_payload)
        raw_dtb.write_bytes(raw_dtb_payload)

        def flashed_raw_over_tcp(connection: socket.socket) -> None:
            # AOSP-aligned raw flash preflight: the frozen 37.0.1 trace only
            # queries has-slot for the target partition before download.
            assert receive_frame(connection) == b"getvar:has-slot:boot"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"download:00005000"
            send_frame(connection, b"DATA00005000")
            image = receive_frame(connection)
            assert len(image) == 20480
            assert image[:8] == b"ANDROID!"
            assert int.from_bytes(image[8:12], "little") == 2048
            assert int.from_bytes(image[12:16], "little") == 0x20001000
            assert int.from_bytes(image[16:20], "little") == 3
            assert int.from_bytes(image[20:24], "little") == 0x20002000
            assert int.from_bytes(image[24:28], "little") == len(raw_second_payload)
            assert int.from_bytes(image[28:32], "little") == 0x20F00000
            assert int.from_bytes(image[32:36], "little") == 0x20004000
            assert int.from_bytes(image[36:40], "little") == 4096
            assert int.from_bytes(image[40:44], "little") == 2
            assert int.from_bytes(image[44:48], "little") == (
                (15 << 25) | (1 << 18) | (25 << 4) | 2
            )
            assert int.from_bytes(image[1644:1648], "little") == 1660
            assert int.from_bytes(image[1648:1652], "little") == len(raw_dtb_payload)
            assert int.from_bytes(image[1652:1660], "little") == 0x21200000
            assert image[64 : 64 + len(b"console=ttyS0")] == b"console=ttyS0"
            assert image[4096:6144] == raw_kernel_payload
            assert image[8192:8195] == raw_ramdisk_payload
            assert image[12288 : 12288 + len(raw_second_payload)] == raw_second_payload
            assert image[16384 : 16384 + len(raw_dtb_payload)] == raw_dtb_payload
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"flash:boot"
            send_frame(connection, b"OKAYflashed")

        stdout, stderr = invoke(
            cli,
            [
                "--json",
                "--cmdline",
                "console=ttyS0",
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
                "--header-version",
                "2",
                "--os-version",
                "15.1",
                "--os-patch-level",
                "2025-02-05",
                "--dtb",
                str(raw_dtb),
                "--dtb-offset",
                "0x1200000",
                "flash:raw",
                "boot",
                str(raw_kernel),
                str(raw_ramdisk),
                str(raw_second),
            ],
            flashed_raw_over_tcp,
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "flash:raw",
            "partition": "boot",
            "kernel": str(raw_kernel),
        }

        signature_file = directory / "签名.bin"
        signature_payload = bytes(range(256))
        signature_file.write_bytes(signature_payload)

        def accepted_signature(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"download:00000100"
            send_frame(connection, b"DATA00000100")
            assert receive_frame(connection) == signature_payload
            send_frame(connection, b"OKAYdownloaded")
            assert receive_frame(connection) == b"signature"
            send_frame(connection, b"INFOverified")
            send_frame(connection, b"OKAYaccepted")

        stdout, stderr = invoke(
            cli,
            ["--json", "signature", str(signature_file)],
            accepted_signature,
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "signature"
        assert document["terminal"] == {
            "base64": base64.b64encode(b"accepted").decode("ascii"),
            "bytes": 8,
        }
        assert document["messages"] == [
            {
                "kind": "INFO",
                "base64": base64.b64encode(b"verified").decode("ascii"),
                "bytes": 8,
            }
        ]

        stdout, stderr = invoke_udp(
            cli,
            ["--json", "flash", "system", str(stage_file)],
            lambda server: serve_udp_flash(server, stage_payload),
        )
        document = parse_success_json(stdout, stderr)
        assert document == {
            "ok": True,
            "command": "flash",
            "partition": "system",
            "file": str(stage_file),
        }

        for invalid_selector in ("tcp:", "udp:127.0.0.1:70000"):
            completed = subprocess.run(
                [
                    str(cli),
                    "-s",
                    invalid_selector,
                    "flash",
                    "system",
                    str(stage_file),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=cli_test_environment(None, True, 5000),
                timeout=10,
                check=False,
            )
            if completed.returncode != 4 or completed.stderr:
                raise AssertionError(
                    f"invalid flash selector was not rejected locally: "
                    f"{invalid_selector!r}, exit={completed.returncode}, "
                    f"stdout={completed.stdout!r}, stderr={completed.stderr!r}"
                )
            parse_failure_json(
                completed.stdout, completed.stderr, "invalid_argument"
            )

        tcp_unavailable = invoke_flash_without_server(cli, "tcp", stage_file)
        assert tcp_unavailable["status"] in {"io", "timeout"}
        assert tcp_unavailable["transferState"] == "not_sent"
        udp_unavailable = invoke_flash_without_server(cli, "udp", stage_file)
        assert udp_unavailable["status"] in {"io", "timeout"}
        assert udp_unavailable["transferState"] == "not_sent"
        udp_zero_timeout = invoke_flash_without_server(
            cli, "udp", stage_file, timeout_ms=0
        )
        assert udp_zero_timeout["status"] == "timeout"
        assert udp_zero_timeout["transferState"] == "not_sent"

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

        staged_file = directory / "已暂存-结果.bin"
        staged_payload = b"s\x00\xfe"

        def staged_data(connection: socket.socket) -> None:
            # AOSP get_staged uses the upload wire command.
            assert receive_frame(connection) == b"upload"
            send_frame(connection, b"DATA00000003")
            send_frame(connection, staged_payload)
            send_frame(connection, b"OKAYstaged")

        stdout, stderr = invoke(
            cli, ["--json", "get_staged", str(staged_file)], staged_data
        )
        document = parse_success_json(stdout, stderr)
        assert document["command"] == "get_staged"
        assert document["dataBytes"] == 3
        assert document["output"] == str(staged_file)
        assert staged_file.read_bytes() == staged_payload
        assert_no_temporary_outputs(directory)

        fetch_file = directory / "分区-结果.bin"
        fetch_payload = b"f\x00\xff"

        def fetched_data(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:has-slot:vendor"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:max-fetch-size"
            send_frame(connection, b"OKAY0x2")
            assert receive_frame(connection) == b"getvar:partition-size:vendor"
            send_frame(connection, b"OKAY0x3")
            assert receive_frame(connection) == b"fetch:vendor:0x00000000:0x00000002"
            send_frame(connection, b"INFOfetching")
            send_frame(connection, b"DATA00000002")
            send_frame(connection, fetch_payload[:2])
            send_frame(connection, b"OKAYfetched")
            assert receive_frame(connection) == b"fetch:vendor:0x00000002:0x00000001"
            send_frame(connection, b"DATA00000001")
            send_frame(connection, fetch_payload[2:])
            send_frame(connection, b"OKAYfetched")

        stdout, stderr = invoke(
            cli,
            [
                "--json",
                "fetch",
                "vendor",
                str(fetch_file),
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

        def binary_failure(connection: socket.socket) -> None:
            assert receive_frame(connection) == b"getvar:has-slot:userdata"
            send_frame(connection, b"OKAYno")
            assert receive_frame(connection) == b"getvar:partition-type:userdata"
            send_frame(connection, b"OKAYraw")
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
