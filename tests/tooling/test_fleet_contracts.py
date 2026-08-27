# SPDX-License-Identifier: MIT
"""Validate the frozen Fleet plan/report schemas and canonical examples."""

from __future__ import annotations

import copy
import datetime
import decimal
import hashlib
import json
import pathlib
import re

from json_schema_subset import (
    InstanceValidationError,
    SchemaDefinitionError,
    check_schema,
    validate,
)

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMAS = ROOT / "schemas"
CONTRACTS = ROOT / "tests" / "contracts"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
UTC_DATE_TIME = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?Z$"
)

STATUS_CODES = {
    1: "invalid_argument",
    2: "out_of_memory",
    3: "not_supported",
    4: "no_device",
    5: "ambiguous_device",
    6: "busy",
    7: "timeout",
    8: "cancelled",
    9: "io",
    10: "internal",
    11: "protocol",
    12: "device_fail",
}
STATUS_ENUM_NAMES = {
    "KB_E_INVALID_ARGUMENT": 1,
    "KB_E_OUT_OF_MEMORY": 2,
    "KB_E_NOT_SUPPORTED": 3,
    "KB_E_NO_DEVICE": 4,
    "KB_E_AMBIGUOUS_DEVICE": 5,
    "KB_E_BUSY": 6,
    "KB_E_TIMEOUT": 7,
    "KB_E_CANCELLED": 8,
    "KB_E_IO": 9,
    "KB_E_INTERNAL": 10,
    "KB_E_PROTOCOL": 11,
    "KB_E_DEVICE_FAIL": 12,
}

PLAN_KEYS = {
    "schemaVersion",
    "manifestApiVersion",
    "kind",
    "manifestSha256",
    "artifacts",
    "targets",
    "policy",
}
PLAN_STEP_KEYS = {
    "index",
    "operation",
    "partition",
    "artifact",
    "slot",
    "rebootTarget",
    "oemCommand",
}
REPORT_KEYS = {
    "schemaVersion",
    "jobId",
    "planSha256",
    "state",
    "startedAt",
    "finishedAt",
    "devices",
    "summary",
    "error",
}
REPORT_STEP_KEYS = PLAN_STEP_KEYS | {
    "state",
    "startedAt",
    "finishedAt",
    "bytesTotal",
    "bytesTransferred",
    "error",
}
ERROR_KEYS = {
    "code",
    "status",
    "message",
    "deviceIdentifier",
    "nativeCode",
    "transferCertainty",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_keys(value: dict[str, object], expected: set[str], name: str) -> None:
    require(set(value) == expected, f"{name} keys differ: {set(value) ^ expected}")


def load_json(path: pathlib.Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(value, dict), f"{path.name} is not an object")
    return value


def load_canonical(path: pathlib.Path) -> dict[str, object]:
    raw = path.read_text(encoding="utf-8")
    value = json.loads(
        raw,
        parse_constant=lambda value: (_ for _ in ()).throw(
            ValueError(f"non-I-JSON constant {value}")
        ),
    )
    expected = json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ) + "\n"
    require(raw == expected, f"{path.name} is not canonical UTF-8 JSON")
    require(isinstance(value, dict), f"{path.name} is not an object")
    return value


def canonical_json_bytes(value: object) -> bytes:
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def canonical_plan_sha(plan: dict[str, object]) -> str:
    return hashlib.sha256(canonical_json_bytes(plan)).hexdigest()


def validate_canonical_json_profile() -> None:
    value = {
        "z": "雪/😀\u2028",
        "a": "\"\\\b\f\n\r\t\x00\x1f",
    }
    expected = (
        r'{"a":"\"\\\b\f\n\r\t\u0000\u001f","z":"'
        + "雪/😀\u2028"
        + '"}'
    ).encode("utf-8")
    require(canonical_json_bytes(value) == expected,
            "canonical JSON string escaping profile")


def require_sha256(value: object, name: str) -> None:
    require(isinstance(value, str) and SHA256.fullmatch(value) is not None,
            f"{name} is not a lowercase SHA-256")


def parse_utc(
    value: object, name: str
) -> tuple[datetime.datetime, decimal.Decimal]:
    require(isinstance(value, str), f"{name} is not UTC")
    match = UTC_DATE_TIME.fullmatch(value)
    require(match is not None, f"{name} is not a UTC date-time")
    try:
        parsed = datetime.datetime(
            int(match.group(1)), int(match.group(2)), int(match.group(3)),
            int(match.group(4)), int(match.group(5)), int(match.group(6)),
            tzinfo=datetime.timezone.utc,
        )
    except ValueError as error:
        raise AssertionError(f"{name} is not a date-time") from error
    fraction = decimal.Decimal(f"0.{match.group(7) or '0'}")
    return parsed, fraction


def validate_operation_fields(step: dict[str, object], name: str) -> None:
    operation = step["operation"]
    require(operation in {"flash", "erase", "set_active", "reboot", "oem"},
            f"{name} operation is invalid")
    partition = step["partition"]
    artifact = step["artifact"]
    slot = step["slot"]
    reboot_target = step["rebootTarget"]
    oem_command = step["oemCommand"]

    if operation == "flash":
        require(isinstance(partition, str) and partition, f"{name} partition")
        require(isinstance(artifact, str) and artifact, f"{name} artifact")
        require(slot in {None, "current", "other", "all", "a", "b"},
                f"{name} slot")
        require(reboot_target is None and oem_command is None, name)
    elif operation == "erase":
        require(isinstance(partition, str) and partition, f"{name} partition")
        require(artifact is None and slot is None and reboot_target is None
                and oem_command is None, name)
    elif operation == "set_active":
        require(partition is None and artifact is None, name)
        require(slot in {"other", "a", "b"}, f"{name} slot")
        require(reboot_target is None and oem_command is None, name)
    elif operation == "reboot":
        require(partition is None and artifact is None and slot is None, name)
        require(reboot_target in {"system", "bootloader", "recovery", "fastboot"},
                f"{name} reboot target")
        require(oem_command is None, name)
    else:
        require(partition is None and artifact is None and slot is None
                and reboot_target is None, name)
        require(isinstance(oem_command, str) and oem_command,
                f"{name} OEM command")


def validate_error(value: object, name: str) -> None:
    require(isinstance(value, dict), f"{name} is not an error object")
    require_keys(value, ERROR_KEYS, name)
    code = value["code"]
    require(isinstance(code, int) and not isinstance(code, bool)
            and code in STATUS_CODES, f"{name} code")
    require(value["status"] == STATUS_CODES[code], f"{name} status/code mismatch")
    require(isinstance(value["message"], str), f"{name} message")
    require(value["deviceIdentifier"] is None
            or isinstance(value["deviceIdentifier"], str), name)
    require(value["nativeCode"] is None
            or (isinstance(value["nativeCode"], int)
                and not isinstance(value["nativeCode"], bool)), name)
    require(value["transferCertainty"] in {
        None, "not_sent", "partial_or_unknown", "fully_transferred"
    }, f"{name} transfer certainty")


