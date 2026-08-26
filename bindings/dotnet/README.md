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

Build and pack:

```sh
dotnet build bindings/dotnet/KairosBoot/KairosBoot.csproj -c Release
dotnet pack bindings/dotnet/KairosBoot/KairosBoot.csproj -c Release
```

`PackageVersion` can be supplied by the release workflow without editing the
project:

```sh
dotnet pack bindings/dotnet/KairosBoot/KairosBoot.csproj -c Release \
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
DLLs remain under `runtimes/<rid>/native`; the pack moves the libusb license and
per-RID manifests to package-root `licenses/libusb/` so they are not flattened
into a consuming application's output directory.

```sh
dotnet pack bindings/dotnet/KairosBoot/KairosBoot.csproj -c Release \
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

The current native foundation enumerates USB Fastboot interfaces. Flash still
returns `NotSupported`; the contract runner verifies that this is surfaced as
an exception, including UTF-8 device identifiers and `NotSent` transfer
certainty.
