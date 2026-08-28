#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare AOSP Fastboot and KairosBoot normalized event captures."""

from __future__ import annotations

import argparse
import copy
import dataclasses
import json
import pathlib
import re
import sys
from typing import Any, Iterable, Optional, Sequence


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY_ROOT / "tests" / "tooling"))

from json_schema_subset import (  # noqa: E402
    InstanceValidationError,
    SchemaDefinitionError,
    check_schema,
    validate,
)


TIMING_FIELDS = frozenset({"timestamp", "timestampNs", "elapsedMs"})
MISSING = object()


class TraceContractError(ValueError):
    """Raised when a schema or capture violates the trace contract."""


@dataclasses.dataclass(frozen=True)
class Difference:
    """The first semantic difference between two normalized captures."""

    path: str
    expected: Any
    actual: Any


def _reject_duplicate_keys(pairs: Iterable[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise TraceContractError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def _load_json(path: pathlib.Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as source:
            return json.load(source, object_pairs_hook=_reject_duplicate_keys)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise TraceContractError(f"cannot load {path}: {error}") from error


def load_capture(capture_path: pathlib.Path, schema_path: pathlib.Path) -> dict[str, Any]:
    """Load and validate one normalized trace capture."""

    schema = _load_json(schema_path)
    capture = _load_json(capture_path)
    if not isinstance(schema, dict):
        raise TraceContractError(f"schema root in {schema_path} is not an object")
    if not isinstance(capture, dict):
        raise TraceContractError(f"capture root in {capture_path} is not an object")
    try:
        check_schema(schema)
        validate(capture, schema)
    except (SchemaDefinitionError, InstanceValidationError) as error:
        raise TraceContractError(f"{capture_path}: {error}") from error

    scenario_ids: set[str] = set()
    for index, scenario in enumerate(capture["scenarios"]):
        scenario_id = scenario["id"]
        if scenario_id in scenario_ids:
            raise TraceContractError(
                f"{capture_path}: duplicate scenario id {scenario_id!r}"
            )
        scenario_ids.add(scenario_id)
        events = scenario["events"]
        if events[0]["kind"] != "CLI_PARSE":
            raise TraceContractError(
                f"{capture_path}: $.scenarios[{index}] must begin with CLI_PARSE"
            )
        if events[-1]["kind"] != "EXIT":
            raise TraceContractError(
                f"{capture_path}: $.scenarios[{index}] must end with EXIT"
            )
        if any(event["kind"] == "CLI_PARSE" for event in events[1:]):
            raise TraceContractError(
                f"{capture_path}: $.scenarios[{index}] has a second CLI_PARSE"
            )
        if any(event["kind"] == "EXIT" for event in events[:-1]):
            raise TraceContractError(
                f"{capture_path}: $.scenarios[{index}] has an early EXIT"
            )
    return capture


def _is_absolute_root(root: str) -> bool:
    return (
        root.startswith("/")
        or root.startswith("\\\\")
        or re.match(r"^[A-Za-z]:[\\/]", root) is not None
    )


def _validate_temp_roots(roots: Sequence[str], label: str) -> None:
    for root in roots:
        trimmed = root.rstrip("/\\")
        drive_root = re.fullmatch(r"[A-Za-z]:", trimmed) is not None
        if not trimmed or drive_root or not _is_absolute_root(root):
            raise TraceContractError(
                f"{label} temporary root must be an absolute non-root path: {root!r}"
            )


def _replace_root(value: str, root: str, marker: str) -> str:
    trimmed = root.rstrip("/\\")
    pattern = re.compile(re.escape(trimmed) + r"(?=$|[/\\])")
    return pattern.sub(marker, value)


def _normalize_paths(value: Any, roots: Sequence[str]) -> Any:
    if isinstance(value, str):
        result = value
        for index, root in enumerate(roots):
            result = _replace_root(result, root, f"<TEMP:{index}>")
        return result
    if isinstance(value, list):
        return [_normalize_paths(item, roots) for item in value]
    if isinstance(value, dict):
        return {
            key: _normalize_paths(item, roots)
            for key, item in value.items()
        }
    return value


def _normalize_capture(capture: dict[str, Any], roots: Sequence[str]) -> Any:
    result = copy.deepcopy(capture)
    for scenario in result["scenarios"]:
        for event in scenario["events"]:
            for field in TIMING_FIELDS:
                event.pop(field, None)
    return _normalize_paths(result, roots)


def _first_difference(
    expected: Any, actual: Any, path: str = "$"
) -> Optional[Difference]:
    if type(expected) is not type(actual):
        return Difference(path, expected, actual)
    if isinstance(expected, dict):
        keys = list(expected)
        keys.extend(sorted(set(actual) - set(expected)))
        for key in keys:
            child_path = f"{path}.{key}"
            if key not in expected:
                return Difference(child_path, MISSING, actual[key])
            if key not in actual:
                return Difference(child_path, expected[key], MISSING)
            difference = _first_difference(expected[key], actual[key], child_path)
            if difference is not None:
                return difference
        return None
    if isinstance(expected, list):
        common_size = min(len(expected), len(actual))
        for index in range(common_size):
            difference = _first_difference(
                expected[index], actual[index], f"{path}[{index}]"
            )
            if difference is not None:
                return difference
        if len(expected) != len(actual):
            index = common_size
            return Difference(
                f"{path}[{index}]",
                expected[index] if index < len(expected) else MISSING,
                actual[index] if index < len(actual) else MISSING,
            )
        return None
    if expected != actual:
        return Difference(path, expected, actual)
    return None


def compare_captures(
    expected: dict[str, Any],
    actual: dict[str, Any],
    expected_temp_roots: Sequence[str] = (),
    actual_temp_roots: Sequence[str] = (),
) -> Optional[Difference]:
    """Return the first difference after the contract's narrow normalization."""

    if len(expected_temp_roots) != len(actual_temp_roots):
        raise TraceContractError(
            "expected and actual temporary root counts must match"
        )
    _validate_temp_roots(expected_temp_roots, "expected")
    _validate_temp_roots(actual_temp_roots, "actual")
    normalized_expected = _normalize_capture(expected, expected_temp_roots)
    normalized_actual = _normalize_capture(actual, actual_temp_roots)
    return _first_difference(normalized_expected, normalized_actual)


def _render(value: Any) -> str:
    if value is MISSING:
        return "<missing>"
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
        allow_nan=False,
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare normalized AOSP Fastboot and KairosBoot traces"
    )
    parser.add_argument("--schema", required=True, type=pathlib.Path)
    parser.add_argument("--expected", required=True, type=pathlib.Path)
    parser.add_argument("--actual", required=True, type=pathlib.Path)
    parser.add_argument("--expected-temp-root", action="append", default=[])
    parser.add_argument("--actual-temp-root", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        expected = load_capture(arguments.expected, arguments.schema)
        actual = load_capture(arguments.actual, arguments.schema)
        difference = compare_captures(
            expected,
            actual,
            arguments.expected_temp_root,
            arguments.actual_temp_root,
        )
    except TraceContractError as error:
        print(f"trace contract error: {error}", file=sys.stderr)
        return 2

    if difference is not None:
        print(
            f"normalized differential mismatch at {difference.path}",
            file=sys.stderr,
        )
        print(f"expected: {_render(difference.expected)}", file=sys.stderr)
        print(f"actual:   {_render(difference.actual)}", file=sys.stderr)
        return 1

    scenario_count = len(expected["scenarios"])
    event_count = sum(len(item["events"]) for item in expected["scenarios"])
    print(f"normalized differential matched: {scenario_count} scenarios, "
          f"{event_count} events")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