def validate_plan(plan: dict[str, object]) -> None:
    require_keys(plan, PLAN_KEYS, "plan")
    require(plan["schemaVersion"] == 1, "plan schemaVersion")
    require(plan["manifestApiVersion"] == "kairosboot.io/v1", "manifest API")
    require(plan["kind"] == "FlashJob", "plan kind")
    require_sha256(plan["manifestSha256"], "manifestSha256")

    artifacts = plan["artifacts"]
    require(isinstance(artifacts, list) and artifacts, "plan artifacts")
    artifact_ids: set[str] = set()
    for index, artifact in enumerate(artifacts):
        require(isinstance(artifact, dict), "artifact object")
        require_keys(artifact, {"index", "id", "path", "sha256"}, "artifact")
        require(artifact["index"] == index, "artifact index")
        require_sha256(artifact["sha256"], "artifact sha256")
        require(artifact["id"] not in artifact_ids, "duplicate artifact id")
        artifact_ids.add(artifact["id"])

    targets = plan["targets"]
    require(isinstance(targets, list) and targets, "plan targets")
    target_names: set[str] = set()
    selector_owners: dict[str, int] = {}
    for target_index, target in enumerate(targets):
        require(isinstance(target, dict), "target object")
        require_keys(target, {"index", "name", "selector", "expectedProduct", "steps"},
                     "target")
        require(target["index"] == target_index, "target index")
        require(target["name"] not in target_names, "duplicate target name")
        target_names.add(target["name"])
        selector = target["selector"]
        require(isinstance(selector, dict), "selector object")
        require_keys(selector, {"serials", "usbPaths"}, "selector")
        require(bool(selector["serials"] or selector["usbPaths"]), "empty selector")
        serials = set(selector["serials"])
        usb_paths = set(selector["usbPaths"])
        for selector_value in serials | usb_paths:
            owner = selector_owners.get(selector_value)
            require(owner is None or owner == target_index,
                    "selector value is owned by multiple targets")
            selector_owners[selector_value] = target_index
        steps = target["steps"]
        require(isinstance(steps, list) and steps, "target steps")
        for step_index, step in enumerate(steps):
            require(isinstance(step, dict), "plan step object")
            require_keys(step, PLAN_STEP_KEYS, "plan step")
            require(step["index"] == step_index, "plan step index")
            validate_operation_fields(step, "plan step")
            if step["operation"] == "flash":
                require(step["artifact"] in artifact_ids,
                        "flash references unknown artifact")

    policy = plan["policy"]
    require(isinstance(policy, dict), "policy object")
    require_keys(policy, {"onDeviceFailure", "maxParallelDevices", "memoryBudget"},
                 "policy")


def validate_device_step_sequence(state: str, steps: list[str]) -> None:
    require(bool(steps), "device has no steps")
    if state == "pending":
        require(all(value == "pending" for value in steps),
                "pending device has started steps")
    elif state == "running":
        require(steps.count("running") == 1, "running device has multiple active steps")
        active = steps.index("running")
        require(all(value == "succeeded" for value in steps[:active])
                and all(value == "pending" for value in steps[active + 1:]),
                "running device does not have a serial step prefix")
    elif state == "succeeded":
        require(all(value == "succeeded" for value in steps),
                "succeeded device has incomplete steps")
    elif state == "failed":
        if all(value == "skipped" for value in steps):
            return
        require(steps.count("failed") == 1, "failed device has multiple failure points")
        failed = steps.index("failed")
        require(all(value == "succeeded" for value in steps[:failed])
                and all(value == "skipped" for value in steps[failed + 1:]),
                "failed device does not have a serial step prefix")
    elif state == "cancelled":
        require(steps.count("cancelled") == 1,
                "cancelled device has multiple cancellation points")
        cancelled = steps.index("cancelled")
        require(all(value == "succeeded" for value in steps[:cancelled])
                and all(value == "skipped" for value in steps[cancelled + 1:]),
                "cancelled device does not have a serial step prefix")
    else:
        require(all(value == "skipped" for value in steps),
                "skipped device has active steps")


