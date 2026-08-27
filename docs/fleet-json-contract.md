# Fleet JSON contract

KairosBoot defines two separate versioned Fleet documents for future public
Fleet APIs. They are not interchangeable; this contract does not itself expose
an ABI or claim that Fleet execution is implemented.

## JobPlan v1

`JobPlan` is an immutable, device-independent normalization of a
`kairosboot.io/v1` manifest. Planning parses the manifest and resolves its
defaults and references, but it does not enumerate devices or open artifacts.
The run preflight remains responsible for device uniqueness, product checks,
artifact reads, and SHA-256 verification before any destructive command.

The plan contains no job identifier or timestamp. The same manifest bytes and
API version therefore produce the same document. Contract fixtures use JSON,
which is also valid YAML 1.2, so tests can verify the source-to-plan
normalization without adding a second YAML parser. JSON uses the KairosBoot
canonical profile: UTF-8 without a BOM, fixed ASCII schema keys in ascending
order, no insignificant whitespace, no floating-point values, and integers in
the interoperable range `[-(2^53-1), 2^53-1]`. Future SDK JSON getters will not
include a trailing newline; the CLI will write exactly one trailing newline.

Artifact, target, selector, and step arrays preserve manifest order. Every
artifact, target, and step also carries its zero-based index so structured API
getters and JSON consumers use the same identity.

Artifact IDs and target names are unique. A serial or USB path may be owned by
only one target, and every flash step references an artifact ID present in the
same plan. These reference and ownership rules are semantic validation beyond
what JSON Schema alone can express. Manifest artifact SHA-256 values accept
either hex case and are normalized to lowercase in the plan.

## JobReport v1

`JobReport` is an immutable snapshot of one running or terminal job. It always
references the SHA-256 of the canonical `JobPlan` payload, excluding the CLI's
trailing newline. A report extracted from a job owns its storage and remains
valid after the job handle is released.

A terminal report is available for success, failure, partial failure, and
cancellation. Blocking run APIs return that report even when their status is
`KB_E_DEVICE_FAIL` or `KB_E_CANCELLED`. Job-level validation or integrity
failures are represented by the top-level `error`; device and step failures
are represented at their corresponding scopes.

`running` snapshots have a null `finishedAt`. Terminal states have a UTC RFC
3339-subset `finishedAt` ending in `Z` and contain no pending or running device
or step. Fractional seconds may have any positive number of digits; leap-second
spellings are excluded. Pending work stopped by policy is `skipped`;
caller-requested or transport-drain cancellation is `cancelled`.
`KB_E_CANCELLED` is valid only with cancelled job, device, or step state; it
cannot be wrapped as a failure to bypass cancellation-wins publication.

| Job state | Required summary/error semantics | Blocking status |
|---|---|---|
| `running` | Any in-progress mix; top-level error is null | Not terminal |
| `succeeded` | At least one device, all succeeded; top-level error is null | `KB_OK` |
| `partially_failed` | At least one succeeded and at least one failed; skipped devices may also exist; top-level error is null | `KB_E_DEVICE_FAIL` |
| `failed` | No device succeeded and at least one failed, or a top-level preflight/integrity error exists | Top-level error code, otherwise `KB_E_DEVICE_FAIL` |
| `cancelled` | Cancellation wins publication even after earlier success/failure; top-level error is `KB_E_CANCELLED` | `KB_E_CANCELLED` |

Step timestamp and error fields use the following local rules:

- `pending`: no timestamps and no error;
- `running`: `startedAt`, no `finishedAt`, and no error;
- `succeeded`: both timestamps and no error;
- `failed`: both timestamps and an error;
- `cancelled`: optional `startedAt`, a `finishedAt`, and `KB_E_CANCELLED`;
- `skipped`: no `startedAt`, a `finishedAt`, and no error.

Device state controls its device-level error: failed and cancelled devices
carry the matching error; other device states do not. A failed device may fail
during product/open preflight before any individual step fails, in which case
its unstarted steps are skipped. Device- and step-level errors carry the exact
parent device identifier.

Per-device protocol execution is strictly serial. A running device has a
succeeded prefix, exactly one running step, and a pending suffix. An execution
failure or cancellation has a succeeded prefix, exactly one failed or
cancelled step, and a skipped suffix. A device-scope preflight failure may have
all steps skipped. Pending, succeeded, and skipped devices contain only their
corresponding step state. The timestamp interval of an executed step cannot
overlap the next executed step on the same device. Step completion boundaries,
including cancelled and skipped markers, are nondecreasing in plan order.

Flash step byte counters are the aggregate Fastboot `DATA` payload bytes for
that step, not the partition's logical size. They are non-null safe integers
with `bytesTransferred <= bytesTotal`; succeeded steps require equality, while
pending and skipped steps require zero transferred bytes. Non-DATA operations
carry null byte counts.

All report JSON uses the same canonical UTF-8 profile as plans. The summary
counts must equal the device states and must add up to `total`. The report
schema has no 256-device total limit; `maxParallelDevices` limits concurrent
actors, not the number of devices represented by a job.

Plan/report validation is cross-document, not hash-only. Every report device
must match exactly one plan selector and name that target; its step definitions
must equal the target's immutable plan steps. A non-failed device must report
the expected product. A failed product preflight may retain the mismatching or
unavailable observed product for diagnosis, but every device step must remain
skipped so the report cannot imply destructive work began after the mismatch.

## C API ownership

Future C handles will follow the existing KairosBoot ABI rules:

- plan and report handles are opaque, immutable, and caller-owned;
- JSON getters return borrowed NUL-terminated UTF-8 owned by the parent handle;
- extracting a report from a job returns a new owned snapshot;
- releasing a job never invalidates an extracted report;
- every options structure carries `struct_size` and `api_version`;
- blocking run is implemented by async run plus wait and report extraction.

The JSON `nativeCode` field is limited to the signed 32-bit range so it maps
losslessly to the stable C ABI. Byte counters, indices, and summary counts use
the wider interoperable JSON integer range.
