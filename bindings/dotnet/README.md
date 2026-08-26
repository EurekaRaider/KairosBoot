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

The no-hardware contract runner requires a native KairosBoot foundation build in
the platform loader path. On macOS, for example:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
DYLD_LIBRARY_PATH="$PWD/build" \
  dotnet run --project bindings/dotnet/KairosBoot.ContractTests -c Release
```

The current native foundation returns `NotSupported` for enumeration and flash.
The contract runner verifies that this is surfaced as an exception, including
UTF-8 device identifiers and `NotSent` transfer certainty.
