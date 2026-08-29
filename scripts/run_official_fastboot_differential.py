#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run an opt-in differential capture against an official Fastboot binary.

The scripted device records bytes received from each CLI. It does not synthesize
an AOSP capture from fixtures or command-line output.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import re
import socket
import struct
import subprocess
import sys
import tempfile
from typing import Any, Iterable, Optional, Sequence


SKIP_EXIT_CODE = 77
MAX_FRAME_BYTES = 4 * 1024 * 1024
PROCESS_TIMEOUT_SECONDS = 15


class CaptureGateError(RuntimeError):
    """The requested capture could not produce trustworthy evidence."""


@dataclasses.dataclass(frozen=True)
class VerifiedFastboot:
    path: pathlib.Path
    platform_key: str
    sha256: str
    version_output: str


@dataclasses.dataclass(frozen=True)
class Scenario:
    identifier: str
    transport: str
    arguments: tuple[str, ...]
    terminal_command: str
    image_path: Optional[pathlib.Path] = None
    coverage_ids: tuple[str, ...] = ()
    aosp_arguments: Optional[tuple[str, ...]] = None
    kairosboot_arguments: Optional[tuple[str, ...]] = None
    variable_values: tuple[tuple[str, str], ...] = ()
    environment: tuple[tuple[str, str], ...] = ()
    receive_payload: Optional[bytes] = None
    output_path: Optional[pathlib.Path] = None
    output_event_path: Optional[str] = None
    host_output_kind: Optional[str] = None
    informational_responses: tuple[tuple[str, str], ...] = ()
    terminal_occurrences: int = 1


