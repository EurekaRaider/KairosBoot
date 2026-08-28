#!/usr/bin/env python3
"""Run a lab-owned device cycle command for a bounded soak duration."""

from __future__ import annotations

import argparse
import datetime
import json
import math
import subprocess
import time
from pathlib import Path
from typing import Callable, Sequence


Invoke = Callable[[Sequence[str]], int]


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def invoke_command(command: Sequence[str]) -> int:
    return subprocess.run(command, check=False).returncode


def run_soak(
    command: Sequence[str],
    duration_seconds: float,
    interval_seconds: float,
    *,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
    invoke: Invoke = invoke_command,
    wall_time: Callable[[], str] = utc_now,
) -> dict[str, object]:
    if not command:
        raise ValueError("device cycle command must not be empty")
    if not math.isfinite(duration_seconds) or duration_seconds <= 0:
        raise ValueError("duration must be positive")
    if not math.isfinite(interval_seconds) or interval_seconds <= 0:
        raise ValueError("sample interval must be positive")

    started_at = wall_time()
    started = monotonic()
    cycles = 0
    failures = 0
    exit_codes: list[int] = []
    while monotonic() - started < duration_seconds:
        exit_code = invoke(command)
        cycles += 1
        failures += exit_code != 0
        exit_codes.append(exit_code)
        remaining = duration_seconds - (monotonic() - started)
        if remaining > 0:
            sleep(min(interval_seconds, remaining))

    return {
        "schemaVersion": 1,
        "observationKind": "raw-soak-observation",
        "qualificationStatus": "raw-observation-only",
        "requestedDurationSeconds": duration_seconds,
        "completedDurationSeconds": monotonic() - started,
        "sampleIntervalSeconds": interval_seconds,
        "startedAt": started_at,
        "finishedAt": wall_time(),
        "cycles": cycles,
        "cycleFailures": failures,
        "exitCodes": exit_codes,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Collect raw soak-cycle observations. This does not create or pass HIL evidence."
        )
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=float, default=24 * 60 * 60)
    parser.add_argument("--interval-seconds", type=float, default=60.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    try:
        observation = run_soak(
            command,
            args.duration_seconds,
            args.interval_seconds,
        )
    except ValueError as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(observation, allow_nan=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"raw soak observation written to {args.output}; "
        "it is not qualification evidence"
    )


if __name__ == "__main__":
    main()
