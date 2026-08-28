# KairosBoot runtime and fleet handoff status

Audit baseline: `29f19cba97d4ee1d69bebf003812648728006700`

This document supersedes the runtime and fleet execution gaps listed in
`AI_HANDOFF_UNFINISHED.md` sections 4.1 and 5. The status below is based on
source and local automated-test evidence. It is not USB HIL, throughput, soak,
or release-signing evidence.

## Audited scope

| Area | Status | Source evidence | Automated evidence |
|---|---|---|---|
| Controller-aware DATA arbitration | Implemented | `PreparedFleetActorBatch::prepare()` registers one weight-1 flow per physical device/root controller and binds a scheduler permit provider to the actor (`src/fleet/device_actor.cpp:1203-1280`). `WeightedControllerScheduler` implements thread-safe per-controller DRR, outstanding tracking, retirement and cancellation (`src/fleet/controller_scheduler.cpp:73-406`). | `tests/performance/test_main.cpp:1322-1432` covers exact DRR 1:3 and independent controller progress. `tests/performance/test_main.cpp:1434-1810` covers settlement, broker cancellation, readiness and lifetime races. |
| `TransferRing` per-chunk permits | Implemented | Each chunk acquires a scheduler-owned buffer/permit before reading and submitting; completion settles `not_submitted`, `fully_transferred`, or `partial_or_unknown` exactly once (`src/transport/transfer_ring.cpp:292-451`). USB DATA uses the configured provider and waits on provider readiness without polling the libusb event thread (`src/transport/usb_fastboot.cpp:403-598`). | `tests/performance/test_main.cpp:762-1100` covers ring/provider submission, completion, cancellation and partial certainty. `tests/performance/test_main.cpp:1434-1557` proves known-not-sent is requeued while partial/unknown retires the flow. |
| Actor lifecycle, limit and failure isolation | Implemented | `FleetCoordinator` creates exactly `min(device_count, maxParallelDevices)` workers, assigns every device at most once, drains cancellation, and applies continue/stop failure policy (`src/fleet/fleet_coordinator.cpp:121-367`). `FleetDeviceActor::execute()` is guarded against concurrent execution and owns one serial Fastboot task sequence (`src/fleet/device_actor.cpp:843-1067`). | `tests/fleet/fleet_coordinator_tests.cpp:157-366` covers exact limits including 32 and 33, one execution per actor, continue isolation and stop/drain. `tests/fleet/device_actor_tests.cpp:1308-1365` covers same-device serialization. `tests/fleet/device_actor_tests.cpp:1760-1827` runs 32 prepared DATA actors across two controllers with one isolated failure and a bounded shared buffer budget. |
| Production fastbootd reconnect | Implemented | Fleet production preparation uses `LibusbRuntime::enumerate()` and libusb preflight/runtime factories (`src/api/fleet_run.cpp:692-859`). The reconnect adapter calls verified open and adopts only the verified transport (`src/fastboot/libusb_reconnect_adapters.cpp:512-739`). Actors create `PrimitiveUpdateDevice::create_with_reconnect()` only for a prepared bootloader-to-fastbootd transition (`src/fleet/device_actor.cpp:495-610`). Reconnect is attempted only after a fully transferred `reboot-fastboot`; partial/unknown never selects discovery or offset retry, and the replacement session is revalidated and rebound to the existing permit provider before publication (`src/fastboot/primitive_update_device.cpp:547-824`). | `tests/fastboot/libusb_reconnect_adapters_tests.cpp:476-1205` covers discovery/open ownership, duplicate and changed identity, live product/mode validation, cancellation and prepared binding. `tests/fleet/device_actor_tests.cpp:1829-1933` covers actor reconnect plus DATA-provider rebinding. |
| C, C++23, C# and CLI fleet run/cancel/report | Implemented | The C ABI exposes blocking/async run, wait, cancel, terminal report extraction and report release (`include/kairosboot/kairosboot.h:735-766`, `src/api/fleet_run.cpp:1135-1340`). Report JSON is copied into immutable shared storage before a public report handle is created (`src/api/fleet_run.cpp:486-497,1282-1337`). The C++ wrapper owns move-only `Job`/`JobReport` handles (`include/kairosboot/kairosboot.hpp:807-833,998-1099`). The .NET wrapper uses `SafeHandle`, `Task`, `CancellationToken`, `IProgress<T>` and independent report handles (`bindings/dotnet/KairosBoot/FleetRun.cs:70-367`). The CLI calls the C++23 wrapper (`cli/main.cpp:2881-2940`). | `tests/c_api/fleet_run_test.c:173-439` covers success, failure/cancel report extraction, report independence, production preparation, 32-device execution and isolation. `tests/cxx_fleet_run_test.cpp:84-146` covers move-only lifecycle and reports after failure/cancel. `bindings/dotnet/KairosBoot.ContractTests/Program.cs:1423-1605` covers the public surface, cancellation drain and independent failure/cancel reports. `tests/cli/scripted_cli_test.py:567-621` covers JSON/human failure reports and CLI validation. |

## Settlement and retry contract

The scheduler and transfer ring share one ownership token for each granted
buffer. No second byte-accounting path exists in the actor or USB transport.

- `not_submitted` returns the entire chunk to the flow.
- `fully_transferred` consumes the chunk.
- `partial_or_unknown` does not return bytes, retires the flow and prevents
  guessed offset continuation or automatic replay.
- A retired/cancelled provider wakes blocked rings and grants no further
  permits.
- Controllers are scheduled independently; a blocked flow on one controller
  does not prevent an eligible flow on another controller from advancing.

These are model and integration-test assertions, not measurements of physical
USB controller fairness or bandwidth.

## Local verification at the audited baseline

The existing exact-head build tree was configured with
`CMAKE_BUILD_TYPE=Release`. The following checks passed locally:

- `device_actor`, `fleet_coordinator`, `reconnect_coordinator`,
  `libusb_reconnect_adapters`, `c_fleet_run`, `cxx_fleet_run`, `performance`
  and `cli_contract`.
- The net10.0 scripted native .NET contract suite: 432 checks, including the
  fleet public surface, asynchronous cancellation drain, and independent
  success/failure/cancel report ownership.

## Remaining acceptance gates

For the runtime/fleet scope audited here, the remaining work is external
acceptance evidence rather than another local scheduler, actor, reconnect or
language-binding implementation slice:

1. Real Windows, Linux and macOS USB HIL with representative bootloader and
   fastbootd devices, including unplug, hub reset, duplicate serial and
   re-enumeration fault injection.
2. Single-device raw bulk ceiling measurement and AOSP Fastboot comparison,
   proving the declared 90% ceiling and conditional 10% improvement gates.
3. A real 32-device, multi-root-controller run proving makespan, per-controller
   throughput and 5-second-window Jain fairness targets.
4. A 24-hour hardware soak proving no deadlock, handle/transfer leak, device
   cross-wiring, incorrect reconnect or sustained RSS growth.
5. Exact-head six-platform Release-mode CI/package validation, ABI freeze,
   signed-tag/release governance, and platform signing/notarization once
   credentials are available.

This audit does not reclassify compatibility inventory entries or claim the
whole project is release-ready. Compatibility inventory regeneration and any
non-runtime command/image gaps must be evaluated by their owning workstream.
