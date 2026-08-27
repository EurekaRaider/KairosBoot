# SPDX-License-Identifier: MIT
"""Small Draft 2020-12 validator for KairosBoot Fleet contract schemas."""

from __future__ import annotations

import datetime
import json
import math
import re
from typing import Any


MAX_SAFE_INTEGER = 9_007_199_254_740_991
_UTC_DATE_TIME = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?Z$"
)

_TYPES = {"array", "boolean", "integer", "null", "number", "object", "string"}
_KEYWORDS = {
    "$comment",
    "$defs",
    "$id",
    "$ref",
    "$schema",
    "additionalProperties",
    "allOf",
    "anyOf",
    "const",
    "else",
    "enum",
    "format",
    "if",
    "items",
    "maximum",
    "maxItems",
    "maxLength",
    "maxProperties",
    "minimum",
    "minItems",
    "minLength",
    "minProperties",
    "oneOf",
    "pattern",
    "properties",
    "required",
    "then",
    "title",
    "type",
    "uniqueItems",
}


class SchemaDefinitionError(ValueError):
    """Raised when a schema uses an invalid or unsupported construct."""


class InstanceValidationError(ValueError):
    """Raised when an instance does not satisfy a checked schema."""


def _definition(condition: bool, path: str, message: str) -> None:
    if not condition:
        raise SchemaDefinitionError(f"{path}: {message}")


def _instance(condition: bool, path: str, message: str) -> None:
    if not condition:
        raise InstanceValidationError(f"{path}: {message}")


def _resolve(root: dict[str, Any], reference: str) -> dict[str, Any]:
    _definition(reference.startswith("#/"), "$ref", "only local references are supported")
    current: Any = root
    for token in reference[2:].split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        _definition(isinstance(current, dict) and token in current, "$ref",
                    f"unresolved reference {reference}")
        current = current[token]
    _definition(isinstance(current, dict), "$ref", "reference does not name a schema")
    return current


def _check_schema_node(node: Any, root: dict[str, Any], path: str) -> None:
    _definition(isinstance(node, dict), path, "schema must be an object")
    unknown = set(node) - _KEYWORDS
    _definition(not unknown, path, f"unsupported keywords: {sorted(unknown)}")

    for keyword in ("$schema", "$id", "title", "$comment"):
        if keyword in node:
            _definition(isinstance(node[keyword], str), path,
                        f"{keyword} must be a string")

    if "$ref" in node:
        _definition(isinstance(node["$ref"], str), path, "$ref must be a string")
        _resolve(root, node["$ref"])
    if "type" in node:
        types = node["type"] if isinstance(node["type"], list) else [node["type"]]
        _definition(types and all(isinstance(value, str) and value in _TYPES for value in types),
                    path, "invalid type")
        _definition(len(types) == len(set(types)), path, "duplicate type")
    if "required" in node:
        required = node["required"]
        _definition(isinstance(required, list) and all(isinstance(value, str) for value in required),
                    path, "required must be a string array")
        _definition(len(required) == len(set(required)), path, "duplicate required field")
    if "additionalProperties" in node:
        _definition(isinstance(node["additionalProperties"], bool), path,
                    "only boolean additionalProperties is supported")
    if "properties" in node:
        _definition(isinstance(node["properties"], dict), path, "properties must be an object")
        for name, child in node["properties"].items():
            _check_schema_node(child, root, f"{path}.properties.{name}")
    if "$defs" in node:
        _definition(isinstance(node["$defs"], dict), path, "$defs must be an object")
        for name, child in node["$defs"].items():
            _check_schema_node(child, root, f"{path}.$defs.{name}")
    if "items" in node:
        _check_schema_node(node["items"], root, f"{path}.items")
    for keyword in ("allOf", "anyOf", "oneOf"):
        if keyword in node:
            children = node[keyword]
            _definition(isinstance(children, list) and children, path,
                        f"{keyword} must be a non-empty array")
            for index, child in enumerate(children):
                _check_schema_node(child, root, f"{path}.{keyword}[{index}]")
    for keyword in ("if", "then", "else"):
        if keyword in node:
            _check_schema_node(node[keyword], root, f"{path}.{keyword}")
    if "enum" in node:
        _definition(isinstance(node["enum"], list) and node["enum"], path,
                    "enum must be a non-empty array")
    if "uniqueItems" in node:
        _definition(isinstance(node["uniqueItems"], bool), path,
                    "uniqueItems must be a boolean")
    if "pattern" in node:
        _definition(isinstance(node["pattern"], str), path, "pattern must be a string")
        try:
            re.compile(node["pattern"])
        except re.error as error:
            raise SchemaDefinitionError(f"{path}: invalid pattern: {error}") from error
    if "format" in node:
        _definition(node["format"] == "date-time", path, "unsupported format")
    for keyword in ("minimum", "maximum"):
        if keyword in node:
            value = node[keyword]
            _definition(isinstance(value, int) and not isinstance(value, bool), path,
                        f"{keyword} must be an integer")
    for keyword in ("minItems", "maxItems", "minLength", "maxLength",
                    "minProperties", "maxProperties"):
        if keyword in node:
            value = node[keyword]
            _definition(isinstance(value, int) and not isinstance(value, bool)
                        and value >= 0, path,
                        f"{keyword} must be a non-negative integer")
    for minimum, maximum in (("minimum", "maximum"),
                             ("minItems", "maxItems"),
                             ("minLength", "maxLength"),
                             ("minProperties", "maxProperties")):
        if minimum in node and maximum in node:
            _definition(node[minimum] <= node[maximum], path,
                        f"{minimum} exceeds {maximum}")


