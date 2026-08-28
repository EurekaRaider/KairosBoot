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
        self.finished = False
        self.device_state: dict[str, Any] = {"product": "product_a"}

    def handle(self, payload: bytes) -> bytes:
        if self.finished:
            raise CaptureGateError("Fastboot client sent bytes after terminal response")
        if self.expected_data_size is not None:
            expected = self.expected_data_size
            self.expected_data_size = None
            if len(payload) != expected:
                raise CaptureGateError(
                    f"download payload is {len(payload)} bytes, expected {expected}"
                )
            self.downloaded = payload
            self.events.append(
                {
                    "kind": "DATA",
                    "direction": "host-to-device",
                    "size": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }
            )
            self.events.append({"kind": "OKAY", "message": "downloaded"})
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
            message = values.get(name, "")
            self.events.append({"kind": "OKAY", "message": message})
            if command == self.scenario.terminal_command:
                self.finished = True
            return b"OKAY" + message.encode("ascii")

        self.events.append({"kind": "COMMAND", "command": command})
        if command.startswith("download:"):
            encoded_size = command[len("download:") :]
            if re.fullmatch(r"[0-9a-fA-F]{8}", encoded_size) is None:
                raise CaptureGateError(f"invalid download command: {command!r}")
            self.expected_data_size = int(encoded_size, 16)
            return b"DATA" + encoded_size.lower().encode("ascii")
        if command.startswith("flash:"):
            if self.downloaded is None:
                raise CaptureGateError("flash command arrived before a download")
            partition = command[len("flash:") :]
            self.device_state["partitions"] = {
                partition: {
                    "size": len(self.downloaded),
                    "sha256": hashlib.sha256(self.downloaded).hexdigest(),
                }
            }
            self.events.append({"kind": "OKAY", "message": "flashed"})
            if command == self.scenario.terminal_command:
                self.finished = True
            return b"OKAYflashed"
        if command == "signature":
            if self.downloaded is None:
                raise CaptureGateError("signature command arrived before a download")
            self.device_state["signature"] = {
                "size": len(self.downloaded),
                "sha256": hashlib.sha256(self.downloaded).hexdigest(),
            }
            self.events.append({"kind": "OKAY", "message": "accepted"})
            if command == self.scenario.terminal_command:
                self.finished = True
            return b"OKAYaccepted"
        if command.startswith("erase:"):
            partition = command[len("erase:") :]
            self.device_state["erased"] = [partition]
            self.events.append({"kind": "OKAY", "message": "erased"})
            if command == self.scenario.terminal_command:
                self.finished = True
            return b"OKAYerased"

        self.events.append({"kind": "FAIL", "message": "unsupported scripted command"})
        self.finished = True
        return b"FAILunsupported scripted command"

    def capture(self, exit_code: int) -> dict[str, Any]:
        if not self.finished:
            raise CaptureGateError(
                f"scenario {self.scenario.identifier} ended before its terminal command"
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
        process = subprocess.Popen(expanded, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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
                    _send_frame(connection, response)
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
        process = subprocess.Popen(expanded, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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
    if scenario.transport == "tcp":
        return _capture_tcp(command, scenario, label)
    if scenario.transport == "udp":
        return _capture_udp(command, scenario, label)
    raise CaptureGateError(f"unknown scenario transport: {scenario.transport}")


def _aosp_command(binary: pathlib.Path, scenario: Scenario) -> list[str]:
    return [str(binary), "-s", "{endpoint}", *scenario.arguments]


def _kairosboot_command(binary: pathlib.Path, scenario: Scenario) -> list[str]:
    return [
        str(binary),
        "--device",
        "{endpoint}",
        "--timeout-ms",
        "5000",
        *scenario.arguments,
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
    kairosboot = _require_executable(arguments.kairosboot, "KairosBoot")

    output_dir = arguments.output_dir
    temporary: Optional[tempfile.TemporaryDirectory[str]] = None
    if output_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix="kairosboot-aosp-capture-")
        output_dir = pathlib.Path(temporary.name)
    output_dir.mkdir(parents=True, exist_ok=True)
    image_path = output_dir / "differential-system.img"
    image_path.write_bytes(bytes(range(32)))
    signature_path = output_dir / "differential-signature.bin"
    signature_path.write_bytes(bytes(range(256)))

    scenarios = [
        Scenario("official-tcp-getvar", "tcp", ("getvar", "product"), "getvar:product"),
        Scenario("official-udp-getvar", "udp", ("getvar", "product"), "getvar:product"),
        Scenario(
            "official-tcp-flash",
            "tcp",
            ("flash", "system", str(image_path)),
            "flash:system",
            image_path,
        ),
        Scenario(
            "official-udp-flash",
            "udp",
            ("flash", "system", str(image_path)),
            "flash:system",
            image_path,
        ),
        Scenario(
            "official-tcp-signature",
            "tcp",
            ("signature", str(signature_path)),
            "signature",
            signature_path,
        ),
        Scenario(
            "official-udp-signature",
            "udp",
            ("signature", str(signature_path)),
            "signature",
            signature_path,
        ),
    ]
    aosp_capture = {"schemaVersion": 1, "scenarios": []}
    kairosboot_capture = {"schemaVersion": 1, "scenarios": []}
    for scenario in scenarios:
        try:
            aosp_capture["scenarios"].append(
                capture_scenario(
                    _aosp_command(verified.path, scenario),
                    scenario,
                    "official Fastboot",
                )
            )
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
    _write_json(
        metadata_path,
        {
            "aospFastboot": {
                "path": str(verified.path),
                "platform": verified.platform_key,
                "sha256": verified.sha256,
                "versionOutput": verified.version_output,
            },
            "kairosboot": {"path": str(kairosboot)},
            "scenarioIds": [scenario.identifier for scenario in scenarios],
        },
    )

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
    print(
        "PASS: official Platform-Tools differential capture matched KairosBoot "
        f"for {len(scenarios)} TCP/UDP scenarios; evidence: {output_dir}"
    )
    if temporary is not None:
        temporary.cleanup()
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