def validate_report(report: dict[str, object]) -> None:
    require_keys(report, REPORT_KEYS, "report")
    require(report["schemaVersion"] == 1, "report schemaVersion")
    require_sha256(report["planSha256"], "planSha256")
    require(report["state"] in {
        "running", "succeeded", "partially_failed", "failed", "cancelled"
    }, "report state")
    started_at = parse_utc(report["startedAt"], "report startedAt")
    finished_at = (None if report["finishedAt"] is None
                   else parse_utc(report["finishedAt"], "report finishedAt"))
    if finished_at is not None:
        require(started_at <= finished_at, "report finishes before it starts")
    if report["error"] is not None:
        validate_error(report["error"], "report error")

    devices = report["devices"]
    require(isinstance(devices, list), "report devices")
    identifiers: set[str] = set()
    counts = {state: 0 for state in
              ("pending", "running", "succeeded", "failed", "cancelled", "skipped")}
    for device in devices:
        require(isinstance(device, dict), "device object")
        require_keys(device, {
            "identifier", "serial", "usbPath", "target", "observedProduct",
            "state", "steps", "error"
        }, "device")
        state = device["state"]
        require(device["identifier"] not in identifiers,
                "duplicate device identifier")
        identifiers.add(device["identifier"])
        require(state in counts, "device state")
        counts[state] += 1
        if state == "failed":
            validate_error(device["error"], "device error")
            require(device["error"]["code"] != 8,
                    "failed device carries cancellation")
            require(device["error"]["deviceIdentifier"] == device["identifier"],
                    "device error identifier mismatch")
        elif state == "cancelled":
            validate_error(device["error"], "device error")
            require(device["error"]["code"] == 8, "cancelled device error")
            require(device["error"]["deviceIdentifier"] == device["identifier"],
                    "device error identifier mismatch")
        else:
            require(device["error"] is None, "non-terminal device error")
        step_states: list[str] = []
        previous_step_finish = None
        for step_index, step in enumerate(device["steps"]):
            require(isinstance(step, dict), "report step object")
            require_keys(step, REPORT_STEP_KEYS, "report step")
            require(step["index"] == step_index, "report step index")
            validate_operation_fields(step, "report step")
            step_state = step["state"]
            step_states.append(step_state)
            step_started = (None if step["startedAt"] is None else
                            parse_utc(step["startedAt"], "step startedAt"))
            step_finished = (None if step["finishedAt"] is None else
                             parse_utc(step["finishedAt"], "step finishedAt"))
            if step_started is not None:
                require(started_at <= step_started, "step starts before job")
            if step_finished is not None:
                require(started_at <= step_finished, "step finishes before job starts")
                if finished_at is not None:
                    require(step_finished <= finished_at, "step finishes after job")
            if step_started is not None and step_finished is not None:
                require(step_started <= step_finished, "step finishes before it starts")
            if previous_step_finish is not None:
                next_boundary = (step_started if step_started is not None
                                 else step_finished)
                if next_boundary is not None:
                    require(previous_step_finish <= next_boundary,
                            "device step timestamps are not causal")
            if step_finished is not None:
                previous_step_finish = step_finished
            if step_state == "failed":
                validate_error(step["error"], "step error")
                require(step["error"]["code"] != 8,
                        "failed step carries cancellation")
                require(step["error"]["deviceIdentifier"] == device["identifier"],
                        "step error identifier mismatch")
            elif step_state == "cancelled":
                validate_error(step["error"], "step error")
                require(step["error"]["code"] == 8, "cancelled step error")
                require(step["error"]["deviceIdentifier"] == device["identifier"],
                        "step error identifier mismatch")
            else:
                require(step["error"] is None, "non-error step has an error")
            if step["operation"] == "flash":
                total = step["bytesTotal"]
                transferred = step["bytesTransferred"]
                require(isinstance(total, int) and not isinstance(total, bool),
                        "flash bytesTotal")
                require(isinstance(transferred, int)
                        and not isinstance(transferred, bool),
                        "flash bytesTransferred")
                require(transferred <= total, "flash transferred beyond total")
                if step_state == "succeeded":
                    require(transferred == total,
                            "succeeded flash is not fully transferred")
                elif step_state in {"pending", "skipped"}:
                    require(transferred == 0,
                            "unstarted flash has transferred bytes")
            else:
                require(step["bytesTotal"] is None
                        and step["bytesTransferred"] is None,
                        "non-DATA operation has byte counts")
        validate_device_step_sequence(state, step_states)

    summary = report["summary"]
    require(isinstance(summary, dict), "summary object")
    require_keys(summary, {"total", *counts}, "summary")
    require(summary["total"] == len(devices), "summary total")
    require(sum(summary[state] for state in counts) == summary["total"],
            "summary counts do not add up")
    for state, count in counts.items():
        require(summary[state] == count, f"summary {state}")

    state = report["state"]
    top_error = report["error"]
    if state == "running":
        require(finished_at is None and top_error is None, "running report semantics")
    else:
        require(finished_at is not None, "terminal report has no finish time")
        require(summary["pending"] == 0 and summary["running"] == 0,
                "terminal report has active devices")
        for device in devices:
            require(device["state"] not in {"pending", "running"},
                    "terminal report has active device")
            require(all(step["state"] not in {"pending", "running"}
                        for step in device["steps"]),
                    "terminal report has active step")
        if state != "cancelled":
            require(summary["cancelled"] == 0,
                    "device cancellation did not win job publication")
    if state == "succeeded":
        require(summary["total"] > 0 and summary["succeeded"] == summary["total"],
                "succeeded report summary")
        require(top_error is None, "succeeded report error")
    elif state == "partially_failed":
        require(summary["succeeded"] > 0
                and summary["failed"] > 0,
                "partial report summary")
        require(top_error is None, "partial report error")
    elif state == "failed":
        require(summary["succeeded"] == 0
                and (summary["failed"] > 0 or top_error is not None),
                "failed report semantics")
        if top_error is not None:
            require(top_error["code"] != 8,
                    "failed report carries cancellation")
    elif state == "cancelled":
        validate_error(top_error, "cancelled report error")
        require(top_error["code"] == 8, "cancelled report error code")


def validate_report_against_plan(
    report: dict[str, object], plan: dict[str, object]
) -> None:
    require(report["planSha256"] == canonical_plan_sha(plan),
            "report is not bound to this plan")
    targets = {target["name"]: target for target in plan["targets"]}
    for device in report["devices"]:
        matching_targets = []
        for target in plan["targets"]:
            selector = target["selector"]
            serial_match = (device["serial"] is not None
                            and device["serial"] in selector["serials"])
            path_match = (device["usbPath"] is not None
                          and device["usbPath"] in selector["usbPaths"])
            if serial_match or path_match:
                matching_targets.append(target)
        require(len(matching_targets) == 1,
                "report device does not match exactly one target")
        require(device["target"] in targets
                and matching_targets[0]["name"] == device["target"],
                "report device target does not match its selector")
        target = targets[device["target"]]
        if device["observedProduct"] != target["expectedProduct"]:
            require(device["state"] == "failed" and device["error"] is not None
                    and all(step["state"] == "skipped"
                            for step in device["steps"]),
                    "non-failed device product differs from plan")

        require(len(device["steps"]) == len(target["steps"]),
                "report step count differs from plan")
        for report_step, plan_step in zip(device["steps"], target["steps"]):
            for field in PLAN_STEP_KEYS:
                require(report_step[field] == plan_step[field],
                        f"report step {field} differs from plan")


def validate_schema_contracts(
    manifest_schema: dict[str, object], plan_schema: dict[str, object],
    report_schema: dict[str, object]
) -> None:
    check_schema(manifest_schema)
    check_schema(plan_schema)
    check_schema(report_schema)
    require(manifest_schema["$id"].endswith("fleet-job.v1.schema.json"),
            "manifest schema id")
    require(plan_schema["$id"].endswith("job-plan.v1.schema.json"), "plan schema id")
    require(report_schema["$id"].endswith("job-report.v1.schema.json"),
            "report schema id")
    require(plan_schema["additionalProperties"] is False, "open plan schema")
    require(report_schema["additionalProperties"] is False, "open report schema")
    require(set(plan_schema["required"]) == PLAN_KEYS, "plan required fields")
    require(set(plan_schema["properties"]) == PLAN_KEYS, "plan properties")
    require(set(report_schema["required"]) == REPORT_KEYS, "report required fields")
    require(set(report_schema["properties"]) == REPORT_KEYS, "report properties")
    require("planned" not in report_schema["properties"]["state"]["enum"],
            "JobReport must not represent a plan")
    require(not ({"jobId", "startedAt", "finishedAt"} & set(plan_schema["properties"])),
            "JobPlan contains nondeterministic fields")
    require(set(plan_schema["$defs"]["step"]["required"]) == PLAN_STEP_KEYS,
            "plan step required fields")
    require(set(report_schema["$defs"]["step"]["required"]) == REPORT_STEP_KEYS,
            "report step required fields")
    require(report_schema["$defs"]["error"]["properties"]["status"]["enum"]
            == list(STATUS_CODES.values()), "status enum drift")
    require(report_schema["$defs"]["error"]["properties"]["nativeCode"]
            ["minimum"] == -2_147_483_648, "nativeCode minimum drift")
    require(report_schema["$defs"]["error"]["properties"]["nativeCode"]
            ["maximum"] == 2_147_483_647, "nativeCode maximum drift")

    header = (ROOT / "include" / "kairosboot" / "kairosboot.h").read_text(
        encoding="utf-8"
    )
    discovered = {
        name: int(value)
        for name, value in re.findall(r"\b(KB_E_[A-Z_]+)\s*=\s*([0-9]+)", header)
    }
    require(discovered == STATUS_ENUM_NAMES, "C header status enum drift")


def expect_instance_rejected(
    value: object, schema: dict[str, object], name: str
) -> None:
    try:
        validate(value, schema)
    except InstanceValidationError:
        return
    raise AssertionError(f"{name} unexpectedly passed schema validation")


