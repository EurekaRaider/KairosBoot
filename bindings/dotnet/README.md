# KairosBoot .NET binding

The `KairosBoot` project is one multi-target NuGet package:

- `net48`: Windows x64, `DllImport`, explicit `LPUTF8Str` parameters.
- `net10.0`: Windows/Linux/macOS x64 and ARM64, source-generated
  `LibraryImport` with UTF-8 string marshalling.

Release packages include both `build/net48` and `buildTransitive/net48`
targets. They deploy the native runtime for SDK-style `PackageReference` and
classic `packages.config` consumers. A net48 consumer must target x64;
AnyCPU/x86 builds fail with the actionable `KB.NET48.X64` error.

All native resources are owned by `SafeHandle` implementations. Managed error
objects copy UTF-8 strings before releasing their native owner.

`Context` owns process-wide discovery and shared runtime resources. Open one `Device`
per DUT with `Context.OpenDevice(...)`, then invoke every single-device command
on that object; command methods do not accept serial numbers or selectors.

Flash operations expose a type-safe per-I/O timeout, `IProgress<T>`, and
cooperative cancellation on both target frameworks:

```csharp
using var context = Context.Create();
using var device = context.OpenDevice();
var options = new FlashOptions(TimeSpan.FromSeconds(30));
var progress = new Progress<FlashProgress>(value =>
    Console.WriteLine($"{value.Stage}: {value.BytesCompleted}/{value.BytesTotal}"));

await device.FlashFileAsync(
    "system",
    "images/system.img",
    options,
    progress: progress,
    cancellationToken: cancellationToken);
```

Use `FlashOptions.Default` or `Timeout.InfiniteTimeSpan` for the native infinite
deadline. Cancellation calls `kb_operation_cancel`; disposal unregisters that
callback, releases and drains the native operation, and only then frees the
managed progress delegate state.

Legacy Android boot header v0 construction is exposed by `BootRawAsync` and
the `LegacyBootOptions` overloads of `FlashRawAsync`. Omitting the layout uses
the AOSP-compatible base, page-size, and component-offset defaults:

```csharp
var legacy = new LegacyBootOptions(
    commandLine: "console=ttyS0",
    baseAddress: 0x10000000,
    pageSize: 4096,
    kernelOffset: 0x00008000,
    ramdiskOffset: 0x01000000,
    secondOffset: 0x00f00000,
    tagsOffset: 0x00000100);

using var device = context.OpenDevice("usb:serial:DEVICE123");
await device.BootRawAsync(
    "images/kernel",
    legacy,
    new FlashOptions(TimeSpan.FromSeconds(30)),
    ramdiskPath: "images/ramdisk",
    progress: progress,
    cancellationToken: cancellationToken);
```

These APIs intentionally construct only the locked legacy boot v0 surface;
modern boot headers, `vendor_boot`, and AVB image construction are not implied.

Complete update packages use the same operation lifetime contract. The C#
first-class entry point is asynchronous on both target frameworks; the native
blocking export remains imported and ABI-tested for parity with the C SDK:

```csharp
using var context = Context.Create();
using var device = context.OpenDevice("usb:serial:DEVICE123");
var options = new UpdateOptions(TimeSpan.FromMinutes(10), wipe: false);
var progress = new Progress<UpdateProgress>(value =>
    Console.WriteLine($"{value.Stage}: {value.BytesCompleted}/{value.BytesTotal}"));

await device.UpdatePackageAsync(
    "factory/update.zip",
    options,
    progress: progress,
    cancellationToken: cancellationToken);
```

`UpdateOptions.Timeout` bounds package preflight, validation, and every update
task after the `Device` is open. `UpdateOptions.Default` uses no deadline and
preserves user data. Set `wipe: true` only when wipe-conditioned package tasks
may erase user data. Cancellation requests `kb_operation_cancel` and continues
polling until the native operation and its callbacks have drained.

The same API exposes typed asynchronous primitives for `getvar`, `erase`,
`set_active`, `reboot`, `continue`, `oem`, raw commands, `boot`, `stage`,
`upload`, and `fetch`. Every primitive returns a binary-safe `CommandResult`:

```csharp
using var context = Context.Create();
using var device = context.OpenDevice("tcp:192.0.2.10:5554");
var options = new CommandOptions(
    TimeSpan.FromSeconds(30),
    maximumReceiveBytes: 128UL * 1024UL * 1024UL);

var result = await device.FetchAsync(
    "vendor_boot",
    offset: 0,
    size: 4UL * 1024UL * 1024UL,
    options: options,
    cancellationToken: cancellationToken);

File.WriteAllBytes("vendor_boot-prefix.img", result.Data);
```

`TerminalPayload`, every ordered INFO/TEXT `CommandMessage.Payload`, upload or
fetch `Data`, and extended exception diagnostics are owned managed byte arrays;
embedded NUL and non-UTF-8 bytes are preserved. `CommandOptions.Default` uses
an infinite per-I/O timeout and a 64 MiB hard receive bound. A custom receive
bound must be positive and fit a managed byte array.

Operation waiting polls the nonblocking native `kb_operation_wait(..., 0)` and
uses task-based delays, so a fleet of pending operations does not consume one
ThreadPool thread per device. Cancellation requests native cancellation and
continues polling until the operation and callbacks are drained before any
`SafeHandle`, pinned stage-start buffer, or progress state is released.

Build and pack:

```sh
dotnet build bindings/dotnet/KairosBoot/KairosBoot.csproj -c Release
dotnet pack bindings/dotnet/KairosBoot/KairosBoot.csproj -c Release
```