def _reject_duplicate_keys(pairs: Iterable[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CaptureGateError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def _load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as source:
            value = json.load(source, object_pairs_hook=_reject_duplicate_keys)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CaptureGateError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise CaptureGateError(f"JSON root in {path} is not an object")
    return value


def _platform_key() -> str:
    system = platform.system()
    mapping = {"Darwin": "darwin", "Linux": "linux", "Windows": "windows"}
    try:
        return mapping[system]
    except KeyError as error:
        raise CaptureGateError(
            f"official Fastboot lock has no binary for platform {system!r}"
        ) from error


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while True:
                chunk = source.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError as error:
        raise CaptureGateError(f"cannot hash official Fastboot binary {path}: {error}") from error
    return digest.hexdigest()


def _repository_head(repository_root: pathlib.Path) -> str:
    try:
        completed = subprocess.run(
            [
                "git", "-C", str(repository_root), "rev-parse", "--verify",
                "HEAD^{commit}",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CaptureGateError(
            f"cannot resolve the KairosBoot source commit: {error}"
        ) from error
    commit = completed.stdout.strip()
    if completed.returncode != 0 or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise CaptureGateError(
            "official differential capture requires a Git checkout at an exact commit"
        )
    try:
        status = subprocess.run(
            [
                "git", "-C", str(repository_root), "status", "--porcelain",
                "--untracked-files=no",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CaptureGateError(
            f"cannot verify the KairosBoot source checkout: {error}"
        ) from error
    if status.returncode != 0 or status.stdout.strip():
        raise CaptureGateError(
            "official differential capture requires a clean tracked source checkout"
        )
    return commit


def _require_executable(path: pathlib.Path, label: str) -> pathlib.Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise CaptureGateError(f"{label} binary does not exist: {resolved}")
    if os.name != "nt" and not os.access(resolved, os.X_OK):
        raise CaptureGateError(f"{label} binary is not executable: {resolved}")
    return resolved


def verify_official_fastboot(
    binary: pathlib.Path, lock_path: pathlib.Path
) -> VerifiedFastboot:
    """Verify the platform-specific binary digest and locked release version."""

    resolved = _require_executable(binary, "official Fastboot")
    lock = _load_json(lock_path)
    try:
        aosp = lock["aosp"]
        locked_version = aosp["platformToolsVersion"]
        platform_key = _platform_key()
        expected_hash = aosp["officialArchives"][platform_key]["fastbootSha256"]
    except (KeyError, TypeError) as error:
        raise CaptureGateError(
            f"{lock_path} does not contain the platform Fastboot lock"
        ) from error
    if not isinstance(locked_version, str) or not re.fullmatch(
        r"[0-9]+(?:\.[0-9]+)+", locked_version
    ):
        raise CaptureGateError("locked Platform-Tools version is invalid")
    if not isinstance(expected_hash, str) or not re.fullmatch(
        r"[0-9a-f]{64}", expected_hash
    ):
        raise CaptureGateError("locked official Fastboot SHA-256 is invalid")

    observed_hash = _sha256_file(resolved)
    if observed_hash != expected_hash:
        raise CaptureGateError(
            "official Fastboot SHA-256 mismatch for "
            f"{platform_key}: expected {expected_hash}, observed {observed_hash}"
        )

    try:
        completed = subprocess.run(
            [str(resolved), "--version"],
            check=False,
            capture_output=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CaptureGateError(
            f"cannot query official Fastboot version from {resolved}: {error}"
        ) from error
    output_bytes = completed.stdout + completed.stderr
    output = output_bytes.decode("utf-8", errors="backslashreplace").strip()
    if completed.returncode != 0:
        raise CaptureGateError(
            f"official Fastboot --version exited {completed.returncode}: {output}"
        )
    version_pattern = re.compile(
        rf"(?<![0-9.]){re.escape(locked_version)}(?![0-9.])"
    )
    if version_pattern.search(output) is None:
        raise CaptureGateError(
            "official Fastboot version mismatch: "
            f"expected Platform-Tools {locked_version}, observed {output!r}"
        )
    return VerifiedFastboot(
        path=resolved,
        platform_key=platform_key,
        sha256=observed_hash,
        version_output=output,
    )


def _receive_exact(connection: socket.socket, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise CaptureGateError("Fastboot client closed the TCP session early")
        result.extend(chunk)
    return bytes(result)


def _receive_frame(connection: socket.socket) -> bytes:
    (length,) = struct.unpack(">Q", _receive_exact(connection, 8))
    if length > MAX_FRAME_BYTES:
        raise CaptureGateError(f"Fastboot TCP frame is too large: {length}")
    return _receive_exact(connection, length)


def _send_frame(connection: socket.socket, payload: bytes) -> None:
    connection.sendall(struct.pack(">Q", len(payload)) + payload)


class WireRecorder:
    def __init__(self, scenario: Scenario) -> None:
        self.scenario = scenario
        self.events: list[dict[str, Any]] = [
            {
                "kind": "CLI_PARSE",
                "argv": list(scenario.arguments),
                "result": "ok",
            }
        ]
        self.expected_data_size: Optional[int] = None
        self.downloaded: Optional[bytes] = None
        self.download_buffer = bytearray()
        self.pending_responses: list[bytes] = []
        self.finished = False
        self.terminal_seen = 0
        self.device_state: dict[str, Any] = {"product": "product_a"}

    def _finish_command(self, command: str) -> None:
        if command == self.scenario.terminal_command:
            self.terminal_seen += 1
            if self.terminal_seen >= self.scenario.terminal_occurrences:
                self.finished = True

    def handle(self, payload: bytes) -> Optional[bytes]:
        if self.finished:
            raise CaptureGateError("Fastboot client sent bytes after terminal response")
        if self.expected_data_size is not None:
            expected = self.expected_data_size
            self.download_buffer.extend(payload)
            if len(self.download_buffer) > expected:
                raise CaptureGateError(
                    f"download payload is {len(self.download_buffer)} bytes, expected {expected}"
                )
            if len(self.download_buffer) < expected:
                return None
            self.expected_data_size = None
            self.downloaded = bytes(self.download_buffer)
            self.download_buffer.clear()
            self.events.append(
                {
                    "kind": "DATA",
                    "direction": "host-to-device",
                    "size": len(self.downloaded),
                    "sha256": hashlib.sha256(self.downloaded).hexdigest(),
                }
            )
            self.events.append({"kind": "OKAY", "message": "downloaded"})
            if self.scenario.terminal_command == "download":
                self.finished = True
            return b"OKAYdownloaded"

        try:
            command = payload.decode("ascii")
        except UnicodeDecodeError as error:
            raise CaptureGateError(
                "received non-ASCII bytes outside a DATA phase"
            ) from error

        if command.startswith("getvar:"):
            name = command[len("getvar:") :]
            self.events.append({"kind": "GETVAR", "name": name})
            values = {
                "product": "product_a",
                "max-download-size": "0x00100000",
                "has-slot:system": "no",
                "is-logical:system": "no",
                "partition-type:system": "raw",
                "is-userspace": "no",
                "unlocked": "yes",
            }
            values.update(dict(self.scenario.variable_values))
            message = values.get(name, "")
            self.events.append({"kind": "OKAY", "message": message})
            self._finish_command(command)
            return b"OKAY" + message.encode("ascii")

        self.events.append({"kind": "COMMAND", "command": command})
        if self.scenario.receive_payload is not None and (
            command == "upload" or command.startswith("fetch:")
        ):
            received = self.scenario.receive_payload
            terminal_message = "uploaded" if command == "upload" else "fetched"
            self.events.append(
                {
                    "kind": "DATA",
                    "direction": "device-to-host",
                    "size": len(received),
                    "sha256": hashlib.sha256(received).hexdigest(),
                }
            )
            self.events.append({"kind": "OKAY", "message": terminal_message})
            self.pending_responses = [
                received,
                b"OKAY" + terminal_message.encode("ascii"),
            ]
            self._finish_command(command)
            return f"DATA{len(received):08x}".encode("ascii")
        if command.startswith("download:"):
            encoded_size = command[len("download:") :]
            if re.fullmatch(r"[0-9a-fA-F]{8}", encoded_size) is None:
                raise CaptureGateError(f"invalid download command: {command!r}")
            self.expected_data_size = int(encoded_size, 16)
            self.download_buffer.clear()
            return b"DATA" + encoded_size.lower().encode("ascii")
        if command.startswith("flash:"):
            if self.downloaded is None:
                raise CaptureGateError("flash command arrived before a download")
            partition = command[len("flash:") :]
            partitions = self.device_state.setdefault("partitions", {})
            partitions[partition] = {
                "size": len(self.downloaded),
                "sha256": hashlib.sha256(self.downloaded).hexdigest(),
            }
            self.events.append({"kind": "OKAY", "message": "flashed"})
            self._finish_command(command)
            return b"OKAYflashed"
        if command == "signature":
            if self.downloaded is None:
                raise CaptureGateError("signature command arrived before a download")
            self.device_state["signature"] = {
                "size": len(self.downloaded),
                "sha256": hashlib.sha256(self.downloaded).hexdigest(),
            }
            self.events.append({"kind": "OKAY", "message": "accepted"})
            self._finish_command(command)
            return b"OKAYaccepted"
        if command.startswith("erase:"):
            partition = command[len("erase:") :]
            self.device_state["erased"] = [partition]
            self.events.append({"kind": "OKAY", "message": "erased"})
            self._finish_command(command)
            return b"OKAYerased"
        if command == "boot":
            if self.downloaded is None:
                raise CaptureGateError("boot command arrived before a download")
            self.device_state["boot"] = {
                "size": len(self.downloaded),
                "sha256": hashlib.sha256(self.downloaded).hexdigest(),
            }
            self.events.append({"kind": "OKAY", "message": "booting"})
            self._finish_command(command)
            return b"OKAYbooting"
        if command.startswith("set_active:"):
            slot = command[len("set_active:") :]
            self.device_state["activeSlot"] = slot
            self.events.append({"kind": "OKAY", "message": "active"})
            self._finish_command(command)
            return b"OKAYactive"
        if command.startswith("update-super:"):
            if self.downloaded is None:
                raise CaptureGateError("update-super command arrived before a download")
            self.device_state["superUpdate"] = {
                "command": command,
                "size": len(self.downloaded),
                "sha256": hashlib.sha256(self.downloaded).hexdigest(),
            }
            self.events.append({"kind": "OKAY", "message": "updated"})
            self._finish_command(command)
            return b"OKAYupdated"
        if command == "stage":
            if self.downloaded is None:
                raise CaptureGateError("stage command arrived before a download")
            self.device_state["staged"] = {
                "size": len(self.downloaded),
                "sha256": hashlib.sha256(self.downloaded).hexdigest(),
            }
            self.events.append({"kind": "OKAY", "message": "staged"})
            self._finish_command(command)
            return b"OKAYstaged"
        simple_commands = {
            "reboot", "reboot-bootloader", "reboot-recovery", "reboot-fastboot",
            "continue", "oem differential", "oem differential-info",
            "flashing get_unlock_ability",
            "flashing lock", "flashing unlock", "flashing lock_critical",
            "flashing unlock_critical",
            "create-logical-partition:differential:4096",
            "delete-logical-partition:differential",
            "resize-logical-partition:differential:8192",
            "gsi:wipe", "gsi:disable", "gsi:status",
            "snapshot-update:cancel", "snapshot-update:merge",
        }
        if command in simple_commands:
            commands = self.device_state.setdefault("commands", [])
            commands.append(command)
            encoded_responses: list[bytes] = []
            for kind, message in self.scenario.informational_responses:
                if kind not in {"INFO", "TEXT"}:
                    raise CaptureGateError(
                        f"unsupported informational response kind: {kind}"
                    )
                self.events.append({"kind": kind, "message": message})
                encoded_responses.append(kind.encode("ascii") + message.encode("utf-8"))
            self.events.append({"kind": "OKAY", "message": "accepted"})
            self._finish_command(command)
            if encoded_responses:
                self.pending_responses = [*encoded_responses[1:], b"OKAYaccepted"]
                return encoded_responses[0]
            return b"OKAYaccepted"

        self.events.append({"kind": "FAIL", "message": "unsupported scripted command"})
        self.finished = True
        return b"FAILunsupported scripted command"

    def take_pending_responses(self) -> list[bytes]:
        responses = self.pending_responses
        self.pending_responses = []
        return responses

    def capture(self, exit_code: int) -> dict[str, Any]:
        if not self.finished:
            raise CaptureGateError(
                f"scenario {self.scenario.identifier} ended before its terminal command"
            )
        if self.scenario.output_path is not None:
            if self.scenario.output_event_path is None:
                raise CaptureGateError("output scenario has no normalized FILE path")
            try:
                output = self.scenario.output_path.read_bytes()
            except OSError as error:
                raise CaptureGateError(
                    f"Fastboot client did not create {self.scenario.output_path}: {error}"
                ) from error
            if output != self.scenario.receive_payload:
                raise CaptureGateError("Fastboot client output differs from device payload")
            self.events.append(
                {
                    "kind": "FILE",
                    "path": self.scenario.output_event_path,
                    "size": len(output),
                    "sha256": hashlib.sha256(output).hexdigest(),
                }
            )
        self.events.append({"kind": "EXIT", "code": exit_code})
        return {
            "id": self.scenario.identifier,
            "events": self.events,
            "deviceState": self.device_state,
        }


def _completed_process(
    process: subprocess.Popen[bytes], recorder: WireRecorder, label: str
) -> dict[str, Any]:
    try:
        stdout, stderr = process.communicate(timeout=PROCESS_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as error:
        process.kill()
        stdout, stderr = process.communicate()
        raise CaptureGateError(
            f"{label} timed out: stdout={stdout!r}, stderr={stderr!r}"
        ) from error
    if process.returncode != 0:
        raise CaptureGateError(
            f"{label} exited {process.returncode}: stdout={stdout!r}, stderr={stderr!r}"
        )
    return recorder.capture(process.returncode)


def _capture_tcp(command: Sequence[str], scenario: Scenario, label: str) -> dict[str, Any]:
    recorder = WireRecorder(scenario)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        endpoint = f"tcp:127.0.0.1:{listener.getsockname()[1]}"
        expanded = [part.replace("{endpoint}", endpoint) for part in command]
        environment = os.environ.copy()
        environment.update(scenario.environment)
        process = subprocess.Popen(
            expanded,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(10)
                hello = _receive_exact(connection, 4)
                if hello != b"FB01":
                    raise CaptureGateError(f"unexpected Fastboot TCP handshake: {hello!r}")
                connection.sendall(hello)
                while not recorder.finished:
                    response = recorder.handle(_receive_frame(connection))
                    if response is not None:
                        _send_frame(connection, response)
                    for pending in recorder.take_pending_responses():
                        _send_frame(connection, pending)
        except socket.timeout as error:
            process.kill()
            stdout, stderr = process.communicate()
            raise CaptureGateError(
                f"{label} TCP exchange timed out; events={recorder.events!r}; "
                f"stdout={stdout!r}, stderr={stderr!r}"
            ) from error
        except CaptureGateError as error:
            process.kill()
            stdout, stderr = process.communicate()
            raise CaptureGateError(
                f"{error}; events={recorder.events!r}; "
                f"stdout={stdout!r}, stderr={stderr!r}"
            ) from error
        except BaseException:
            process.kill()
            process.communicate()
            raise
    return _completed_process(process, recorder, label)


def _udp_packet(packet_id: int, sequence: int, payload: bytes = b"") -> bytes:
    return struct.pack(">BBH", packet_id, 0, sequence) + payload


def _capture_udp(command: Sequence[str], scenario: Scenario, label: str) -> dict[str, Any]:
    recorder = WireRecorder(scenario)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
        server.bind(("127.0.0.1", 0))
        server.settimeout(10)
        endpoint = f"udp:127.0.0.1:{server.getsockname()[1]}"
        expanded = [part.replace("{endpoint}", endpoint) for part in command]
        environment = os.environ.copy()
        environment.update(scenario.environment)
        process = subprocess.Popen(
            expanded,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        try:
            query, peer = server.recvfrom(8192)
            if query != _udp_packet(1, 0):
                raise CaptureGateError(f"unexpected Fastboot UDP query: {query!r}")
            server.sendto(_udp_packet(1, 0, struct.pack(">H", 100)), peer)

            init, observed_peer = server.recvfrom(8192)
            if (
                observed_peer != peer
                or len(init) != 8
                or init[:4] != _udp_packet(2, 100)
            ):
                raise CaptureGateError(f"unexpected Fastboot UDP init: {init!r}")
            version, requested_packet_size = struct.unpack(">HH", init[4:])
            if version != 1 or requested_packet_size < 4:
                raise CaptureGateError(f"unsupported Fastboot UDP init: {init!r}")
            negotiated_packet_size = min(requested_packet_size, 512)
            server.sendto(
                _udp_packet(
                    2, 100, struct.pack(">HH", 1, negotiated_packet_size)
                ),
                peer,
            )

            sequence = 101
            while not recorder.finished:
                request, observed_peer = server.recvfrom(8192)
                if observed_peer != peer or request[:4] != _udp_packet(3, sequence):
                    raise CaptureGateError(f"unexpected Fastboot UDP request: {request!r}")
                response = recorder.handle(request[4:])
                if response is None:
                    raise CaptureGateError(
                        "fragmented Fastboot DATA is unsupported by the UDP fixture"
                    )
                server.sendto(_udp_packet(3, sequence), peer)
                sequence += 1

                poll, observed_peer = server.recvfrom(8192)
                if observed_peer != peer or poll != _udp_packet(3, sequence):
                    raise CaptureGateError(f"unexpected Fastboot UDP response poll: {poll!r}")
                server.sendto(_udp_packet(3, sequence, response), peer)
                sequence += 1
        except BaseException:
            process.kill()
            process.communicate()
            raise
    return _completed_process(process, recorder, label)


def capture_scenario(
    command: Sequence[str], scenario: Scenario, label: str
) -> dict[str, Any]:
    if scenario.transport == "host":
        return _capture_host(command, scenario, label)
    if scenario.transport == "tcp":
        return _capture_tcp(command, scenario, label)
    if scenario.transport == "udp":
        return _capture_udp(command, scenario, label)
    raise CaptureGateError(f"unknown scenario transport: {scenario.transport}")


def _capture_host(
    command: Sequence[str], scenario: Scenario, label: str
) -> dict[str, Any]:
    environment = os.environ.copy()
    environment.update(scenario.environment)
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            check=False,
            env=environment,
            timeout=PROCESS_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CaptureGateError(f"{label} host command failed: {error}") from error
    if completed.returncode != 0:
        raise CaptureGateError(
            f"{label} host command exited {completed.returncode}: "
            f"stdout={completed.stdout!r}, stderr={completed.stderr!r}"
        )
    try:
        output = (completed.stdout + completed.stderr).decode("utf-8")
    except UnicodeDecodeError as error:
        raise CaptureGateError(f"{label} host output is not UTF-8") from error
    output_kind = scenario.host_output_kind
    if output_kind == "help" and "usage" not in output.lower():
        raise CaptureGateError(f"{label} help output has no usage text")
    if output_kind == "version" and (
        not output.strip() or re.search(r"\d+\.\d+", output) is None
    ):
        raise CaptureGateError(f"{label} version output has no version number")
    if output_kind not in {"devices", "help", "version"}:
        raise CaptureGateError("host scenario has no supported output contract")
    return {
        "id": scenario.identifier,
        "events": [
            {
                "kind": "CLI_PARSE",
                "argv": list(scenario.arguments),
                "result": "ok",
            },
            {"kind": "TEXT", "message": output_kind},
            {"kind": "EXIT", "code": completed.returncode},
        ],
        "deviceState": {"hostOutputKind": output_kind},
    }


def _aosp_command(binary: pathlib.Path, scenario: Scenario) -> list[str]:
    arguments = scenario.aosp_arguments or scenario.arguments
    if scenario.transport == "host":
        return [str(binary), *arguments]
    return [str(binary), "-s", "{endpoint}", *arguments]


def _kairosboot_command(binary: pathlib.Path, scenario: Scenario) -> list[str]:
    arguments = scenario.kairosboot_arguments or scenario.arguments
    if scenario.transport == "host":
        return [str(binary), *arguments]
    return [
        str(binary),
        "-s",
        "{endpoint}",
        *arguments,
    ]


def _write_json(path: pathlib.Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _load_comparator(repository_root: pathlib.Path) -> Any:
    path = repository_root / "scripts" / "compare_fastboot_traces.py"
    spec = importlib.util.spec_from_file_location("kairosboot_trace_comparator", path)
    if spec is None or spec.loader is None:
        raise CaptureGateError(f"cannot load trace comparator from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _scenario_catalog(output_dir: pathlib.Path) -> list[Scenario]:
    image_path = output_dir / "differential-system.img"
    image_path.write_bytes(bytes(range(32)))
    signature_path = output_dir / "differential-signature.bin"
    signature_path.write_bytes(bytes(range(256)))
    kernel_path = output_dir / "differential-kernel.bin"
    kernel_path.write_bytes(bytes(index & 0xFF for index in range(2000)))
    ramdisk_path = output_dir / "differential-ramdisk.bin"
    ramdisk_path.write_bytes(b"differential-ramdisk")
    dtb_path = output_dir / "differential-dtb.bin"
    dtb_path.write_bytes(bytes(range(64)))
    vbmeta_path = output_dir / "differential-vbmeta.img"
    vbmeta_payload = bytearray([0x5A] * 256)
    vbmeta_payload[0:4] = b"AVB0"
    vbmeta_payload[123] = 0x40
    vbmeta_path.write_bytes(vbmeta_payload)
    sparse_input_path = output_dir / "differential-sparse-input.img"
    sparse_input_path.write_bytes(
        bytes((index * 17 + 3) % 251 for index in range(8192))
    )
    receive_payload = b"kairosboot-receive\x00\xff"
    staged_output = output_dir / "staged-output.bin"
    default_system_path = output_dir / "system.img"
    default_system_path.write_bytes(bytes(range(32)))
    return [
        Scenario(
            "official-host-devices", "host", ("devices",), "host",
            coverage_ids=("command.devices",), host_output_kind="devices",
        ),
        Scenario(
            "official-host-help", "host", ("--help",), "host",
            coverage_ids=("option.help",), host_output_kind="help",
        ),
        Scenario(
            "official-host-help-short", "host", ("-h",), "host",
            coverage_ids=("option.help-short",), host_output_kind="help",
        ),
        Scenario(
            "official-host-version", "host", ("--version",), "host",
            coverage_ids=("option.version",), host_output_kind="version",
        ),
        Scenario(
            "official-tcp-getvar", "tcp", ("getvar", "product"),
            "getvar:product", coverage_ids=("command.getvar", "transport.tcp"),
        ),
        Scenario(
            "official-udp-getvar", "udp", ("getvar", "product"),
            "getvar:product", coverage_ids=("command.getvar", "transport.udp"),
        ),
        Scenario(
            "official-tcp-flash", "tcp",
            ("flash", "system", "<ARTIFACT>/system.img"), "flash:system", image_path,
            ("command.flash", "protocol.command", "protocol.download",
             "capability.raw-image", "image.system"),
            aosp_arguments=("flash", "system", str(image_path)),
            kairosboot_arguments=("flash", "system", str(image_path)),
        ),
        Scenario(
            "official-udp-flash", "udp",
            ("flash", "system", "<ARTIFACT>/system.img"), "flash:system", image_path,
            ("command.flash", "protocol.command", "protocol.download",
             "capability.raw-image", "image.system"),
            aosp_arguments=("flash", "system", str(image_path)),
            kairosboot_arguments=("flash", "system", str(image_path)),
        ),
        Scenario(
            "official-tcp-flash-default-file", "tcp",
            ("flash", "system"), "flash:system", default_system_path,
            ("command.flash", "protocol.command", "protocol.download",
             "capability.raw-image", "image.system"),
            environment=(("ANDROID_PRODUCT_OUT", str(output_dir)),),
        ),
        Scenario(
            "official-tcp-flash-force", "tcp",
            ("--force", "flash", "system", "<ARTIFACT>/system.img"),
            "flash:system", image_path, ("option.force",),
            aosp_arguments=("--force", "flash", "system", str(image_path)),
            kairosboot_arguments=("--force", "flash", "system", str(image_path)),
        ),
        Scenario(
            "official-tcp-signature", "tcp",
            ("signature", "<ARTIFACT>/signature.bin"), "signature", signature_path,
            ("command.signature",),
            aosp_arguments=("signature", str(signature_path)),
            kairosboot_arguments=("signature", str(signature_path)),
        ),
        Scenario(
            "official-udp-signature", "udp",
            ("signature", "<ARTIFACT>/signature.bin"), "signature", signature_path,
            ("command.signature",),
            aosp_arguments=("signature", str(signature_path)),
            kairosboot_arguments=("signature", str(signature_path)),
        ),
        Scenario(
            "official-tcp-reboot", "tcp", ("reboot",), "reboot",
            coverage_ids=("command.reboot",),
        ),
        Scenario(
            "official-tcp-reboot-bootloader", "tcp",
            ("reboot", "bootloader"), "reboot-bootloader",
            coverage_ids=("command.reboot-bootloader",),
        ),
        Scenario(
            "official-tcp-reboot-recovery", "tcp",
            ("reboot-recovery",), "reboot-recovery",
            coverage_ids=("command.reboot-recovery",),
            aosp_arguments=("reboot-recovery",),
        ),
        Scenario(
            "official-tcp-continue", "tcp", ("continue",), "continue",
            coverage_ids=("command.continue",),
        ),
        Scenario(
            "official-tcp-erase", "tcp", ("erase", "system"),
            "erase:system", coverage_ids=("command.erase",),
        ),
        Scenario(
            "official-tcp-set-active", "tcp", ("set_active", "b"),
            "set_active:b",
            coverage_ids=("command.set-active", "capability.a-b-slots"),
            variable_values=(("slot-count", "2"),),
        ),
        Scenario(
            "official-tcp-slot-options", "tcp",
            ("--slot", "b", "--set-active", "flash", "system",
             "<ARTIFACT>/system.img"),
            "set_active:b", image_path,
            ("option.slot", "option.set-active"),
            aosp_arguments=("--slot", "b", "--set-active", "flash", "system",
                            str(image_path)),
            kairosboot_arguments=("--slot", "b", "--set-active", "flash",
                                  "system", str(image_path)),
            variable_values=(("slot-count", "2"),
                             ("has-slot:system", "yes"),
                             ("is-logical:system_b", "no")),
        ),
        Scenario(
            "official-tcp-avb-flags", "tcp",
            ("--disable-verity", "--disable-verification", "flash", "vbmeta",
             "<ARTIFACT>/vbmeta.img"),
            "flash:vbmeta", vbmeta_path,
            ("option.disable-verity", "option.disable-verification",
             "capability.vbmeta-avb-mutation"),
            aosp_arguments=("--disable-verity", "--disable-verification",
                            "flash", "vbmeta", str(vbmeta_path)),
            kairosboot_arguments=("--disable-verity", "--disable-verification",
                                  "flash", "vbmeta", str(vbmeta_path)),
            variable_values=(("has-slot:vbmeta", "no"),
                             ("is-logical:vbmeta", "no"),
                             ("partition-size:vbmeta", "0x1000")),
        ),
        Scenario(
            "official-tcp-sparse-limit", "tcp",
            ("-S", "4200", "flash", "system",
             "<ARTIFACT>/sparse-input.img"),
            "flash:system", sparse_input_path,
            ("option.sparse-limit", "capability.android-sparse"),
            aosp_arguments=("-S", "4200", "flash", "system",
                            str(sparse_input_path)),
            kairosboot_arguments=("-S", "4200", "flash", "system",
                                  str(sparse_input_path)),
            variable_values=(("has-slot:system", "no"),
                             ("is-logical:system", "no"),
                             ("partition-size:system", "0x2000")),
            terminal_occurrences=2,
        ),
        Scenario(
            "official-tcp-oem", "tcp", ("oem", "differential"),
            "oem differential", coverage_ids=("command.oem",),
        ),
        Scenario(
            "official-tcp-informational-responses", "tcp",
            ("oem", "differential-info"), "oem differential-info",
            coverage_ids=("protocol.responses",),
            informational_responses=(("INFO", "phase one"), ("TEXT", "phase two")),
        ),
        Scenario(
            "official-tcp-stage", "tcp", ("stage", "<ARTIFACT>/stage.bin"),
            "download", image_path, ("command.stage",),
            aosp_arguments=("stage", str(image_path)),
            kairosboot_arguments=("stage", str(image_path)),
        ),
        Scenario(
            "official-tcp-get-staged", "tcp",
            ("get_staged", "<OUTPUT>/stage.bin"), "upload",
            coverage_ids=("command.get-staged", "protocol.upload"),
            aosp_arguments=("get_staged", str(staged_output)),
            kairosboot_arguments=("get_staged", str(staged_output)),
            receive_payload=receive_payload,
            output_path=staged_output,
            output_event_path="<OUTPUT>/stage.bin",
        ),
        Scenario(
            "official-tcp-flashing-get-unlock-ability", "tcp",
            ("flashing", "get_unlock_ability"),
            "flashing get_unlock_ability",
            coverage_ids=("command.flashing-get-unlock-ability",),
            aosp_arguments=("flashing", "get_unlock_ability"),
            kairosboot_arguments=("flashing", "get_unlock_ability"),
        ),
        Scenario(
            "official-tcp-flashing-lock", "tcp", ("flashing", "lock"),
            "flashing lock", coverage_ids=("command.flashing-lock",),
        ),
        Scenario(
            "official-tcp-flashing-unlock", "tcp", ("flashing", "unlock"),
            "flashing unlock", coverage_ids=("command.flashing-unlock",),
        ),
        Scenario(
            "official-tcp-flashing-lock-critical", "tcp",
            ("flashing", "lock_critical"), "flashing lock_critical",
            coverage_ids=("command.flashing-lock-critical",),
            aosp_arguments=("flashing", "lock_critical"),
            kairosboot_arguments=("flashing", "lock_critical"),
        ),
        Scenario(
            "official-tcp-flashing-unlock-critical", "tcp",
            ("flashing", "unlock_critical"), "flashing unlock_critical",
            coverage_ids=("command.flashing-unlock-critical",),
            aosp_arguments=("flashing", "unlock_critical"),
            kairosboot_arguments=("flashing", "unlock_critical"),
        ),
        Scenario(
            "official-tcp-create-logical-partition", "tcp",
            ("create-logical-partition", "differential", "4096"),
            "create-logical-partition:differential:4096",
            coverage_ids=("command.create-logical-partition",),
        ),
        Scenario(
            "official-tcp-delete-logical-partition", "tcp",
            ("delete-logical-partition", "differential"),
            "delete-logical-partition:differential",
            coverage_ids=("command.delete-logical-partition",),
        ),
        Scenario(
            "official-tcp-gsi-wipe", "tcp", ("gsi", "wipe"), "gsi:wipe",
            coverage_ids=("command.gsi-wipe",),
        ),
        Scenario(
            "official-tcp-gsi-disable", "tcp", ("gsi", "disable"),
            "gsi:disable", coverage_ids=("command.gsi-disable",),
        ),
        Scenario(
            "official-tcp-gsi-status", "tcp", ("gsi", "status"),
            "gsi:status", coverage_ids=("command.gsi-status",),
        ),
        Scenario(
            "official-tcp-snapshot-cancel", "tcp",
            ("snapshot-update", "cancel"), "snapshot-update:cancel",
            coverage_ids=("command.snapshot-cancel",),
        ),
        Scenario(
            "official-tcp-snapshot-merge", "tcp",
            ("snapshot-update", "merge"), "snapshot-update:merge",
            coverage_ids=("command.snapshot-merge",),
        ),
        Scenario(
            "official-tcp-serial-selector", "tcp",
            ("-s", "<ENDPOINT>", "getvar", "product"),
            "getvar:product", coverage_ids=("option.serial",),
            aosp_arguments=("getvar", "product"),
            kairosboot_arguments=("getvar", "product"),
        ),
        Scenario(
            "official-tcp-verbose", "tcp",
            ("--verbose", "getvar", "product"), "getvar:product",
            coverage_ids=("option.verbose",),
        ),
        Scenario(
            "official-tcp-boot-raw", "tcp",
            ("boot", "<ARTIFACT>/kernel.bin", "<ARTIFACT>/ramdisk.bin"),
            "boot", coverage_ids=("command.boot",
                                   "capability.boot-image-construction"),
            aosp_arguments=("boot", str(kernel_path), str(ramdisk_path)),
            kairosboot_arguments=("boot", str(kernel_path), str(ramdisk_path)),
        ),
        Scenario(
            "official-tcp-boot-raw-options", "tcp",
            (
                "--base", "0x10000000",
                "--kernel-offset", "0x00008000",
                "--ramdisk-offset", "0x01000000",
                "--tags-offset", "0x00000100",
                "--page-size", "4096",
                "--header-version", "2",
                "--os-version", "13.0.0",
                "--os-patch-level", "2024-01-05",
                "--cmdline", "console=ttyS0 differential",
                "--dtb", "<ARTIFACT>/dtb.bin",
                "--dtb-offset", "0x01100000",
                "boot", "<ARTIFACT>/kernel.bin", "<ARTIFACT>/ramdisk.bin",
            ),
            "boot",
            coverage_ids=(
                "option.base", "option.kernel-offset", "option.ramdisk-offset",
                "option.tags-offset", "option.page-size", "option.header-version",
                "option.os-version", "option.os-patch-level", "option.cmdline",
                "option.dtb", "option.dtb-offset",
            ),
            aosp_arguments=(
                "--base", "0x10000000",
                "--kernel-offset", "0x00008000",
                "--ramdisk-offset", "0x01000000",
                "--tags-offset", "0x00000100",
                "--page-size", "4096",
                "--header-version", "2",
                "--os-version", "13.0.0",
                "--os-patch-level", "2024-01-05",
                "--cmdline", "console=ttyS0 differential",
                "--dtb", str(dtb_path),
                "--dtb-offset", "0x01100000",
                "boot", str(kernel_path), str(ramdisk_path),
            ),
            kairosboot_arguments=(
                "--base", "0x10000000",
                "--kernel-offset", "0x00008000",
                "--ramdisk-offset", "0x01000000",
                "--tags-offset", "0x00000100",
                "--page-size", "4096",
                "--header-version", "2",
                "--os-version", "13.0.0",
                "--os-patch-level", "2024-01-05",
                "--cmdline", "console=ttyS0 differential",
                "--dtb", str(dtb_path),
                "--dtb-offset", "0x01100000",
                "boot", str(kernel_path), str(ramdisk_path),
            ),
        ),
        Scenario(
            "official-tcp-flash-raw", "tcp",
            ("flash:raw", "boot", "<ARTIFACT>/kernel.bin",
             "<ARTIFACT>/ramdisk.bin"),
            "flash:boot", coverage_ids=("command.flash-raw",
                                         "capability.boot-image-construction"),
            aosp_arguments=("flash:raw", "boot", str(kernel_path),
                            str(ramdisk_path)),
            kairosboot_arguments=("flash:raw", "boot", str(kernel_path),
                                  str(ramdisk_path)),
            variable_values=(("has-slot:boot", "no"),
                             ("is-logical:boot", "no"),
                             ("partition-type:boot", "raw")),
        ),
    ]


UNCOVERED_SCENARIOS: tuple[dict[str, Any], ...] = (
    {
        "id": "official-scripted-fetch-chunking",
        "coverageIds": ["command.fetch"],
        "reason": (
            "official Fastboot probes has-slot and max-fetch-size and performs "
            "ranged chunked fetches, while the current no-range KairosBoot CLI "
            "sends one direct fetch:partition receive command"
        ),
    },
    {
        "id": "official-scripted-reboot-fastboot",
        "coverageIds": ["command.reboot-fastboot"],
        "reason": (
            "official Fastboot reconnects after reboot-fastboot and verifies "
            "getvar:is-userspace, while the current KairosBoot reboot API retires "
            "the session after the terminal response"
        ),
    },
    {
        "id": "official-scripted-format",
        "coverageIds": ["command.format", "option.fs-options"],
        "reason": (
            "format delegates filesystem construction to platform mkfs tools; "
            "their filesystem bytes are not deterministic across hosted platforms"
        ),
    },
    {
        "id": "official-scripted-wipe-super",
        "coverageIds": ["command.wipe-super", "capability.dynamic-partitions"],
        "reason": (
            "wipe-super requires a valid device-specific liblp metadata image and "
            "fastbootd state that the transport-only scripted device cannot infer"
        ),
    },
    {
        "id": "official-scripted-update-flashall",
        "coverageIds": ["command.update", "command.flashall", "capability.update-zip"],
        "reason": (
            "update and flashall may reboot between bootloader and fastbootd and reopen "
            "the device; the single-session TCP/UDP scripted transport cannot model "
            "that lifecycle without turning the device plan into a fixture oracle"
        ),
    },
    {
        "id": "official-scripted-resize-logical-partition",
        "coverageIds": ["command.resize-logical-partition"],
        "reason": (
            "official Fastboot fetches and parses device-specific super metadata before "
            "resizing; a transport-only scripted device cannot synthesize that metadata "
            "without becoming a fixture oracle"
        ),
    },
)


def _run_capture(arguments: argparse.Namespace) -> int:
    if arguments.fastboot is None or arguments.kairosboot is None:
        missing = []
        if arguments.fastboot is None:
            missing.append("--fastboot")
        if arguments.kairosboot is None:
            missing.append("--kairosboot")
        message = "official differential capture not requested; missing " + ", ".join(missing)
        if arguments.require:
            raise CaptureGateError(message)
        print(f"SKIP: {message}")
        return SKIP_EXIT_CODE

    repository_root = arguments.repository_root.resolve()
    lock_path = arguments.lock or repository_root / "compat" / "aosp.lock.json"
    verified = verify_official_fastboot(arguments.fastboot, lock_path)
    lock = _load_json(lock_path)
    try:
        platform_tools_version = lock["aosp"]["platformToolsVersion"]
        source_commit = lock["aosp"]["sourceCommit"]
    except (KeyError, TypeError) as error:
        raise CaptureGateError("official Fastboot lock is missing baseline metadata") from error
    kairosboot = _require_executable(arguments.kairosboot, "KairosBoot")
    kairosboot_commit = _repository_head(repository_root)
    kairosboot_artifact_sha256 = _sha256_file(kairosboot)

    output_dir = arguments.output_dir
    temporary: Optional[tempfile.TemporaryDirectory[str]] = None
    if output_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix="kairosboot-aosp-capture-")
        output_dir = pathlib.Path(temporary.name)
    output_dir.mkdir(parents=True, exist_ok=True)
    artifact_temporary = tempfile.TemporaryDirectory(
        prefix="kairosboot-aosp-artifacts-"
    )
    scenarios = _scenario_catalog(pathlib.Path(artifact_temporary.name))
    selected_scenarios = getattr(arguments, "scenario", [])
    if selected_scenarios:
        requested = set(selected_scenarios)
        known = {scenario.identifier for scenario in scenarios}
        unknown = sorted(requested - known)
        if unknown:
            raise CaptureGateError("unknown scenario ids: " + ", ".join(unknown))
        scenarios = [scenario for scenario in scenarios
                     if scenario.identifier in requested]
    aosp_capture = {"schemaVersion": 1, "scenarios": []}
    kairosboot_capture = {"schemaVersion": 1, "scenarios": []}
    for scenario in scenarios:
        try:
            if scenario.output_path is not None:
                scenario.output_path.unlink(missing_ok=True)
            aosp_capture["scenarios"].append(
                capture_scenario(
                    _aosp_command(verified.path, scenario),
                    scenario,
                    "official Fastboot",
                )
            )
            if scenario.output_path is not None:
                scenario.output_path.unlink(missing_ok=True)
            kairosboot_capture["scenarios"].append(
                capture_scenario(
                    _kairosboot_command(kairosboot, scenario),
                    scenario,
                    "KairosBoot",
                )
            )
        except CaptureGateError as error:
            raise CaptureGateError(
                f"scenario {scenario.identifier} failed: {error}"
            ) from error

    aosp_path = output_dir / "aosp-fastboot-normalized-trace.json"
    kairosboot_path = output_dir / "kairosboot-normalized-trace.json"
    metadata_path = output_dir / "official-capture-metadata.json"
    _write_json(aosp_path, aosp_capture)
    _write_json(kairosboot_path, kairosboot_capture)

    comparator = _load_comparator(repository_root)
    schema = repository_root / "tests" / "compat" / "normalized-fastboot-trace.schema.json"
    expected = comparator.load_capture(aosp_path, schema)
    actual = comparator.load_capture(kairosboot_path, schema)
    difference = comparator.compare_captures(expected, actual)
    if difference is not None:
        raise CaptureGateError(
            f"normalized differential mismatch at {difference.path}: "
            f"AOSP={difference.expected!r}, KairosBoot={difference.actual!r}; "
            f"captures: {aosp_path}, {kairosboot_path}"
        )
    _write_json(
        metadata_path,
        {
            "documentType": "kairosboot.official-fastboot-differential-evidence",
            "schemaVersion": 1,
            "result": "matched",
            "baseline": {
                "platformToolsVersion": platform_tools_version,
                "sourceCommit": source_commit,
            },
            "aospFastboot": {
                "platform": verified.platform_key,
                "sha256": verified.sha256,
                "versionOutput": verified.version_output.splitlines()[0],
            },
            "kairosboot": {
                "releaseArtifactSha256": kairosboot_artifact_sha256,
                "sha256": kairosboot_artifact_sha256,
                "sourceCommit": kairosboot_commit,
            },
            "captureFiles": {
                "aosp": {
                    "path": aosp_path.name,
                    "sha256": _sha256_file(aosp_path),
                },
                "kairosboot": {
                    "path": kairosboot_path.name,
                    "sha256": _sha256_file(kairosboot_path),
                },
            },
            "scenarios": [
                {
                    "id": scenario.identifier,
                    "transport": scenario.transport,
                    "coverageIds": list(scenario.coverage_ids),
                    "result": "matched",
                }
                for scenario in scenarios
            ],
            "uncoveredScenarios": list(UNCOVERED_SCENARIOS),
        },
    )
    print(
        "PASS: official Platform-Tools differential capture matched KairosBoot "
        f"for {len(scenarios)} host/TCP/UDP scenarios; evidence: {output_dir}"
    )
    if temporary is not None:
        temporary.cleanup()
    artifact_temporary.cleanup()
    return 0


def parse_arguments(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture and compare official Fastboot and KairosBoot wire traces"
    )
    parser.add_argument("--repository-root", type=pathlib.Path, required=True)
    parser.add_argument("--lock", type=pathlib.Path)
    parser.add_argument("--fastboot", type=pathlib.Path)
    parser.add_argument("--kairosboot", type=pathlib.Path)
    parser.add_argument("--output-dir", type=pathlib.Path)
    parser.add_argument(
        "--scenario", action="append", default=[],
        help="run only the named scenario (repeatable)",
    )
    parser.add_argument(
        "--require",
        action="store_true",
        help="fail instead of skip when either binary was not provided",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    try:
        return _run_capture(parse_arguments(argv))
    except CaptureGateError as error:
        print(f"official Fastboot differential gate failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
