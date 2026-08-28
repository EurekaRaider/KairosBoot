# KairosBoot for .NET

This package contains the first-class managed API for KairosBoot.

- `net48` supports Windows x64 and uses runtime `DllImport` interop.
- `net10.0` supports Windows, Linux, and macOS on x64 and ARM64 and uses
  source-generated `LibraryImport` interop.

Development packages are managed-only by default. A developer using one must
stage the native SDK library and its dependencies in the operating system's
native library search path, renaming or aliasing the SDK library to the
platform's `kairosboot_native` filename. Release packages include validated
KairosBoot, libusb, and platform runtime assets for all six RIDs. Boost, miniz,
and yaml-cpp remain internal native dependencies; their license texts are
included at package root, and the private static dependencies add no runtime
library.

Within the .NET/NuGet distribution, the native library is named
`kairosboot_native` so it cannot collide with the managed `KairosBoot.dll` on
case-insensitive filesystems. The installed native C/C++ SDK remains named
`kairosboot`; the NuGet-only filename does not change the stable C ABI.

The net48 package supports only x64. Its `build` and `buildTransitive` targets
copy KairosBoot, libusb, and the app-local Microsoft Visual C++ runtime beside
the application; AnyCPU and x86 builds fail before compilation. Libusb license
and dependency manifests are stored under package-root `licenses/`, not as
runtime assets.

The native runtime enumerates USB Fastboot interfaces and performs bounded,
source-streamed `download` plus `flash` operations. `FlashFileAsync` reports
preflight, selection, transport, device `FAIL`, cancellation, and transfer
certainty through `KairosBootException`; successful completion requires a
compatible physical Fastboot device.

Use `FlashOptions` for a strongly typed per-I/O deadline and pass
`IProgress<FlashProgress>` plus a `CancellationToken` to `FlashFileAsync`.
`FlashOptions.Default` and `Timeout.InfiniteTimeSpan` preserve the native
infinite timeout. The binding retains the managed progress callback until
native operation release has drained every callback.

`BootRawAsync` and the `LegacyBootOptions` overloads of `FlashRawAsync` build a
legacy Android boot header v0 image from kernel, optional ramdisk, and optional
second-stage component files. They use the same `Task`, `FlashOptions`,
`IProgress<FlashProgress>`, `CancellationToken`, and drained SafeHandle lifetime
contract on both target frameworks. This surface does not construct modern boot
headers, `vendor_boot`, or AVB metadata.

Use `UpdatePackageAsync` for a complete `update.zip`/flash-all operation.
`UpdateOptions` supplies a whole-operation deadline and an explicit `Wipe`
policy; its default has no deadline and preserves user data. Progress is
reported as `IProgress<UpdateProgress>`, and `CancellationToken` requests native
cancellation while the managed task continues draining the operation before
releasing its SafeHandle and callback state. The asynchronous method is the
first-class C# entry point on net48 and net10.0; the package also carries the
blocking native import for ABI parity with the C SDK.

Typed `GetVarAsync`, `EraseAsync`, `SetActiveAsync`, `RebootAsync`,
`ContinueBootAsync`, `OemAsync`, `RawCommandAsync`, `BootAsync`, `StageAsync`,
`UploadAsync`, and `FetchAsync` methods return `Task<CommandResult>`. They use
the same stable C ABI on both target frameworks and accept USB, TCP, or UDP
device selectors. Binary terminal payloads, ordered INFO/TEXT messages, and
upload/fetch data are copied into owned `byte[]` values before the native
result is released.

`CommandOptions.Default` applies an infinite per-I/O timeout and a 64 MiB hard
receive bound. Cancellation requests the native operation to stop and then
drains it asynchronously; pending operations do not reserve one ThreadPool
thread per device. `KairosBootException` includes the binary device message,
ordered command messages, inbound expected/transferred counts and certainty,
and whether the native session was poisoned.

`Fleet.ValidateJobFile` and `Fleet.PlanJobFile` validate and plan Fleet job
manifests without a `Context`; they never initialize USB transport, enumerate
devices, or open artifact paths. Failures throw `KairosBootException` with the
manifest path and, when known, its `line:column` diagnostics plus the platform
native code. `JobPlan` owns an immutable native snapshot; `CanonicalJson`
returns an owned copy of the canonical SDK JSON without a trailing LF and
`Sha256Hex` its lowercase hexadecimal digest, and the plan must be disposed.