def check_schema(schema: dict[str, Any]) -> None:
    """Check syntax, local references, and the supported keyword vocabulary."""

    _definition(isinstance(schema, dict), "$", "root schema must be an object")
    _check_schema_node(schema, schema, "$")


def _json_equal(left: Any, right: Any) -> bool:
    if isinstance(left, bool) or isinstance(right, bool):
        return type(left) is type(right) and left == right
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return left == right
    return type(left) is type(right) and left == right


def _type_matches(instance: Any, expected: str) -> bool:
    if expected == "null":
        return instance is None
    if expected == "boolean":
        return isinstance(instance, bool)
    if expected == "integer":
        return isinstance(instance, int) and not isinstance(instance, bool)
    if expected == "number":
        return isinstance(instance, (int, float)) and not isinstance(instance, bool)
    if expected == "string":
        return isinstance(instance, str)
    if expected == "array":
        return isinstance(instance, list)
    if expected == "object":
        return isinstance(instance, dict)
    raise AssertionError(expected)


def _ensure_ijson(instance: Any, path: str) -> None:
    if isinstance(instance, bool) or instance is None:
        return
    if isinstance(instance, str):
        _instance(not any(0xD800 <= ord(value) <= 0xDFFF for value in instance),
                  path, "string contains an unpaired Unicode surrogate")
        return
    if isinstance(instance, int):
        _instance(-MAX_SAFE_INTEGER <= instance <= MAX_SAFE_INTEGER, path,
                  "integer is outside the interoperable JSON range")
        return
    if isinstance(instance, float):
        _instance(math.isfinite(instance), path, "non-finite JSON number")
        raise InstanceValidationError(f"{path}: floating-point values are outside the contract")
    if isinstance(instance, list):
        for index, value in enumerate(instance):
            _ensure_ijson(value, f"{path}[{index}]")
        return
    if isinstance(instance, dict):
        for key, value in instance.items():
            _instance(isinstance(key, str), path, "object key is not a string")
            _instance(not any(0xD800 <= ord(character) <= 0xDFFF
                              for character in key),
                      path, "object key contains an unpaired Unicode surrogate")
            _ensure_ijson(value, f"{path}.{key}")
        return
    raise InstanceValidationError(f"{path}: unsupported JSON value")


def _is_valid(instance: Any, schema: dict[str, Any], root: dict[str, Any], path: str) -> bool:
    try:
        _validate(instance, schema, root, path)
        return True
    except InstanceValidationError:
        return False


