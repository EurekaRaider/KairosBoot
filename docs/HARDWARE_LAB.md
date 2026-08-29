# KairosBoot hardware qualification contract

KairosBoot does not accept scripted transports or hand-authored JSON as release
hardware evidence. The protected `HIL` workflow must run against real Fastboot
devices and a lab-controlled fault-injection harness on the exact candidate
commit.

## Required protected runners

Register self-hosted runners with all labels shown for their role:

| Role | Required labels | Harness entry point |
|---|---|---|
| Linux USB/fault | `self-hosted`, `kairosboot-hil`, `linux` | `/opt/kairosboot-hil/run platform` |
| macOS USB/fault | `self-hosted`, `kairosboot-hil`, `macos` | `/opt/kairosboot-hil/run platform` |
| Windows USB/fault | `self-hosted`, `kairosboot-hil`, `windows` | `C:\KairosBoot-HIL\run.ps1 platform` |
| Linux qualification | `self-hosted`, `kairosboot-hil`, `kairosboot-qualification`, `linux` | `/opt/kairosboot-hil/run qualification` |

Keep these runners unavailable to pull requests and forks. The workflow has no
`pull_request` or `pull_request_target` trigger; it runs only on a protected
manual dispatch or schedule with read-only repository permissions.

## Platform USB and fault evidence

Each platform harness receives the checked-out source path, output path, and
the expected `--os` value. It must build and exercise that exact checkout, then
write schema-version-1 JSON accepted by
`scripts/validate_usb_hil_evidence.py`. The evidence contains hashed lab,
operator, machine, and device identities; unique physical USB paths; harness
version/source digest; and per-scenario raw-evidence digests.

Every attempt must succeed for every required scenario:

- device-side download hash, partial I/O, short packet, ZLP, STALL, NAK, timeout
- unplug, Hub reset, process-exit cleanup, and fastbootd re-enumeration
- duplicate serial, error response, and out-of-order callback handling

The validator rejects stale commits, the wrong host OS, synthetic evidence,
missing or duplicated scenarios, unknown devices, failed attempts, and extra
fields. Raw logs and captures referenced by each `evidenceSha256` must be
retained by the lab under its access and privacy policy.

## Linux 32-device qualification

The qualification runner must expose at least 32 unique physical Fastboot
devices across the controller topology used for acceptance. Its harness must
build CMake `Release`, compare the locked AOSP Fastboot binary, measure raw bulk
ceilings, execute single- and multi-device workloads, and complete the full
24-hour soak. It writes JSON accepted by `scripts/validate_hil_evidence.py`.

That validator recomputes and enforces the release thresholds: single-device
ceiling utilization, host-bound gain where headroom exists, no significant
ceiling-bound regression, 32-device makespan, per-controller aggregate
utilization, five-second Jain fairness, device uniqueness, and zero soak
deadlocks, leaks, misrouting, sustained RSS growth, or duration shortfall.

## Running the protected gate

1. Install and validate the lab-owned harness at the fixed entry points above.
2. Register all four runner roles and confirm that no untrusted repository can
   target them.
3. Set the repository variable `KAIROSBOOT_HIL_ENABLED=true`.
4. Dispatch `HIL` for the exact protected `main` commit intended for release.
5. Preserve the four uploaded evidence artifacts and require `hil-required` to
   succeed on that same commit.

Do not enable the variable before every runner, DUT, fault injector, and raw-log
retention path is ready. A skipped, queued, cancelled, or partially successful
job keeps `hil-required` red and therefore blocks the Release workflow.
