#!/usr/bin/env python3
"""Apply the release HIL hard gates to lab-produced evidence."""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path


SHA = re.compile(r"^[0-9a-f]{40}$")
TOP_LEVEL_KEYS = {
    "schemaVersion",
    "commit",
    "deviceCount",
    "soakHours",
    "singleDeviceDownloadCeilingUtilization",
    "minimumJainFairness",
    "batchMakespanSpeedup",
    "controllers",
    "scenarios",
    "deadlocks",
    "transferLeaks",
    "handleLeaks",
    "deviceMisrouting",
    "sustainedRssGrowth",
}
SCENARIO_KEYS = {
    "id",
    "weight",
    "fastbootCeilingUtilization",
    "relativeDelta",
    "statisticallySignificant",
}
CONTROLLER_KEYS = {"id", "ceilingUtilization"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"HIL gate failed: {message}")


def finite_number(value: object) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
    )


def validate(data: object, expected_commit: str) -> None:
    require(isinstance(data, dict), "evidence must be a JSON object")
    require(set(data) == TOP_LEVEL_KEYS, "unexpected or missing top-level fields")
    require(SHA.fullmatch(expected_commit) is not None, "invalid expected commit")

    require(data.get("schemaVersion") == 1, "unsupported evidence schema")
    commit = data.get("commit")
    require(isinstance(commit, str) and SHA.fullmatch(commit) is not None, "invalid commit")
    require(commit == expected_commit, "evidence is not for the workflow commit")
    device_count = data.get("deviceCount")
    require(
        isinstance(device_count, int) and not isinstance(device_count, bool),
        "deviceCount must be an integer",
    )
    require(device_count >= 32, "fewer than 32 devices")
    soak_hours = data.get("soakHours")
    require(finite_number(soak_hours) and soak_hours >= 24, "soak shorter than 24 hours")
    single_device_utilization = data.get("singleDeviceDownloadCeilingUtilization")
    require(
        finite_number(single_device_utilization)
        and 0.90 <= single_device_utilization <= 1.10,
        "single-device download is below 90% of raw bulk ceiling",
    )
    fairness = data.get("minimumJainFairness")
    require(
        finite_number(fairness) and 0.95 <= fairness <= 1.0,
        "Jain fairness below 0.95",
    )
    speedup = data.get("batchMakespanSpeedup")
    require(
        finite_number(speedup) and speedup >= 1.10,
        "32-device makespan speedup below 10%",
    )
    scenarios = data.get("scenarios")
    require(isinstance(scenarios, list) and scenarios, "no performance scenarios")
    headroom_weighted_logs = []
    scenario_ids = set()
    for scenario in scenarios:
        require(isinstance(scenario, dict), "scenario must be an object")
        require(set(scenario) == SCENARIO_KEYS, "unexpected or missing scenario fields")
        identifier = scenario.get("id")
        require(isinstance(identifier, str) and identifier, "scenario id must not be empty")
        require(identifier not in scenario_ids, f"duplicate scenario id: {identifier}")
        scenario_ids.add(identifier)
        weight = scenario.get("weight")
        ceiling = scenario.get("fastbootCeilingUtilization")
        delta = scenario.get("relativeDelta")
        require(finite_number(weight) and weight > 0, f"invalid weight in {identifier}")
        require(
            finite_number(ceiling) and 0 <= ceiling <= 1.10,
            f"invalid Fastboot ceiling utilization in {identifier}",
        )
        require(finite_number(delta) and delta > -1, f"invalid relative delta in {identifier}")
        significant_value = scenario.get("statisticallySignificant")
        require(isinstance(significant_value, bool), f"invalid significance in {identifier}")
        if ceiling <= 0.90:
            headroom_weighted_logs.append((weight, math.log(1 + delta)))
        elif significant_value:
            require(delta >= -0.03, f"significant regression in {identifier}")
    if headroom_weighted_logs:
        total_weight = sum(weight for weight, _ in headroom_weighted_logs)
        geometric_mean = math.exp(
            sum(weight * logarithm for weight, logarithm in headroom_weighted_logs)
            / total_weight
        )
        require(geometric_mean >= 1.10, "host-bound weighted geometric speedup below 10%")
    controllers = data.get("controllers")
    require(isinstance(controllers, list) and controllers, "no USB controller evidence")
    controller_ids = set()
    for controller in controllers:
        require(isinstance(controller, dict), "controller must be an object")
        require(set(controller) == CONTROLLER_KEYS, "unexpected or missing controller fields")
        identifier = controller.get("id")
        utilization = controller.get("ceilingUtilization")
        require(isinstance(identifier, str) and identifier, "controller id must not be empty")
        require(identifier not in controller_ids, f"duplicate controller id: {identifier}")
        controller_ids.add(identifier)
        require(
            finite_number(utilization) and 0.90 <= utilization <= 1.10,
            f"controller below 90% ceiling: {identifier}",
        )
    for field in ("deadlocks", "transferLeaks", "handleLeaks", "deviceMisrouting"):
        require(
            isinstance(data.get(field), int)
            and not isinstance(data.get(field), bool)
            and data.get(field) == 0,
            f"{field} is non-zero",
        )
    require(data.get("sustainedRssGrowth") is False, "sustained RSS growth detected")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    data = json.loads(args.evidence.read_text(encoding="utf-8"))
    validate(data, args.commit)
    print("HIL evidence satisfies the KairosBoot v1 release gates")


if __name__ == "__main__":
    main()