def _validate(instance: Any, schema: dict[str, Any], root: dict[str, Any], path: str) -> None:
    if "$ref" in schema:
        _validate(instance, _resolve(root, schema["$ref"]), root, path)
    if "type" in schema:
        expected = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        _instance(any(_type_matches(instance, value) for value in expected), path,
                  f"expected type {expected}")
    if "const" in schema:
        _instance(_json_equal(instance, schema["const"]), path, "const mismatch")
    if "enum" in schema:
        _instance(any(_json_equal(instance, value) for value in schema["enum"]), path,
                  "value is not in enum")
    if "allOf" in schema:
        for child in schema["allOf"]:
            _validate(instance, child, root, path)
    if "anyOf" in schema:
        _instance(any(_is_valid(instance, child, root, path) for child in schema["anyOf"]),
                  path, "no anyOf branch matched")
    if "oneOf" in schema:
        matches = sum(_is_valid(instance, child, root, path) for child in schema["oneOf"])
        _instance(matches == 1, path, f"expected one oneOf match, got {matches}")
    if "if" in schema:
        branch = schema.get("then") if _is_valid(instance, schema["if"], root, path) \
            else schema.get("else")
        if branch is not None:
            _validate(instance, branch, root, path)

    if isinstance(instance, dict):
        if "minProperties" in schema:
            _instance(len(instance) >= schema["minProperties"], path,
                      "too few properties")
        if "maxProperties" in schema:
            _instance(len(instance) <= schema["maxProperties"], path,
                      "too many properties")
        required = schema.get("required", [])
        for name in required:
            _instance(name in instance, path, f"missing required property {name}")
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            unknown = set(instance) - set(properties)
            _instance(not unknown, path, f"unknown properties: {sorted(unknown)}")
        for name, child in properties.items():
            if name in instance:
                _validate(instance[name], child, root, f"{path}.{name}")
    if isinstance(instance, list):
        if "minItems" in schema:
            _instance(len(instance) >= schema["minItems"], path, "too few items")
        if "maxItems" in schema:
            _instance(len(instance) <= schema["maxItems"], path, "too many items")
        if schema.get("uniqueItems"):
            encoded = [json.dumps(value, ensure_ascii=False, sort_keys=True,
                                  separators=(",", ":"), allow_nan=False)
                       for value in instance]
            _instance(len(encoded) == len(set(encoded)), path, "items are not unique")
        if "items" in schema:
            for index, value in enumerate(instance):
                _validate(value, schema["items"], root, f"{path}[{index}]")
    if isinstance(instance, str):
        if "minLength" in schema:
            _instance(len(instance) >= schema["minLength"], path, "string is too short")
        if "maxLength" in schema:
            _instance(len(instance) <= schema["maxLength"], path, "string is too long")
        if "pattern" in schema:
            match = re.search(schema["pattern"], instance)
            if (match is not None and schema["pattern"].startswith("^")
                    and schema["pattern"].endswith("$")):
                # Python's `$` also matches immediately before a final newline,
                # unlike ECMAScript without multiline mode. Every anchored Fleet
                # pattern is intended to cover the complete JSON string.
                match = match if match.span() == (0, len(instance)) else None
            _instance(match is not None, path, "string does not match pattern")
        if schema.get("format") == "date-time":
            match = _UTC_DATE_TIME.fullmatch(instance)
            _instance(match is not None, path, "invalid UTC date-time")
            try:
                datetime.datetime(
                    int(match.group(1)), int(match.group(2)), int(match.group(3)),
                    int(match.group(4)), int(match.group(5)), int(match.group(6)),
                    tzinfo=datetime.timezone.utc,
                )
            except ValueError as error:
                raise InstanceValidationError(f"{path}: invalid date-time") from error
    if isinstance(instance, int) and not isinstance(instance, bool):
        if "minimum" in schema:
            _instance(instance >= schema["minimum"], path, "number is below minimum")
        if "maximum" in schema:
            _instance(instance <= schema["maximum"], path, "number is above maximum")


def validate(instance: Any, schema: dict[str, Any]) -> None:
    """Validate an I-JSON instance against a checked schema."""

    check_schema(schema)
    _ensure_ijson(instance, "$")
    _validate(instance, schema, schema, "$")
