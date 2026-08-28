#!/usr/bin/env python3
"""Deterministic calculations for KairosBoot performance evidence."""

from __future__ import annotations

import datetime
import math
import re
import statistics


SHA256 = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")
RUN_ID = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"
)
UTC_DATE_TIME = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?Z$"
)
SUPPORTED_HOSTS = {"windows", "linux", "macos"}
SUPPORTED_ARCHES = {"x64", "arm64"}
BENCHMARK_KEYS = {
    "schemaVersion",
    "evidenceKind",
    "commit",
    "runId",
    "recordedAt",
    "harness",
    "build",
    "host",
    "devices",
    "rawBulkCeiling",
    "singleDeviceDownload",
    "scenarios",
    "fleet",
}
HARNESS_KEYS = {"name", "version", "sourceSha256"}
BUILD_KEYS = {
    "configuration",
    "kairosbootVersion",
    "kairosbootBinarySha256",
    "aospFastbootVersion",
    "aospFastbootBinarySha256",
    "libusbVersion",
}
HOST_KEYS = {"os", "arch", "machineIdHash"}
DEVICE_KEYS = {"idHash", "product", "usbPath", "controllerId"}
SINGLE_KEYS = {
    "deviceIdHash",
    "kairosbootSamplesBytesPerSecond",
    "aospFastbootSamplesBytesPerSecond",
}
RAW_CEILING_KEYS = {"deviceIdHash", "samplesBytesPerSecond"}
SCENARIO_KEYS = {
    "id",
    "weight",
    "kairosbootSamplesBytesPerSecond",
    "aospFastbootSamplesBytesPerSecond",
}
FLEET_KEYS = {
    "aospProcessMakespanSamplesMilliseconds",
    "kairosbootMakespanSamplesMilliseconds",
    "controllers",
    "fairnessWindows",
}
CONTROLLER_KEYS = {
    "id",
    "rawBulkCeilingSamplesBytesPerSecond",
    "kairosbootAggregateSamplesBytesPerSecond",
}
WINDOW_KEYS = {"durationMilliseconds", "devices"}
WINDOW_DEVICE_KEYS = {"idHash", "bytesPerSecond"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"performance gate failed: {message}")


def require_exact_keys(value: object, keys: set[str], name: str) -> dict[str, object]:
    require(isinstance(value, dict), f"{name} must be an object")
    require(set(value) == keys, f"unexpected or missing fields in {name}")
    return value


def require_string(value: object, name: str) -> str:
    require(isinstance(value, str) and bool(value), f"{name} must not be empty")
    return value


def require_sha256(value: object, name: str) -> str:
    require(isinstance(value, str) and SHA256.fullmatch(value) is not None,
            f"{name} must be a lowercase SHA-256")
    return value


def require_utc(value: object, name: str) -> datetime.datetime:
    require(isinstance(value, str), f"{name} must be a UTC date-time")
    match = UTC_DATE_TIME.fullmatch(value)
    require(match is not None, f"{name} must be a UTC date-time")
    try:
        return datetime.datetime(
            int(match.group(1)), int(match.group(2)), int(match.group(3)),
            int(match.group(4)), int(match.group(5)), int(match.group(6)),
            int((match.group(7) or "0").ljust(6, "0")[:6]),
            tzinfo=datetime.timezone.utc,
        )
    except ValueError as error:
        raise SystemExit(f"performance gate failed: {name} is invalid") from error


def require_positive_samples(value: object, name: str) -> list[float]:
    require(isinstance(value, list) and len(value) >= 5,
            f"{name} must contain at least five samples")
    samples: list[float] = []
    for sample in value:
        require(
            isinstance(sample, (int, float))
            and not isinstance(sample, bool)
            and math.isfinite(sample)
            and sample > 0,
            f"{name} contains an invalid sample",
        )
        samples.append(float(sample))
    return samples


def median(samples: object, name: str) -> float:
    return float(statistics.median(require_positive_samples(samples, name)))


def paired_sign_test_p_value(left: list[float], right: list[float]) -> float:
    """Return the exact two-sided sign-test p-value, dropping tied pairs."""

    require(len(left) == len(right), "paired scenario sample counts differ")
    positive = sum(first > second for first, second in zip(left, right))
    negative = sum(first < second for first, second in zip(left, right))
    sample_count = positive + negative
    if sample_count == 0:
        return 1.0
    tail = min(positive, negative)
    probability = sum(math.comb(sample_count, index) for index in range(tail + 1))
    return min(1.0, 2.0 * probability / (2**sample_count))


def jain_fairness(rates: list[float]) -> float:
    require(bool(rates), "fairness window has no devices")
    square_sum = sum(rate * rate for rate in rates)
    require(square_sum > 0, "fairness window has no transferred bytes")
    return (sum(rates) ** 2) / (len(rates) * square_sum)


