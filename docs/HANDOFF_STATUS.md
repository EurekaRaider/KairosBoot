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
| Controller-aware scheduling | Implemented; hardware proof pending | Explicit `DeviceBatch` execution groups USB devices by the root-controller identity captured when each `Device` is opened, gives every device a weighted scheduler flow, and injects scheduler-owned DATA permits into flash, boot, stage/signature, format, update, wipe-super, and reconnect-capable fastbootd sessions. TCP/UDP devices remain independent. Real multi-controller hardware acceptance remains release work. |
| External task files | Removed | Multi-device execution is expressed only through explicit device objects and batch APIs; no runtime task-file parser, schema, artifact planner, or job actor remains. |
| AOSP differential | 46 scenarios matched on all three hosts | The last merged baseline passed the locked Platform-Tools 37.0.1 differential on Darwin, Linux, and Windows. KairosBoot produced identical normalized traces for 46 host/TCP/UDP scenarios, including bootloader-side `super` metadata rewrite and fastbootd direct resize. The final change must repeat this check on its exact head. |
| HIL enforcement | Implemented; labs absent | The protected HIL workflow now requires separate Windows, Linux, and macOS real-USB/fault evidence plus an independent Linux 32-device performance and 24-hour-soak qualification. Strict validators reject synthetic, stale, partial, duplicated, or invented evidence. |

## Local evidence

- C11, C++23, and C# contract tests cover the public device and batch surface.
- Scripted TCP/UDP and USB adapter tests cover command serialization, identity
  verification, cancellation, reconnect safety, and error propagation.
- Performance tests cover controller scheduling and transfer-ring behavior;
  the public C test also rejects two handles that resolve to the same physical
  target, preventing duplicate scheduler flows for one DUT.
- Repository policy rejects product/runtime task files and rejects restoration
  of the removed manifest/job pipeline. GitHub workflow configuration remains
  under `.github/` because GitHub Actions requires it.
- The last merged baseline (`28c58cee75cd53b7efe32d867d1d3bc17133ecf4`)
  passed exact-head CI run `33247608133` and Policy run `33247608098`, including
  six native targets, managed consumers, CodeQL, sanitizers, and the official
  differential on Darwin, Linux, and Windows.
- The bootloader `resize-logical-partition` path fetches the bounded `super`
  prefix, validates primary/backup liblp geometry and metadata, emits the same
  metadata-only sparse image as locked AOSP Fastboot, and flashes `super`.
  Fastbootd retains the direct protocol command after mode/slot/logical checks.

## Remaining release acceptance

The remaining gates require external hardware or protected release state:

1. Windows, Linux, and macOS USB HIL/fault-injection runs.
2. Raw bulk ceiling and AOSP Fastboot performance comparisons.
3. A real 32-device, multi-root-controller throughput/fairness run.
4. A 24-hour hardware soak.
5. A verified signed release tag, protected Release approval, and final
   governance readback.

Unit tests and scripted devices do not substitute for those hardware results.
The runner labels, fixed harness entry points, and evidence contracts are
documented in `docs/HARDWARE_LAB.md`.
