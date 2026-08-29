# KairosBoot release readiness

Repository audit refreshed: 2026-08-29. Exact capture commits and artifact
digests are recorded in the compatibility evidence metadata.

This audit covers R5/R6 release construction and governance contracts that can
be verified without publishing, signing credentials, GitHub Actions execution,
or USB hardware. It does not declare KairosBoot v1.0.0 ready.

## Repository-complete release contracts

| Contract | Repository evidence |
|---|---|
| Optimized native builds | All six native jobs build, test, install, and package CMake `Release`; `check_release_build.py` rejects Debug, RelWithDebInfo, missing optimization, or missing `NDEBUG`. The installed evidence is required again by `package_native.py` before archive creation. |
| Native assets | `release/expected-assets.json` requires SDK, CLI, and external symbols archives for Windows, Linux, and macOS on x64 and ARM64 (18 archives). Archive smoke tests clean-install the SDK and execute C11, C++23, and CLI consumers. |
| Managed assets | One Release-mode `KairosBoot` package targets `net48;net10.0`, carries net48 Windows x64 and net10 six-RID native runtimes, and emits both `.nupkg` and `.snupkg`. Release smoke covers all six net10 RIDs plus SDK-style and classic net48 consumers on Windows x64. |
| Supply-chain assets | The frozen 35-file contract includes source, SHA256SUMS, SPDX 2.3 SBOM, in-toto provenance, dependency source archives and licenses, third-party notices, and exact libusb 1.0.30 source plus COPYING. The workflow creates and validates these assets before publication. |
| Unsigned mode | `SIGNING_MODE=off` is fail-closed. `UNSIGNED.txt` and `signing-status.json` must declare no Authenticode, Developer ID, notarization, or checksum signature. Credentialed signing is not silently accepted. |
| Release context | The tag must be verified, annotated, allowlisted, version-matched, and point at the exact `origin/main` and checkout commit. Successful CI, Policy, and HIL workflows are required for that same commit. |
| Publication scope | Only a GitHub draft Release is assembled and published after the protected `github-release` Environment. No NuGet.org, GitHub Packages, or other registry publish command exists. |
| Environment fixtures | The repository freezes the required reviewer/self-review settings in `.github/environments/github-release.json` and the sole `v*` tag deployment policy in `.github/environments/github-release-deployment-policies.json`. Repository policy and release-tool tests validate both fixtures. |
| Hardware evidence workflow | Protected HIL automation requires independent Windows, Linux, and macOS USB/fault jobs and a Linux 32-device performance/24-hour-soak qualification job. The hosted `hil-required` job fails unless all four succeed on the same commit. |

## Release blockers at this baseline

These gates must remain blocking; none can be inferred from local tests.

1. The capability inventory has no missing or partial entries. The last merged
   baseline passed locked Platform-Tools 37.0.1 differentials on Darwin, Linux,
   and Windows across 46 normalized host/TCP/UDP scenarios, including both
   bootloader metadata rewrite and fastbootd direct paths for
   `resize-logical-partition`. Three scenarios
   remain intentionally outside the transport-only oracle: platform-dependent
   `format` bytes, device-specific `wipe-super`, and multi-session
   `update`/`flashall`. `claimCompatibility` remains false until current-source
   evidence is available on every locked platform and the release policy accepts
   the required-entry set.
2. The repository and managed package versions remain `0.1.0-dev`. Change them
   to the final identical version only after functionality and ABI are frozen;
   the release workflow rejects any tag/version mismatch.
3. The ABI v1 manifest and symbol whitelist are enforced and the public API now
   uses isolated Device objects plus explicit-device batches. The final ABI
   freeze must still be tied to the release version and pass on every native
   target.
4. Exact merged baseline `28c58cee75cd53b7efe32d867d1d3bc17133ecf4`
   passed CI run `33247608133` and Policy run `33247608098`. The current HIL
   enforcement change has not run remote CI by design; CI and Policy must pass
   once on its exact PR head and again on the exact merged `main` commit if the
   merge changes the commit.
5. The hardware lab is not configured on the public repository: the
   `KAIROSBOOT_HIL_ENABLED` repository variable, self-hosted runners, and HIL
   secrets were absent at the audit time. Configure the three protected
   platform labs and the Linux qualification lab described in
   `docs/HARDWARE_LAB.md`, then produce exact-head evidence for real USB fault
   injection, throughput, 32-device fairness/makespan, reconnect behavior, and
   the 24-hour soak.
6. The live `github-release` Environment currently has `EurekaRaider` as its
   required reviewer, `prevent_self_review: false`, admin bypass enabled, and
   the protected custom tag policy. Read these settings back again immediately
   before tagging so governance drift remains fail-closed.
7. Create the final tag only as an annotated, verified, signed `vX.Y.Z` tag by
   an allowlisted actor/email. The Environment approval, draft Release,
   attestation, asset validation, and final publication must occur on that tag.
   No usable local GPG or SSH signing identity was available at audit time, so
   this remains an external credential blocker.
8. Platform code signing and notarization remain intentionally unavailable.
   The first release may proceed only as explicitly unsigned with
   `SIGNING_MODE=off`; switching to `required` needs a separate credentialed
   implementation and checksum-signature gate.

## Final release sequence

1. Regenerate exact-source compatibility evidence, run the same 46 scenarios
   on Darwin, Linux, and Windows in CI, freeze the C ABI, and synchronize
   `version.json` and the managed package version only after hardware acceptance.
2. Merge the reviewed batch to `main`; run the six-platform CI and Policy once
   on that exact head.
3. Configure and run HIL on the same head, retaining validated evidence for the
   required performance, fault, fleet, and soak gates.
4. Read back the Release Environment reviewer, self-review, admin-bypass, and
   `v*` tag policy settings and repair any drift from the checked-in fixtures.
5. Create the verified signed tag. After the Release workflow builds all
   artifacts in Release mode, approve the protected Environment and let the
   workflow validate and publish the GitHub Release.

This audit read the live Release Environment and exact merged-baseline workflow
results. It did not run the new completion branch remotely, mutate the
Environment, run HIL, create a tag or Release, or publish to a registry.
