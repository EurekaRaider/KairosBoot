# KairosBoot

KairosBoot is an early-stage, cross-platform host SDK and CLI for the standard
Android Fastboot protocol. The project targets a stable C11 API, a C++23 RAII
wrapper, .NET bindings, and high-throughput multi-device flashing.

> [!IMPORTANT]
> KairosBoot is under active development and is not yet ready for production
> flashing. Do not use it on devices containing irreplaceable data.

## Planned platforms

- Windows 10/11 on x64 and ARM64
- Linux on x64 and ARM64
- macOS 14 or newer on Intel and Apple silicon

## Planned interfaces

- A first-class C11 SDK with blocking and asynchronous operations
- A header-only C++23 RAII wrapper
- C# bindings for .NET Framework 4.8 and .NET 10
- The `kairosboot` command-line tool

The current implementation milestone includes the stable API foundation,
Fastboot response state machine, TCP v1 and reliable UDP v1 transports, a
bidirectional asynchronous libusb Fastboot session, public USB device
enumeration, transfer-ring scheduling, sparse-image validation, and no-hardware
test doubles. The transport cores are not yet wired to the public destructive
device commands.

## Build the foundation milestone

KairosBoot requires CMake 3.22 or newer, Python 3, `make`, and a compiler with
C++23 support. Release builds use exactly libusb 1.0.30 as a dynamically linked
dependency. Prepare it from the locked, hash-verified archives before
configuring (choose `linux`, `macos`, or `windows` and `x64` or `arm64`):

```sh
python3 scripts/prepare_libusb.py \
  --prefix "$PWD/build-deps/libusb" \
  --platform macos \
  --architecture arm64
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKAIROSBOOT_LIBUSB_ROOT="$PWD/build-deps/libusb"
cmake --build build
ctest --test-dir build --output-on-failure
```

Official release assets are always compiled with the CMake `Release`
configuration. `KAIROSBOOT_RELEASE_SYMBOLS=ON` adds symbol information without
removing Release optimization; CI then publishes separate PDB, dSYM, or GNU
debug files and packages stripped SDK, CLI, and NuGet runtimes.

The installed SDK/CLI contains the matching libusb runtime, LGPL license, and a
dependency manifest. USB enumeration is active; flashing still returns
`KB_E_NOT_SUPPORTED` and never reports a successful flash. Check the runtime
and currently visible Fastboot interfaces with:

```sh
./build/kairosboot doctor --json
```

## API quick start

The C11 API is declared in one header and owns all returned handles:

```c
#include <kairosboot/kairosboot.h>

kb_context_t *context = NULL;
kb_error_t *error = NULL;
if (kb_context_create(NULL, &context, &error) != KB_OK) {
  /* kb_error_message(error) describes the failure. */
  kb_error_release(error);
  return 1;
}
kb_context_release(context);
```

The header-only C++23 wrapper provides move-only RAII handles and
`std::expected` results:

```cpp
#include <kairosboot/kairosboot.hpp>

auto context = kairosboot::Context::create();
if (!context) {
  return 1;
}
```

Installed CMake packages export `KairosBoot::C` and `KairosBoot::Cxx`.

## Contributing

External contributions are accepted through pull requests. All changes to
`main` require review by the repository owner. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

KairosBoot original source code is licensed under the [MIT License](LICENSE).
Third-party components retain their respective licenses.
