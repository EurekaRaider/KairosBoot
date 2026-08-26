# KairosBoot for .NET

This package contains the first-class managed API for KairosBoot.

- `net48` supports Windows x64 and uses runtime `DllImport` interop.
- `net10.0` supports Windows, Linux, and macOS on x64 and ARM64 and uses
  source-generated `LibraryImport` interop.

Development packages are managed-only by default. A developer using one must
stage the native SDK library and its dependencies in the operating system's
native library search path, renaming or aliasing the SDK library to the
platform's `kairosboot_native` filename. Release packages include validated
KairosBoot, libusb, and platform runtime assets for all six RIDs. Boost and
miniz remain internal native dependencies; their license texts are included at
package root, and miniz adds no runtime library because it is linked statically.

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