def expect_semantic_rejected(value: dict[str, object], name: str) -> None:
    try:
        validate_report(value)
    except AssertionError:
        return
    raise AssertionError(f"{name} unexpectedly passed semantic validation")


def expect_plan_semantic_rejected(value: dict[str, object], name: str) -> None:
    try:
        validate_plan(value)
    except AssertionError:
        return
    raise AssertionError(f"{name} unexpectedly passed plan semantic validation")


def expect_report_plan_rejected(
    report: dict[str, object], plan: dict[str, object], name: str
) -> None:
    try:
        validate_report_against_plan(report, plan)
    except AssertionError:
        return
    raise AssertionError(f"{name} unexpectedly passed report/plan validation")


def expect_plan_derivation_rejected(
    plan: dict[str, object], manifest: dict[str, object],
    manifest_sha256: str, name: str
) -> None:
    if plan != normalize_manifest(manifest, manifest_sha256):
        return
    raise AssertionError(f"{name} unexpectedly matched normalized manifest")


def cancellation_error(identifier: str | None = None) -> dict[str, object]:
    return {
        "code": 8,
        "status": "cancelled",
        "message": "operation cancelled",
        "deviceIdentifier": identifier,
        "nativeCode": None,
        "transferCertainty": "partial_or_unknown",
    }


def summary_for(devices: list[dict[str, object]]) -> dict[str, int]:
    states = ("pending", "running", "succeeded", "failed", "cancelled", "skipped")
    result = {state: 0 for state in states}
    for device in devices:
        result[device["state"]] += 1
    return {"total": len(devices), **result}


def validate_manifest_schema(manifest_schema: dict[str, object]) -> None:
    manifest = {
        "apiVersion": "kairosboot.io/v1",
        "kind": "FlashJob",
        "artifacts": [{
            "id": "system",
            "path": "images/system.img",
            "sha256": "0" * 64,
        }],
        "targets": [{
            "name": "product-a",
            "selector": {"serials": ["SERIAL-01"]},
            "expectedProduct": "product_a",
            "steps": [{"flash": {"partition": "system", "artifact": "system"}}],
        }],
    }
    validate(manifest, manifest_schema)

    empty_selector = copy.deepcopy(manifest)
    empty_selector["targets"][0]["selector"] = {}
    expect_instance_rejected(empty_selector, manifest_schema,
                             "empty manifest selector")

    empty_step = copy.deepcopy(manifest)
    empty_step["targets"][0]["steps"] = [{}]
    expect_instance_rejected(empty_step, manifest_schema,
                             "empty manifest step")


def normalize_manifest(
    manifest: dict[str, object], manifest_sha256: str
) -> dict[str, object]:
    artifacts = [
        {"index": index, **artifact, "sha256": artifact["sha256"].lower()}
        for index, artifact in enumerate(manifest["artifacts"])
    ]
    targets = []
    for target_index, target in enumerate(manifest["targets"]):
        normalized_steps = []
        for step_index, step in enumerate(target["steps"]):
            action, payload = next(iter(step.items()))
            normalized = {
                "index": step_index,
                "operation": None,
                "partition": None,
                "artifact": None,
                "slot": None,
                "rebootTarget": None,
                "oemCommand": None,
            }
            if action == "flash":
                normalized.update({
                    "operation": "flash",
                    "partition": payload["partition"],
                    "artifact": payload["artifact"],
                    "slot": payload.get("slot"),
                })
            elif action == "erase":
                normalized.update({
                    "operation": "erase",
                    "partition": payload["partition"],
                })
            elif action == "setActive":
                normalized.update({
                    "operation": "set_active",
                    "slot": payload["slot"],
                })
            elif action == "reboot":
                normalized.update({
                    "operation": "reboot",
                    "rebootTarget": payload.get("target", "system"),
                })
            else:
                normalized.update({
                    "operation": "oem",
                    "oemCommand": payload["command"],
                })
            normalized_steps.append(normalized)
        selector = target["selector"]
        targets.append({
            "index": target_index,
            "name": target["name"],
            "selector": {
                "serials": selector.get("serials", []),
                "usbPaths": selector.get("usbPaths", []),
            },
            "expectedProduct": target["expectedProduct"],
            "steps": normalized_steps,
        })
    policy = manifest.get("policy", {})
    return {
        "schemaVersion": 1,
        "manifestApiVersion": manifest["apiVersion"],
        "kind": manifest["kind"],
        "manifestSha256": manifest_sha256,
        "artifacts": artifacts,
        "targets": targets,
        "policy": {
            "onDeviceFailure": policy.get("onDeviceFailure", "continue"),
            "maxParallelDevices": policy.get("maxParallelDevices", 32),
            "memoryBudget": policy.get("memoryBudget", "auto"),
        },
    }


def validate_normalization_branches(
    manifest_schema: dict[str, object], plan_schema: dict[str, object]
) -> None:
    manifest = {
        "apiVersion": "kairosboot.io/v1",
        "kind": "FlashJob",
        "artifacts": [{
            "id": "boot",
            "path": "images/boot.img",
            "sha256": "A" * 64,
        }],
        "targets": [{
            "name": "usb-only",
            "selector": {"usbPaths": ["usb:2-1"]},
            "expectedProduct": "product_b",
            "steps": [
                {"flash": {"partition": "boot", "artifact": "boot"}},
                {"erase": {"partition": "metadata"}},
                {"setActive": {"slot": "other"}},
                {"reboot": {}},
                {"oem": {"command": "diagnostic-mode"}},
            ],
        }],
    }
    validate(manifest, manifest_schema)
    plan = normalize_manifest(manifest, "0" * 64)
    validate(plan, plan_schema)
    validate_plan(plan)
    require(plan["artifacts"][0]["sha256"] == "a" * 64,
            "uppercase artifact SHA-256 was not normalized")
    target = plan["targets"][0]
    require(target["selector"] == {
        "serials": [], "usbPaths": ["usb:2-1"]
    }, "USB-only selector normalization")
    require([step["operation"] for step in target["steps"]] == [
        "flash", "erase", "set_active", "reboot", "oem"
    ], "operation normalization")
    require(target["steps"][3]["rebootTarget"] == "system",
            "default reboot target")
    require(plan["policy"] == {
        "onDeviceFailure": "continue",
        "maxParallelDevices": 32,
        "memoryBudget": "auto",
    }, "default policy normalization")