`Version` and `PackageVersion` must be supplied together by release automation
so the managed assembly and NuGet metadata cannot diverge:

```sh
dotnet pack bindings/dotnet/KairosBoot/KairosBoot.csproj -c Release \
  /p:Version=1.2.3 \
  /p:PackageVersion=1.2.3
```

Development packages are managed-only when `IncludeNativeRuntimes` is omitted.
For a release package, stage these exact files beneath a clean runtime root:

```text
runtimes/win-x64/native/kairosboot_native.dll
runtimes/win-x64/native/libusb-1.0.dll
runtimes/win-x64/native/{msvcp140,msvcp140_1,msvcp140_2,msvcp140_atomic_wait}.dll
runtimes/win-x64/native/{vcruntime140,vcruntime140_1}.dll
runtimes/win-arm64/native/kairosboot_native.dll
runtimes/win-arm64/native/libusb-1.0.dll
runtimes/win-arm64/native/{msvcp140,msvcp140_1,msvcp140_2,msvcp140_atomic_wait}.dll
runtimes/win-arm64/native/{vcruntime140,vcruntime140_1}.dll
runtimes/linux-x64/native/libkairosboot_native.so
runtimes/linux-x64/native/libusb-1.0.so.0
runtimes/linux-arm64/native/libkairosboot_native.so
runtimes/linux-arm64/native/libusb-1.0.so.0
runtimes/osx-x64/native/libkairosboot_native.dylib
runtimes/osx-x64/native/libusb-1.0.0.dylib
runtimes/osx-arm64/native/libkairosboot_native.dylib
runtimes/osx-arm64/native/libusb-1.0.0.dylib
runtimes/<rid>/native/licenses/libusb/COPYING
runtimes/<rid>/native/licenses/libusb/kairosboot-libusb.json
runtimes/<rid>/native/licenses/boost/LICENSE_1_0.txt
runtimes/<rid>/native/licenses/miniz/LICENSE
```

`kairosboot_native` is an internal NuGet/P/Invoke filename. It prevents the
native library from colliding with the managed `KairosBoot.dll` on
case-insensitive filesystems. The installed C/C++ SDK remains named
`kairosboot`, and this packaging rename does not change its stable C ABI.

The Microsoft DLLs must come from the licensed app-local Visual C++ v14 runtime
for the same or a newer toolset than the KairosBoot build. Additional DLLs from
that app-local CRT directory may be staged and are included automatically.

Then pack with both release properties. The pack fails before producing a
package if any native dependency or redistribution record is missing. Runtime
DLLs remain under `runtimes/<rid>/native`; the pack moves the libusb license,
per-RID dependency metadata, Boost, and miniz licenses to package-root `licenses/`
so they are not flattened into a consuming application's output directory.
miniz is part of the native library as private static code; there is no miniz
or zlib shared library to stage for any RID.
```sh
dotnet pack bindings/dotnet/KairosBoot/KairosBoot.csproj -c Release \
  /p:Version=1.2.3 \
  /p:PackageVersion=1.2.3 \
  /p:IncludeNativeRuntimes=true \
  /p:NativeRuntimeRoot=/absolute/path/to/staged/runtimes
```

The repository contains no native package binaries. Verify the managed-only,
missing-RID, complete-layout, and version-override contracts with:

```sh
dotnet msbuild bindings/dotnet/tests/NativePackContract.proj \
  /t:Verify /p:Configuration=Release -m:1
```

Run a real local-feed package through the same multi-target consumer used by
CI. This never copies native files by hand: net10 uses NuGet RID selection and
net48 uses the package targets. On Windows, `--classic` additionally exercises
full-framework MSBuild plus `packages.config` and requires `nuget.exe` on PATH.

```sh
python3 bindings/dotnet/tests/run_package_smoke.py \
  --package /absolute/path/KairosBoot.1.2.3.nupkg \
  --framework net10.0 --rid osx-arm64

python bindings/dotnet/tests/run_package_smoke.py \
  --package C:\packages\KairosBoot.1.2.3.nupkg \
  --framework net48 --rid win-x64 --classic
```

The no-hardware contract runner targets both net48 and net10. The net10 runner
requires a native KairosBoot foundation build in the platform loader path. On
macOS, for example:

```sh
python3 scripts/prepare_libusb.py \
  --prefix "$PWD/build-deps/libusb" \
  --platform macos \
  --architecture arm64
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKAIROSBOOT_LIBUSB_ROOT="$PWD/build-deps/libusb"
cmake --build build
cmake -E copy_if_different \
  "$PWD/build/libkairosboot.dylib" \
  "$PWD/build/libkairosboot_native.dylib"
DYLD_LIBRARY_PATH="$PWD/build:$PWD/build-deps/libusb/lib" \
  dotnet run --project bindings/dotnet/KairosBoot.ContractTests \
    -c Release -f net10.0
```

The native foundation enumerates USB Fastboot interfaces and executes bounded,
source-streamed `download` plus `flash` operations. The contract runner uses an
invalid partition to verify deterministic preflight errors, including UTF-8
device identifiers and `NotSent` transfer certainty, without touching hardware.

The update binding also has a deterministic C11 shim that validates the exact
`kb_update_options_t` layout, UTF-8 marshalling, blocking-import parity,
progress, cancellation, and SafeHandle release without USB hardware. It builds
the shim with release optimization and runs net10.0 on the current platform;
Windows x64 can run both target frameworks:

```sh
python3 bindings/dotnet/KairosBoot.ContractTests/run_update_shim_tests.py

python bindings/dotnet/KairosBoot.ContractTests/run_update_shim_tests.py \
  --framework net48 --framework net10.0
```
