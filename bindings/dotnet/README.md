# KairosBoot .NET binding

The `KairosBoot` project is one multi-target NuGet package:

- `net48`: Windows x64, `DllImport`, explicit `LPUTF8Str` parameters.
- `net10.0`: Windows/Linux/macOS x64 and ARM64, source-generated
  `LibraryImport` with UTF-8 string marshalling.

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
runtimes/win-x64/native/kairosboot.dll
runtimes/win-arm64/native/kairosboot.dll
runtimes/linux-x64/native/libkairosboot.so
runtimes/linux-arm64/native/libkairosboot.so
runtimes/osx-x64/native/libkairosboot.dylib
runtimes/osx-arm64/native/libkairosboot.dylib
```

Then pack with both release properties. The pack fails before producing a
package if any RID is missing; additional files below each `native/` directory
are included by the `native/**` rule.

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

The no-hardware contract runner targets both net48 and net10. The net10 runner
requires a native KairosBoot foundation build in the platform loader path. On
macOS, for example:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
DYLD_LIBRARY_PATH="$PWD/build" \
  dotnet run --project bindings/dotnet/KairosBoot.ContractTests \
    -c Release -f net10.0
```

The current native foundation returns `NotSupported` for enumeration and flash.
The contract runner verifies that this is surfaced as an exception, including
UTF-8 device identifiers and `NotSent` transfer certainty.