def validate_benchmark(data: object, expected_commit: str) -> dict[str, float]:
    """Validate raw benchmark evidence and return reproducible derived metrics."""

    document = require_exact_keys(data, BENCHMARK_KEYS, "benchmark evidence")
    require(GIT_SHA.fullmatch(expected_commit) is not None, "invalid expected commit")
    require(document["schemaVersion"] == 1, "unsupported benchmark schema")
    require(document["evidenceKind"] == "hardware", "benchmark is not hardware evidence")
    require(document["commit"] == expected_commit, "benchmark commit does not match")
    require(
        isinstance(document["runId"], str)
        and RUN_ID.fullmatch(document["runId"]) is not None,
        "invalid benchmark runId",
    )
    require_utc(document["recordedAt"], "benchmark recordedAt")

    harness = require_exact_keys(document["harness"], HARNESS_KEYS, "harness")
    require_string(harness["name"], "harness name")
    require_string(harness["version"], "harness version")
    require_sha256(harness["sourceSha256"], "harness sourceSha256")

    build = require_exact_keys(document["build"], BUILD_KEYS, "build")
    require(build["configuration"] == "Release", "KairosBoot was not built in Release mode")
    require_string(build["kairosbootVersion"], "KairosBoot version")
    require_sha256(build["kairosbootBinarySha256"], "KairosBoot binary hash")
    require_string(build["aospFastbootVersion"], "AOSP Fastboot version")
    require_sha256(build["aospFastbootBinarySha256"], "AOSP Fastboot binary hash")
    require(build["libusbVersion"] == "1.0.30", "libusb version is not 1.0.30")

    host = require_exact_keys(document["host"], HOST_KEYS, "host")
    require(host["os"] in SUPPORTED_HOSTS, "unsupported host OS")
    require(host["arch"] in SUPPORTED_ARCHES, "unsupported host architecture")
    require_sha256(host["machineIdHash"], "host machineIdHash")

    devices = document["devices"]
    require(isinstance(devices, list) and len(devices) >= 32,
            "benchmark requires at least 32 devices")
    device_ids: set[str] = set()
    usb_paths: set[str] = set()
    device_controllers: set[str] = set()
    for index, raw_device in enumerate(devices):
        device = require_exact_keys(raw_device, DEVICE_KEYS, f"device {index}")
        identifier = require_sha256(device["idHash"], f"device {index} idHash")
        require(identifier not in device_ids, "duplicate device identity")
        device_ids.add(identifier)
        require_string(device["product"], f"device {index} product")
        usb_path = require_string(device["usbPath"], f"device {index} usbPath")
        require(usb_path not in usb_paths, "duplicate physical USB path")
        usb_paths.add(usb_path)
        device_controllers.add(require_string(
            device["controllerId"], f"device {index} controllerId"
        ))

    raw_ceiling = require_exact_keys(
        document["rawBulkCeiling"], RAW_CEILING_KEYS, "raw bulk ceiling"
    )
    raw_device_id = require_sha256(raw_ceiling["deviceIdHash"], "raw ceiling device")
    require(raw_device_id in device_ids, "raw ceiling device is not in the inventory")
    raw_median = median(raw_ceiling["samplesBytesPerSecond"], "raw bulk ceiling")

    single = require_exact_keys(
        document["singleDeviceDownload"], SINGLE_KEYS, "single-device download"
    )
    require(single["deviceIdHash"] == raw_device_id,
            "single-device and raw ceiling devices differ")
    single_kairosboot = median(
        single["kairosbootSamplesBytesPerSecond"], "single-device KairosBoot"
    )
    median(single["aospFastbootSamplesBytesPerSecond"], "single-device AOSP Fastboot")
    single_utilization = single_kairosboot / raw_median
    require(single_utilization <= 1.10, "single-device result exceeds raw ceiling sanity limit")
    require(single_utilization + 1e-12 >= 0.90,
            "single-device download is below 90% of raw bulk ceiling")

    scenarios = document["scenarios"]
    require(isinstance(scenarios, list) and bool(scenarios), "no performance scenarios")
    scenario_ids: set[str] = set()
    headroom_weighted_logs: list[tuple[int, float]] = []
    for index, raw_scenario in enumerate(scenarios):
        scenario = require_exact_keys(raw_scenario, SCENARIO_KEYS, f"scenario {index}")
        identifier = require_string(scenario["id"], f"scenario {index} id")
        require(identifier not in scenario_ids, f"duplicate scenario id: {identifier}")
        scenario_ids.add(identifier)
        weight = scenario["weight"]
        require(isinstance(weight, int) and not isinstance(weight, bool) and weight > 0,
                f"invalid weight in {identifier}")
        kairosboot_samples = require_positive_samples(
            scenario["kairosbootSamplesBytesPerSecond"], f"{identifier} KairosBoot"
        )
        aosp_samples = require_positive_samples(
            scenario["aospFastbootSamplesBytesPerSecond"], f"{identifier} AOSP Fastboot"
        )
        require(len(kairosboot_samples) == len(aosp_samples),
                f"paired sample counts differ in {identifier}")
        kairosboot_median = float(statistics.median(kairosboot_samples))
        aosp_median = float(statistics.median(aosp_samples))
        aosp_utilization = aosp_median / raw_median
        require(aosp_utilization <= 1.10,
                f"AOSP result exceeds raw ceiling sanity limit in {identifier}")
        relative_delta = kairosboot_median / aosp_median - 1.0
        p_value = paired_sign_test_p_value(kairosboot_samples, aosp_samples)
        if aosp_utilization <= 0.90 + 1e-12:
            headroom_weighted_logs.append((weight, math.log1p(relative_delta)))
        elif p_value <= 0.05 + 1e-12:
            require(relative_delta + 1e-12 >= -0.03,
                    f"statistically significant regression exceeds 3% in {identifier}")
    if headroom_weighted_logs:
        total_weight = sum(weight for weight, _ in headroom_weighted_logs)
        speedup = math.exp(
            sum(weight * logarithm for weight, logarithm in headroom_weighted_logs)
            / total_weight
        )
        require(speedup + 1e-12 >= 1.10,
                "host-bound weighted geometric speedup is below 10%")

    fleet = require_exact_keys(document["fleet"], FLEET_KEYS, "fleet")
    aosp_makespan = median(
        fleet["aospProcessMakespanSamplesMilliseconds"], "AOSP fleet makespan"
    )
    kairosboot_makespan = median(
        fleet["kairosbootMakespanSamplesMilliseconds"], "KairosBoot fleet makespan"
    )
    makespan_speedup = aosp_makespan / kairosboot_makespan
    require(makespan_speedup + 1e-12 >= 1.10,
            "32-device makespan speedup is below 10%")

    controllers = fleet["controllers"]
    require(isinstance(controllers, list) and bool(controllers),
            "no controller aggregate evidence")
    controller_ids: set[str] = set()
    for index, raw_controller in enumerate(controllers):
        controller = require_exact_keys(
            raw_controller, CONTROLLER_KEYS, f"controller {index}"
        )
        identifier = require_string(controller["id"], f"controller {index} id")
        require(identifier not in controller_ids, f"duplicate controller: {identifier}")
        controller_ids.add(identifier)
        controller_ceiling = median(
            controller["rawBulkCeilingSamplesBytesPerSecond"],
            f"{identifier} raw aggregate ceiling",
        )
        aggregate = median(
            controller["kairosbootAggregateSamplesBytesPerSecond"],
            f"{identifier} KairosBoot aggregate",
        )
        utilization = aggregate / controller_ceiling
        require(utilization <= 1.10, f"controller exceeds ceiling sanity limit: {identifier}")
        require(utilization + 1e-12 >= 0.90,
                f"controller below 90% aggregate ceiling: {identifier}")
    require(controller_ids == device_controllers,
            "controller evidence does not match the device inventory")

    windows = fleet["fairnessWindows"]
    require(isinstance(windows, list) and bool(windows), "no fairness windows")
    minimum_fairness = 1.0
    for index, raw_window in enumerate(windows):
        window = require_exact_keys(raw_window, WINDOW_KEYS, f"fairness window {index}")
        require(window["durationMilliseconds"] == 5000,
                f"fairness window {index} is not five seconds")
        rates = window["devices"]
        require(isinstance(rates, list) and len(rates) == len(device_ids),
                f"fairness window {index} does not cover every device")
        seen: set[str] = set()
        numeric_rates: list[float] = []
        for raw_rate in rates:
            rate = require_exact_keys(raw_rate, WINDOW_DEVICE_KEYS,
                                      f"fairness window {index} device")
            identifier = require_sha256(rate["idHash"], "fairness device idHash")
            require(identifier in device_ids and identifier not in seen,
                    f"fairness window {index} has an invalid device set")
            seen.add(identifier)
            value = rate["bytesPerSecond"]
            require(
                isinstance(value, (int, float))
                and not isinstance(value, bool)
                and math.isfinite(value)
                and value > 0,
                "fairness throughput is invalid",
            )
            numeric_rates.append(float(value))
        fairness = jain_fairness(numeric_rates)
        minimum_fairness = min(minimum_fairness, fairness)
        require(fairness + 1e-12 >= 0.95,
                f"Jain fairness below 0.95 in window {index}")

    return {
        "singleDeviceDownloadCeilingUtilization": single_utilization,
        "batchMakespanSpeedup": makespan_speedup,
        "minimumJainFairness": minimum_fairness,
    }
