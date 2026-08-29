# KairosBoot current implementation status

This document records the current architecture after the public SDK moved to
device-owned sessions and explicit multi-device batches. It replaces older
handoff notes that described file-driven fleet jobs.

## Current SDK model

| Area | Status | Current contract |
|---|---|---|
| Device ownership | Implemented | `kb_device_t`, C++ `Device`, and C# `Device` each own one isolated Fastboot session. Operations no longer accept a context plus device selector. |
| Multi-device execution | Implemented | `kb_device_batch_*`, C++ `DeviceBatch`, and C# `DeviceBatch` accept explicit device objects, enforce uniqueness, support bounded parallelism, cancellation, continue/stop-on-error behavior, ordered per-device results, and JSON reports. |
| CLI compatibility | Implemented | The CLI exposes Fastboot-compatible commands and options. It does not expose custom planning or job-file commands. |
| USB identity and reconnect | Implemented | `src/fastboot/device_connection.*` exclusively opens the selected interface, records the verified physical identity, probes live product/mode, and creates the sealed reconnect binding used by update operations. |
| Controller-aware scheduling | Component implemented; batch integration pending | `WeightedControllerScheduler` remains independently tested, but the explicit `DeviceBatch` path currently provides bounded parallelism rather than controller-aware arbitration. Wiring it into the public batch path and proving it on real multi-controller hardware remain release work. |
| External task files | Removed | Multi-device execution is expressed only through explicit device objects and batch APIs; no runtime task-file parser, schema, artifact planner, or job actor remains. |

## Local evidence

- C11, C++23, and C# contract tests cover the public device and batch surface.
- Scripted TCP/UDP and USB adapter tests cover command serialization, identity
  verification, cancellation, reconnect safety, and error propagation.
- Performance tests cover controller scheduling and transfer-ring behavior.
- Repository policy rejects product/runtime task files and rejects restoration
  of the removed manifest/job pipeline. GitHub workflow configuration remains
  under `.github/` because GitHub Actions requires it.

## Remaining release acceptance

The remaining gates require external hardware or protected release state:

1. Windows, Linux, and macOS USB HIL/fault-injection runs.
2. Raw bulk ceiling and AOSP Fastboot performance comparisons.
3. A real 32-device, multi-root-controller throughput/fairness run.
4. A 24-hour hardware soak.
5. Protected Release workflow, signing-policy, and governance verification.

Unit tests and scripted devices do not substitute for those hardware results.
