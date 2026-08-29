# KairosBoot release readiness

Repository audit baseline: `906c9b25604b62ce61cd647393d11afae3faa773`

This audit covers R5/R6 release construction and governance contracts that can
be verified without publishing, signing credentials, GitHub Actions execution,
or USB hardware. It does not declare KairosBoot v1.0.0 ready.

## Repository-complete release contracts

| Contract | Repository evidence |
|---|---|
| Optimized native builds | All six native jobs build, test, install, and package CMake `Release`; `check_release_build.py` rejects Debug, RelWithDebInfo, missing optimization, or missing `NDEBUG`. The installed evidence is required again by `package_native.py` before archive creation. |
| Native assets | `release/expected-assets.json` requires SDK, CLI, and external symbols archives for Windows, Linux, and macOS on x64 and ARM64 (18 archives). Archive smoke tests clean-install the SDK and execute C11, C++23, and CLI consumers. |
| Managed assets | One Release-mode `KairosBoot` package targets `net48;net10.0`, carries net48 Windows x64 and net10 six-RID native runtimes, and emits both `.nupkg` and `.snupkg`. Release smoke covers all six net10 RIDs plus SDK-style and classic net48 consumers on Windows x64. |
| Supply-chain assets | The frozen 37-file contract includes source, SHA256SUMS, SPDX 2.3 SBOM, in-toto provenance, dependency source archives and licenses, third-party notices, and exact libusb 1.0.30 source plus COPYING. The workflow creates and validates these assets before publication. |
| Unsigned mode | `SIGNING_MODE=off` is fail-closed. `UNSIGNED.txt` and `signing-status.json` must declare no Authenticode, Developer ID, notarization, or checksum signature. Credentialed signing is not silently accepted. |
| Release context | The tag must be verified, annotated, allowlisted, version-matched, and point at the exact `origin/main` and checkout commit. Successful CI, Policy, and HIL workflows are required for that same commit. |
| Publication scope | Only a GitHub draft Release is assembled and published after the protected `github-release` Environment. No NuGet.org, GitHub Packages, or other registry publish command exists. |
| Environment fixtures | The repository freezes the required reviewer/self-review settings in `.github/environments/github-release.json` and the sole `v*` tag deployment policy in `.github/environments/github-release-deployment-policies.json`. Repository policy and release-tool tests validate both fixtures. |

## Release blockers at this baseline

These gates must remain blocking; none can be inferred from local tests.

1. The capability inventory has no missing entries. The 25 formerly partial
   optional image entries now have per-entry execution coverage, but
   `claimCompatibility` must remain false until official differential evidence
   covers the required-entry set (the current frozen capture covers 22 of 87
   required entries and leaves 8 candidate scenarios uncovered).
2. The repository and managed package versions remain `0.1.0-dev`. Change them
   to the final identical version only after functionality and ABI are frozen;
   the release workflow rejects any tag/version mismatch.
3. The ABI v1 manifest and symbol whitelist are enforced, but the final ABI
   freeze must occur after all remaining public C API work is complete and then
   pass on every native target.
4. CI and Policy passed on exact `main` commit
   `906c9b25604b62ce61cd647393d11afae3faa773`. Any later compatibility,
   version, ABI, or release batch must pass both workflows again on its exact
   merged commit.
5. The hardware lab is not configured on the public repository: the
   `KAIROSBOOT_HIL_ENABLED` repository variable was absent and no HIL workflow
   runs existed at the audit time. Configure the protected self-hosted lab and
   produce exact-head evidence for real USB fault injection, throughput,
   32-device fairness/makespan, reconnect behavior, and the 24-hour soak.
6. The live `github-release` Environment currently has `EurekaRaider` as its
   required reviewer, `prevent_self_review: false`, admin bypass enabled, and
   the protected custom tag policy. Read these settings back again immediately
   before tagging so governance drift remains fail-closed.
7. Create the final tag only as an annotated, verified, signed `vX.Y.Z` tag by
   an allowlisted actor/email. The Environment approval, draft Release,
   attestation, asset validation, and final publication must occur on that tag.
8. Platform code signing and notarization remain intentionally unavailable.
   The first release may proceed only as explicitly unsigned with
   `SIGNING_MODE=off`; switching to `required` needs a separate credentialed
   implementation and checksum-signature gate.

## Final release sequence

1. Expand official differential coverage to the required-entry set, regenerate
   the compatibility inventory, freeze the C ABI, and synchronize
   `version.json` and the managed package version.
2. Merge the reviewed batch to `main`; run the six-platform CI and Policy once
   on that exact head.
3. Configure and run HIL on the same head, retaining validated evidence for the
   required performance, fault, fleet, and soak gates.
4. Read back the Release Environment reviewer, self-review, admin-bypass, and
   `v*` tag policy settings and repair any drift from the checked-in fixtures.
5. Create the verified signed tag. After the Release workflow builds all
   artifacts in Release mode, approve the protected Environment and let the
   workflow validate and publish the GitHub Release.

This audit read the live Release Environment and prior exact-head workflow
results. It did not trigger remote CI, mutate the Environment, run HIL, create
a tag or Release, or publish to a registry.
