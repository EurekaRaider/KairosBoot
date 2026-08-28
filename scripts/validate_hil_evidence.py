#!/usr/bin/env python3
"""Apply the release HIL hard gates to lab-produced raw evidence."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from performance_evidence import (  # noqa: E402
    GIT_SHA,
    RUN_ID,
    require,
    require_exact_keys,
    require_sha256,
    require_string,
    require_utc,
    validate_benchmark,
)


HIL_KEYS = {
    "schemaVersion",
    "evidenceKind",
    "commit",
    "runId",
    "lab",
    "benchmark",
    "soak",
}
LAB_KEYS = {"id", "operatorIdHash"}
SOAK_KEYS = {
    "requestedDurationSeconds",
    "completedDurationSeconds",
    "startedAt",
    "finishedAt",
    "sampleIntervalSeconds",
    "cycles",
    "deadlocks",
    "transferLeaks",
    "handleLeaks",
    "deviceMisrouting",
    "sustainedRssGrowth",
}


def finite_nonnegative(value: object) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value >= 0
    )


def validate(
    data: object,
    expected_commit: str,
    minimum_soak_hours: float = 24.0,
) -> dict[str, float]:
    require(
        math.isfinite(minimum_soak_hours) and minimum_soak_hours > 0,
        "minimum soak duration must be positive",
    )
    document = require_exact_keys(data, HIL_KEYS, "HIL evidence")
    require(GIT_SHA.fullmatch(expected_commit) is not None, "invalid expected commit")
    require(document["schemaVersion"] == 1, "unsupported HIL evidence schema")
    require(document["evidenceKind"] == "hardware", "HIL evidence is not hardware evidence")
    require(document["commit"] == expected_commit, "HIL evidence commit does not match")
    require(
        isinstance(document["runId"], str)
        and RUN_ID.fullmatch(document["runId"]) is not None,
        "invalid HIL runId",
    )

    lab = require_exact_keys(document["lab"], LAB_KEYS, "lab")
    require_string(lab["id"], "lab id")
    require_sha256(lab["operatorIdHash"], "lab operatorIdHash")

    benchmark = document["benchmark"]
    metrics = validate_benchmark(benchmark, expected_commit)
    require(isinstance(benchmark, dict), "benchmark must be an object")
    require(benchmark["runId"] == document["runId"], "benchmark runId differs from HIL runId")

    soak = require_exact_keys(document["soak"], SOAK_KEYS, "soak")
    minimum_seconds = minimum_soak_hours * 3600.0
    requested = soak["requestedDurationSeconds"]
    completed = soak["completedDurationSeconds"]
    interval = soak["sampleIntervalSeconds"]
    require(finite_nonnegative(requested) and requested + 1e-9 >= minimum_seconds,
            "requested soak duration is below the required duration")
    require(finite_nonnegative(completed) and completed + 1e-9 >= minimum_seconds,
            "completed soak duration is below the required duration")
    require(finite_nonnegative(interval) and interval > 0, "invalid soak sample interval")
    started = require_utc(soak["startedAt"], "soak startedAt")
    finished = require_utc(soak["finishedAt"], "soak finishedAt")
    elapsed = (finished - started).total_seconds()
    require(elapsed + 1e-9 >= minimum_seconds, "soak timestamps are shorter than required")
    require(abs(elapsed - float(completed)) <= max(float(interval), 1.0),
            "soak completed duration disagrees with timestamps")
    require(
        isinstance(soak["cycles"], int)
        and not isinstance(soak["cycles"], bool)
        and soak["cycles"] > 0,
        "soak has no completed cycles",
    )
    for field in ("deadlocks", "transferLeaks", "handleLeaks", "deviceMisrouting"):
        require(
            isinstance(soak[field], int)
            and not isinstance(soak[field], bool)
            and soak[field] == 0,
            f"soak {field} is non-zero",
        )
    require(soak["sustainedRssGrowth"] is False, "sustained RSS growth detected")
    return metrics


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument(
        "--minimum-soak-hours",
        type=float,
        default=24.0,
        help="qualification minimum (default: 24; short values are for harness tests only)",
    )
    args = parser.parse_args()
    data = json.loads(args.evidence.read_text(encoding="utf-8"))
    metrics = validate(data, args.commit, args.minimum_soak_hours)
    print(
        "HIL evidence satisfies the KairosBoot v1 release gates "
        f"(single={metrics['singleDeviceDownloadCeilingUtilization']:.3f}, "
        f"fleet={metrics['batchMakespanSpeedup']:.3f}x, "
        f"fairness={metrics['minimumJainFairness']:.3f})"
    )


if __name__ == "__main__":
    main()