def validate_positive_state_cases(
    report: dict[str, object], report_schema: dict[str, object]
) -> None:
    succeeded = copy.deepcopy(report)
    succeeded["state"] = "succeeded"
    succeeded["devices"] = [succeeded["devices"][0]]
    succeeded["summary"] = summary_for(succeeded["devices"])

    failed = copy.deepcopy(report)
    failed["state"] = "failed"
    failed["devices"] = [failed["devices"][1]]
    failed["summary"] = summary_for(failed["devices"])

    running = copy.deepcopy(report)
    running["state"] = "running"
    running["finishedAt"] = None
    running["devices"] = [running["devices"][0]]
    running_device = running["devices"][0]
    running_device["state"] = "running"
    running_step = running_device["steps"][0]
    running_step["state"] = "running"
    running_step["finishedAt"] = None
    running_step["bytesTransferred"] = 1024
    running["summary"] = summary_for(running["devices"])

    pending = copy.deepcopy(running)
    pending_device = pending["devices"][0]
    pending_device["state"] = "pending"
    pending_step = pending_device["steps"][0]
    pending_step["state"] = "pending"
    pending_step["startedAt"] = None
    pending_step["bytesTransferred"] = 0
    pending["summary"] = summary_for(pending["devices"])

    cancelled = copy.deepcopy(report)
    cancelled["state"] = "cancelled"
    cancelled["error"] = cancellation_error()
    cancelled["devices"] = [cancelled["devices"][0]]
    cancelled_device = cancelled["devices"][0]
    cancelled_device["state"] = "cancelled"
    cancelled_device["error"] = cancellation_error(cancelled_device["identifier"])
    cancelled_step = cancelled_device["steps"][0]
    cancelled_step["state"] = "cancelled"
    cancelled_step["bytesTransferred"] = 2048
    cancelled_step["error"] = cancellation_error(cancelled_device["identifier"])
    cancelled["summary"] = summary_for(cancelled["devices"])

    skipped = copy.deepcopy(report)
    skipped["state"] = "failed"
    skipped["devices"] = [skipped["devices"][0]]
    skipped_device = skipped["devices"][0]
    skipped_device["state"] = "skipped"
    skipped_step = skipped_device["steps"][0]
    skipped_step["state"] = "skipped"
    skipped_step["startedAt"] = None
    skipped_step["bytesTransferred"] = 0
    skipped["summary"] = summary_for(skipped["devices"])
    skipped["error"] = {
        "code": 1,
        "status": "invalid_argument",
        "message": "preflight stopped remaining devices",
        "deviceIdentifier": None,
        "nativeCode": None,
        "transferCertainty": "not_sent",
    }

    device_preflight_failed = copy.deepcopy(report)
    device_preflight_failed["state"] = "failed"
    device_preflight_failed["devices"] = [device_preflight_failed["devices"][1]]
    failed_device = device_preflight_failed["devices"][0]
    failed_step = failed_device["steps"][0]
    failed_step["state"] = "skipped"
    failed_step["startedAt"] = None
    failed_step["bytesTransferred"] = 0
    failed_step["error"] = None
    device_preflight_failed["summary"] = summary_for(
        device_preflight_failed["devices"]
    )

    preflight_failed = copy.deepcopy(report)
    preflight_failed["state"] = "failed"
    preflight_failed["devices"] = []
    preflight_failed["summary"] = summary_for([])
    preflight_failed["error"] = {
        "code": 1,
        "status": "invalid_argument",
        "message": "artifact hash mismatch",
        "deviceIdentifier": None,
        "nativeCode": None,
        "transferCertainty": "not_sent",
    }

    for name, value in {
        "succeeded": succeeded,
        "failed": failed,
        "running": running,
        "pending": pending,
        "cancelled": cancelled,
        "skipped": skipped,
        "device preflight failed": device_preflight_failed,
        "preflight failed": preflight_failed,
    }.items():
        validate(value, report_schema)
        validate_report(value)


def validate_operation_branches(
    plan: dict[str, object], report: dict[str, object],
    plan_schema: dict[str, object], report_schema: dict[str, object]
) -> None:
    fields = {
        "flash": ("system", "system", None, None, None),
        "erase": ("userdata", None, None, None, None),
        "set_active": (None, None, "a", None, None),
        "reboot": (None, None, None, "bootloader", None),
        "oem": (None, None, None, None, "device-info"),
    }
    for operation, (partition, artifact, slot, reboot_target, oem_command) in fields.items():
        plan_case = copy.deepcopy(plan)
        plan_step = plan_case["targets"][0]["steps"][0]
        plan_step.update({
            "operation": operation,
            "partition": partition,
            "artifact": artifact,
            "slot": slot,
            "rebootTarget": reboot_target,
            "oemCommand": oem_command,
        })
        validate(plan_case, plan_schema)
        validate_plan(plan_case)

        report_case = copy.deepcopy(report)
        report_step = report_case["devices"][0]["steps"][0]
        report_step.update({
            "operation": operation,
            "partition": partition,
            "artifact": artifact,
            "slot": slot,
            "rebootTarget": reboot_target,
            "oemCommand": oem_command,
            "bytesTotal": 4096 if operation == "flash" else None,
            "bytesTransferred": 4096 if operation == "flash" else None,
        })
        validate(report_case, report_schema)
        validate_report(report_case)


