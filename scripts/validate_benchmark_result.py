#!/usr/bin/env python3
"""Validate a standalone KairosBoot hardware benchmark result."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from performance_evidence import validate_benchmark  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    data = json.loads(args.evidence.read_text(encoding="utf-8"))
    metrics = validate_benchmark(data, args.commit)
    print(
        "Benchmark satisfies the KairosBoot v1 performance gates "
        f"(single={metrics['singleDeviceDownloadCeilingUtilization']:.3f}, "
        f"fleet={metrics['batchMakespanSpeedup']:.3f}x, "
        f"fairness={metrics['minimumJainFairness']:.3f})"
    )


if __name__ == "__main__":
    main()