def validate_negative_cases(
    plan: dict[str, object], report: dict[str, object],
    plan_schema: dict[str, object], report_schema: dict[str, object]
) -> None:
    unknown = copy.deepcopy(report)
    unknown["unexpected"] = True
    expect_instance_rejected(unknown, report_schema, "unknown report field")

    mismatch = copy.deepcopy(report)
    mismatch["devices"][1]["error"]["status"] = "io"
    expect_instance_rejected(mismatch, report_schema, "status/code mismatch")

    failed_without_error = copy.deepcopy(report)
    failed_without_error["devices"][1]["error"] = None
    expect_instance_rejected(failed_without_error, report_schema,
                             "failed device without error")

    success_with_error = copy.deepcopy(report)
    success_with_error["devices"][0]["error"] = copy.deepcopy(
        report["devices"][1]["error"]
    )
    expect_instance_rejected(success_with_error, report_schema,
                             "succeeded device with error")

    non_data_bytes = copy.deepcopy(report)
    non_data_step = non_data_bytes["devices"][0]["steps"][0]
    non_data_step.update({
        "operation": "erase", "partition": "userdata", "artifact": None,
        "slot": None, "rebootTarget": None, "oemCommand": None,
    })
    expect_instance_rejected(non_data_bytes, report_schema,
                             "non-DATA operation byte counts")

    offset_time = copy.deepcopy(report)
    offset_time["startedAt"] = "2026-08-27T14:00:00+08:00"
    expect_instance_rejected(offset_time, report_schema, "non-Z timestamp")

    trailing_newline_hash = copy.deepcopy(plan)
    trailing_newline_hash["manifestSha256"] += "\n"
    expect_instance_rejected(trailing_newline_hash, plan_schema,
                             "SHA-256 with trailing newline")

    unsafe_integer = copy.deepcopy(report)
    unsafe_integer["summary"]["total"] = 9_007_199_254_740_992
    expect_instance_rejected(unsafe_integer, report_schema, "unsafe integer")

    floating_count = copy.deepcopy(report)
    floating_count["summary"]["total"] = 2.0
    expect_instance_rejected(floating_count, report_schema, "floating count")

    nonfinite_count = copy.deepcopy(report)
    nonfinite_count["summary"]["total"] = float("nan")
    expect_instance_rejected(nonfinite_count, report_schema, "non-finite count")

    oversized_native_code = copy.deepcopy(report)
    oversized_native_code["devices"][1]["error"]["nativeCode"] = 2_147_483_648
    expect_instance_rejected(oversized_native_code, report_schema,
                             "native code outside int32")

    too_many_bytes = copy.deepcopy(report)
    too_many_bytes["devices"][0]["steps"][0]["bytesTransferred"] = 4097
    validate(too_many_bytes, report_schema)
    expect_semantic_rejected(too_many_bytes, "transferred beyond total")

    incomplete_success = copy.deepcopy(report)
    incomplete_success["devices"][0]["steps"][0]["bytesTransferred"] = 4095
    validate(incomplete_success, report_schema)
    expect_semantic_rejected(incomplete_success,
                             "succeeded flash not fully transferred")

    wrong_summary = copy.deepcopy(report)
    wrong_summary["summary"]["failed"] = 0
    wrong_summary["summary"]["skipped"] = 1
    validate(wrong_summary, report_schema)
    expect_semantic_rejected(wrong_summary, "summary/device mismatch")

    duplicate_device = copy.deepcopy(report)
    duplicate_device["devices"][1]["identifier"] = \
        duplicate_device["devices"][0]["identifier"]
    validate(duplicate_device, report_schema)
    expect_semantic_rejected(duplicate_device, "duplicate device identifier")

    invalid_partial = copy.deepcopy(report)
    invalid_partial["devices"] = [invalid_partial["devices"][0]]
    invalid_partial["summary"] = summary_for(invalid_partial["devices"])
    validate(invalid_partial, report_schema)
    expect_semantic_rejected(invalid_partial, "partial report without failure")

    terminal_active = copy.deepcopy(report)
    terminal_active["state"] = "cancelled"
    terminal_active["error"] = cancellation_error()
    terminal_active["devices"] = [terminal_active["devices"][0]]
    active_device = terminal_active["devices"][0]
    active_device["state"] = "running"
    active_step = active_device["steps"][0]
    active_step["state"] = "running"
    active_step["finishedAt"] = None
    active_step["bytesTransferred"] = 1024
    terminal_active["summary"] = summary_for(terminal_active["devices"])
    expect_instance_rejected(terminal_active, report_schema,
                             "terminal report with running work")

    terminal_without_finish = copy.deepcopy(report)
    terminal_without_finish["finishedAt"] = None
    expect_instance_rejected(terminal_without_finish, report_schema,
                             "terminal report without finish")

    running_with_finish = copy.deepcopy(report)
    running_with_finish["state"] = "running"
    expect_instance_rejected(running_with_finish, report_schema,
                             "running report with finish")

    reversed_report_time = copy.deepcopy(report)
    reversed_report_time["startedAt"] = "2026-08-27T06:01:00Z"
    validate(reversed_report_time, report_schema)
    expect_semantic_rejected(reversed_report_time, "reversed report timestamps")

    reversed_step_time = copy.deepcopy(report)
    reversed_step_time["devices"][0]["steps"][0].update({
        "startedAt": "2026-08-27T06:00:02Z",
        "finishedAt": "2026-08-27T06:00:01Z",
    })
    validate(reversed_step_time, report_schema)
    expect_semantic_rejected(reversed_step_time, "reversed step timestamps")

    overlapping_steps = copy.deepcopy(report)
    overlapping_device = overlapping_steps["devices"][0]
    overlapping_second = copy.deepcopy(overlapping_device["steps"][0])
    overlapping_second.update({
        "index": 1,
        "startedAt": "2026-08-27T06:00:01.5Z",
        "finishedAt": "2026-08-27T06:00:03Z",
    })
    overlapping_device["steps"].append(overlapping_second)
    validate(overlapping_steps, report_schema)
    expect_semantic_rejected(overlapping_steps, "overlapping serial steps")

    reversed_skipped_finish = copy.deepcopy(report)
    failed_device = reversed_skipped_finish["devices"][1]
    skipped_suffix = copy.deepcopy(failed_device["steps"][0])
    skipped_suffix.update({
        "index": 1,
        "state": "skipped",
        "startedAt": None,
        "finishedAt": "2026-08-27T06:00:02Z",
        "bytesTransferred": 0,
        "error": None,
    })
    failed_device["steps"].append(skipped_suffix)
    validate(reversed_skipped_finish, report_schema)
    expect_semantic_rejected(
        reversed_skipped_finish,
        "skipped suffix finishing before the failed step",
    )

    wrong_cancel_error = copy.deepcopy(report)
    wrong_cancel_error["state"] = "cancelled"
    wrong_cancel_error["error"] = copy.deepcopy(report["devices"][1]["error"])
    expect_instance_rejected(wrong_cancel_error, report_schema,
                             "cancelled report with non-cancel error")

    cancellation_as_failed_device = copy.deepcopy(report)
    cancellation_as_failed_device["devices"][1]["error"] = \
        cancellation_error("usb:1-3")
    expect_instance_rejected(cancellation_as_failed_device, report_schema,
                             "cancellation encoded as failed device")
    expect_semantic_rejected(cancellation_as_failed_device,
                             "cancellation encoded as failed device semantics")

    cancellation_as_failed_step = copy.deepcopy(report)
    cancellation_as_failed_step["devices"][1]["steps"][0]["error"] = \
        cancellation_error("usb:1-3")
    expect_instance_rejected(cancellation_as_failed_step, report_schema,
                             "cancellation encoded as failed step")
    expect_semantic_rejected(cancellation_as_failed_step,
                             "cancellation encoded as failed step semantics")

    cancellation_as_failed_job = copy.deepcopy(report)
    cancellation_as_failed_job["state"] = "failed"
    cancellation_as_failed_job["devices"] = []
    cancellation_as_failed_job["summary"] = summary_for([])
    cancellation_as_failed_job["error"] = cancellation_error()
    expect_instance_rejected(cancellation_as_failed_job, report_schema,
                             "cancellation encoded as failed job")
    expect_semantic_rejected(cancellation_as_failed_job,
                             "cancellation encoded as failed job semantics")

    failed_step_without_error = copy.deepcopy(report)
    failed_step_without_error["devices"][1]["steps"][0]["error"] = None
    expect_instance_rejected(failed_step_without_error, report_schema,
                             "failed step without error")

    wrong_device_error_scope = copy.deepcopy(report)
    wrong_device_error_scope["devices"][1]["error"] \
        ["deviceIdentifier"] = "usb:someone-else"
    validate(wrong_device_error_scope, report_schema)
    expect_semantic_rejected(wrong_device_error_scope,
                             "device error scoped to another device")

    wrong_step_error_scope = copy.deepcopy(report)
    wrong_step_error_scope["devices"][1]["steps"][0]["error"] \
        ["deviceIdentifier"] = "usb:someone-else"
    validate(wrong_step_error_scope, report_schema)
    expect_semantic_rejected(wrong_step_error_scope,
                             "step error scoped to another device")

    two_running_steps = copy.deepcopy(report)
    two_running_steps["state"] = "running"
    two_running_steps["finishedAt"] = None
    two_running_steps["devices"] = [two_running_steps["devices"][0]]
    active_device = two_running_steps["devices"][0]
    active_device["state"] = "running"
    first_active = active_device["steps"][0]
    first_active.update({
        "state": "running", "finishedAt": None, "bytesTransferred": 1024,
    })
    second_active = copy.deepcopy(first_active)
    second_active["index"] = 1
    active_device["steps"].append(second_active)
    two_running_steps["summary"] = summary_for(two_running_steps["devices"])
    validate(two_running_steps, report_schema)
    expect_semantic_rejected(two_running_steps, "multiple running device steps")

    partial_without_failure = copy.deepcopy(report)
    skipped_device = partial_without_failure["devices"][1]
    skipped_device["state"] = "skipped"
    skipped_device["error"] = None
    skipped_step = skipped_device["steps"][0]
    skipped_step.update({
        "state": "skipped", "startedAt": None, "bytesTransferred": 0,
        "error": None,
    })
    partial_without_failure["summary"] = summary_for(
        partial_without_failure["devices"]
    )
    validate(partial_without_failure, report_schema)
    expect_semantic_rejected(partial_without_failure,
                             "partial report without error source")

    failed_after_success = copy.deepcopy(report)
    failed_after_success["state"] = "failed"
    failed_after_success["error"] = {
        "code": 10,
        "status": "internal",
        "message": "preflight integrity failure",
        "deviceIdentifier": None,
        "nativeCode": None,
        "transferCertainty": "not_sent",
    }
    validate(failed_after_success, report_schema)
    expect_semantic_rejected(failed_after_success,
                             "preflight failure after device success")

    impossible_failed_device = copy.deepcopy(report)
    impossible_step = impossible_failed_device["devices"][1]["steps"][0]
    impossible_step["state"] = "succeeded"
    impossible_step["error"] = None
    validate(impossible_failed_device, report_schema)
    expect_semantic_rejected(impossible_failed_device,
                             "failed device without failure point")

    pending_with_progress = copy.deepcopy(report)
    pending_with_progress["state"] = "running"
    pending_with_progress["finishedAt"] = None
    pending_with_progress["devices"] = [pending_with_progress["devices"][0]]
    pending_device = pending_with_progress["devices"][0]
    pending_device["state"] = "pending"
    pending_step = pending_device["steps"][0]
    pending_step.update({
        "state": "pending", "startedAt": None, "finishedAt": None,
        "bytesTransferred": 1, "error": None,
    })
    pending_with_progress["summary"] = summary_for(
        pending_with_progress["devices"]
    )
    expect_instance_rejected(pending_with_progress, report_schema,
                             "pending flash with progress")

    skipped_with_progress = copy.deepcopy(report)
    skipped_with_progress["state"] = "failed"
    skipped_with_progress["error"] = {
        "code": 1,
        "status": "invalid_argument",
        "message": "preflight failed",
        "deviceIdentifier": None,
        "nativeCode": None,
        "transferCertainty": "not_sent",
    }
    skipped_with_progress["devices"] = [skipped_with_progress["devices"][0]]
    skipped_device = skipped_with_progress["devices"][0]
    skipped_device["state"] = "skipped"
    skipped_step = skipped_device["steps"][0]
    skipped_step.update({
        "state": "skipped", "startedAt": None, "bytesTransferred": 1,
        "error": None,
    })
    skipped_with_progress["summary"] = summary_for(
        skipped_with_progress["devices"]
    )
    expect_instance_rejected(skipped_with_progress, report_schema,
                             "skipped flash with progress")

    unknown_target = copy.deepcopy(report)
    unknown_target["devices"][0]["target"] = "missing"
    validate(unknown_target, report_schema)
    expect_report_plan_rejected(unknown_target, plan, "unknown report target")

    unselected_device = copy.deepcopy(report)
    unselected_device["devices"][0]["serial"] = "SERIAL-UNKNOWN"
    validate(unselected_device, report_schema)
    expect_report_plan_rejected(unselected_device, plan,
                                "device outside plan selectors")

    wrong_product = copy.deepcopy(report)
    wrong_product["devices"][0]["observedProduct"] = "product_b"
    validate(wrong_product, report_schema)
    expect_report_plan_rejected(wrong_product, plan,
                                "succeeded device with unexpected product")

    destructive_wrong_product = copy.deepcopy(report)
    destructive_wrong_product["devices"][1]["observedProduct"] = "product_b"
    validate(destructive_wrong_product, report_schema)
    validate_report(destructive_wrong_product)
    expect_report_plan_rejected(
        destructive_wrong_product,
        plan,
        "unexpected product after a destructive step",
    )

    wrong_plan_hash = copy.deepcopy(report)
    wrong_plan_hash["planSha256"] = "0" * 64
    validate(wrong_plan_hash, report_schema)
    validate_report(wrong_plan_hash)
    expect_report_plan_rejected(wrong_plan_hash, plan, "wrong plan hash")

    changed_report_step = copy.deepcopy(report)
    changed_report_step["devices"][0]["steps"][0]["partition"] = "vendor"
    validate(changed_report_step, report_schema)
    expect_report_plan_rejected(changed_report_step, plan,
                                "report step differing from plan")

    cross_namespace_overlap = copy.deepcopy(plan)
    path_target = copy.deepcopy(cross_namespace_overlap["targets"][0])
    path_target["index"] = 1
    path_target["name"] = "product-path"
    path_target["selector"] = {"serials": [], "usbPaths": ["usb:1-2"]}
    cross_namespace_overlap["targets"].append(path_target)
    validate(cross_namespace_overlap, plan_schema)
    validate_plan(cross_namespace_overlap)
    overlap_report = copy.deepcopy(report)
    overlap_report["planSha256"] = canonical_plan_sha(cross_namespace_overlap)
    expect_report_plan_rejected(
        overlap_report, cross_namespace_overlap,
        "device matching selectors in two target namespaces",
    )

    repeated_cross_namespace_value = copy.deepcopy(plan)
    repeated_value_target = copy.deepcopy(
        repeated_cross_namespace_value["targets"][0]
    )
    repeated_value_target["index"] = 1
    repeated_value_target["name"] = "cross-namespace-value"
    repeated_value_target["selector"] = {
        "serials": [], "usbPaths": ["SERIAL-01"]
    }
    repeated_cross_namespace_value["targets"].append(repeated_value_target)
    validate(repeated_cross_namespace_value, plan_schema)
    expect_plan_semantic_rejected(
        repeated_cross_namespace_value,
        "selector value repeated across namespaces",
    )

    missing_artifact = copy.deepcopy(plan)
    missing_artifact["targets"][0]["steps"][0]["artifact"] = "missing"
    validate(missing_artifact, plan_schema)
    expect_plan_semantic_rejected(missing_artifact,
                                  "missing artifact reference")

    duplicate_artifact = copy.deepcopy(plan)
    repeated_artifact = copy.deepcopy(duplicate_artifact["artifacts"][0])
    repeated_artifact["index"] = 1
    duplicate_artifact["artifacts"].append(repeated_artifact)
    validate(duplicate_artifact, plan_schema)
    expect_plan_semantic_rejected(duplicate_artifact, "duplicate artifact id")

    overlapping_target = copy.deepcopy(plan)
    repeated_target = copy.deepcopy(overlapping_target["targets"][0])
    repeated_target["index"] = 1
    repeated_target["name"] = "product-b"
    overlapping_target["targets"].append(repeated_target)
    validate(overlapping_target, plan_schema)
    expect_plan_semantic_rejected(overlapping_target,
                                  "selector owned by multiple targets")

    duplicate_target_name = copy.deepcopy(plan)
    repeated_name = copy.deepcopy(duplicate_target_name["targets"][0])
    repeated_name["index"] = 1
    repeated_name["selector"] = {"serials": ["SERIAL-02"], "usbPaths": []}
    duplicate_target_name["targets"].append(repeated_name)
    validate(duplicate_target_name, plan_schema)
    expect_plan_semantic_rejected(duplicate_target_name, "duplicate target name")

    for digits in ("1", "123", "123456", "123456789"):
        fractional_time = copy.deepcopy(report)
        fractional_time["startedAt"] = f"2026-08-27T06:00:00.{digits}Z"
        validate(fractional_time, report_schema)
        validate_report(fractional_time)

    invalid_calendar_date = copy.deepcopy(report)
    invalid_calendar_date["startedAt"] = "2026-02-30T06:00:00Z"
    expect_instance_rejected(invalid_calendar_date, report_schema,
                             "invalid calendar date")

    surrogate_text = copy.deepcopy(report)
    surrogate_text["jobId"] = "job-\ud800"
    expect_instance_rejected(surrogate_text, report_schema,
                             "unpaired Unicode surrogate")

    changed_manifest = json.loads(
        (CONTRACTS / "fleet-job-v1.fixture.yaml").read_text(encoding="utf-8")
    )
    changed_manifest["targets"][0]["expectedProduct"] = "product_b"
    changed_bytes = json.dumps(
        changed_manifest, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    changed_sha = hashlib.sha256(changed_bytes).hexdigest()
    stale_plan = copy.deepcopy(plan)
    stale_plan["manifestSha256"] = changed_sha
    validate(stale_plan, plan_schema)
    expect_plan_derivation_rejected(
        stale_plan, changed_manifest, changed_sha,
        "stale plan with updated manifest hash",
    )

    bad_schema = copy.deepcopy(plan_schema)
    bad_schema["unsupportedKeyword"] = True
    try:
        check_schema(bad_schema)
    except SchemaDefinitionError:
        pass
    else:
        raise AssertionError("unsupported schema keyword was accepted")

    unresolved = copy.deepcopy(plan_schema)
    unresolved["properties"]["manifestSha256"] = {"$ref": "#/$defs/missing"}
    try:
        check_schema(unresolved)
    except SchemaDefinitionError:
        pass
    else:
        raise AssertionError("unresolved schema reference was accepted")

    invalid_operation = copy.deepcopy(plan)
    invalid_operation["targets"][0]["steps"][0]["operation"] = "format"
    expect_instance_rejected(invalid_operation, plan_schema, "unknown operation")

    invalid_unique_items = copy.deepcopy(plan_schema)
    invalid_unique_items["$defs"]["selector"]["properties"]["serials"] \
        ["uniqueItems"] = 1
    try:
        check_schema(invalid_unique_items)
    except SchemaDefinitionError:
        pass
    else:
        raise AssertionError("non-boolean uniqueItems was accepted")


def main() -> None:
    manifest_schema = load_json(SCHEMAS / "fleet-job.v1.schema.json")
    plan_schema = load_json(SCHEMAS / "job-plan.v1.schema.json")
    report_schema = load_json(SCHEMAS / "job-report.v1.schema.json")
    validate_canonical_json_profile()
    validate_schema_contracts(manifest_schema, plan_schema, report_schema)
    validate_manifest_schema(manifest_schema)
    validate_normalization_branches(manifest_schema, plan_schema)

    plan_path = CONTRACTS / "job-plan-v1.golden.json"
    report_path = CONTRACTS / "job-report-v1.golden.json"
    manifest_path = CONTRACTS / "fleet-job-v1.fixture.yaml"
    plan = load_canonical(plan_path)
    report = load_canonical(report_path)
    validate(plan, plan_schema)
    validate(report, report_schema)
    validate_plan(plan)
    validate_report(report)
    validate_report_against_plan(report, plan)
    same_target_cross_namespace = copy.deepcopy(plan)
    same_target_cross_namespace["targets"][0]["selector"]["usbPaths"] = [
        "SERIAL-01"
    ]
    validate(same_target_cross_namespace, plan_schema)
    validate_plan(same_target_cross_namespace)
    same_target_report = copy.deepcopy(report)
    same_target_report["planSha256"] = canonical_plan_sha(
        same_target_cross_namespace
    )
    same_target_report["devices"][0]["usbPath"] = "SERIAL-01"
    validate_report_against_plan(
        same_target_report,
        same_target_cross_namespace,
    )
    failed_product_preflight = copy.deepcopy(report)
    preflight_device = failed_product_preflight["devices"][1]
    preflight_device["observedProduct"] = "product_b"
    preflight_device["error"].update({
        "message": "device product does not match target",
        "transferCertainty": "not_sent",
    })
    for step in preflight_device["steps"]:
        step.update({
            "state": "skipped",
            "startedAt": None,
            "bytesTransferred": 0,
            "error": None,
        })
    validate(failed_product_preflight, report_schema)
    validate_report(failed_product_preflight)
    validate_report_against_plan(failed_product_preflight, plan)

    manifest_sha = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    require(plan["manifestSha256"] == manifest_sha, "manifest provenance hash")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    validate(manifest, manifest_schema)
    require(plan == normalize_manifest(manifest, manifest_sha),
            "plan is not the normalized manifest")
    uppercase_manifest = copy.deepcopy(manifest)
    uppercase_manifest["artifacts"][0]["sha256"] = \
        uppercase_manifest["artifacts"][0]["sha256"].upper()
    validate(uppercase_manifest, manifest_schema)
    uppercase_plan = normalize_manifest(uppercase_manifest, "0" * 64)
    validate(uppercase_plan, plan_schema)
    require(uppercase_plan["artifacts"][0]["sha256"] ==
            manifest["artifacts"][0]["sha256"],
            "manifest SHA-256 was not normalized to lowercase")
    plan_bytes = plan_path.read_bytes()
    require(plan_bytes.endswith(b"\n") and not plan_bytes.endswith(b"\n\n"),
            "plan golden trailing newline")
    plan_sha = hashlib.sha256(plan_bytes[:-1]).hexdigest()
    require(report["planSha256"] == plan_sha, "plan provenance hash")

    validate_positive_state_cases(report, report_schema)
    validate_operation_branches(plan, report, plan_schema, report_schema)
    validate_negative_cases(plan, report, plan_schema, report_schema)


if __name__ == "__main__":
    main()
