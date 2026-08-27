<div align="center">

# KairosBoot

**A cross-platform Fastboot SDK and CLI engineered for direct integration.**

![Version](https://img.shields.io/badge/version-0.1.0--dev-111827)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-2563EB)](#target-platforms)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://isocpp.org/)
[![CMake](https://img.shields.io/badge/CMake-3.22%2B-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![.NET](https://img.shields.io/badge/.NET-4.8%20%7C%2010.0-512BD4?logo=dotnet&logoColor=white)](https://dotnet.microsoft.com/)
[![License](https://img.shields.io/badge/license-MIT-00A86B)](LICENSE)

KairosBoot is a host-side library and command-line tool for the standard
Android Fastboot protocol. Its long-term direction is a single in-process
runtime for C, C++, .NET, and CLI consumers, with measured high-throughput and
multi-device operation across Windows, Linux, and macOS.

**KairosBoot is in early development.** The current milestone provides the SDK
foundation, diagnostics, USB enumeration, transport internals, and test
infrastructure—not a production-ready replacement for AOSP Fastboot.

[Why](#why-kairosboot) | [Status](#current-status) |
[Quick Start](#quick-start) | [API](#api-surface) |
[Architecture](#architecture) | [Build](#build) | [Roadmap](#roadmap)

</div>

---

> [!IMPORTANT]
> Public single-device USB flashing is now integrated, but KairosBoot has not
> completed real-device HIL, fault-injection, soak, or throughput acceptance.
> Treat this development build as destructive and experimental; do not use it
> on devices containing irreplaceable data.

## Why KairosBoot?

AOSP Fastboot is a mature command-line tool. KairosBoot is being designed for
applications that need Fastboot as an embeddable, observable component instead
of a collection of child processes.

| | AOSP Fastboot | KairosBoot direction |
|---|---|---|
| Integration | Invoke an external executable | Use a C11 ABI, C++23 RAII API, .NET API, or CLI |
| Concurrency | Coordinate independent processes | Share one runtime and schedule devices by USB topology |
| Progress and cancellation | Parse process output and signals | Use typed operations, callbacks, tasks, and cancellation |
| Performance | Production baseline | Measure against raw USB ceilings and optimize only where headroom exists |
| Maturity | Production tool | Early development; the initial flash path is integrated but not production-qualified |

The right-hand column is the project direction, not a claim that those outcomes
have already been delivered or benchmarked.

## Current Status

The `0.1.0-dev` milestone establishes the public contracts and an experimental
single-file USB flash path while the broader command set and hardware gates are
still under development.

| Area | Status today |
|---|---|
| C11 SDK | Versioned opaque-handle API for version, context, errors, USB device lists, and operation-shaped flash entry points |
| C++23 SDK | Header-only, move-only RAII wrapper over the C ABI using `std::expected` |
| .NET SDK | Thin `net48;net10.0` binding with `SafeHandle`, UTF-8 marshalling, tasks, cancellation, and native error propagation |
| CLI | `--version`, `doctor --json`, `devices`, and single-file `flash`, with text or JSON output where applicable |
| USB discovery | Public enumeration of Fastboot USB interfaces through the locked libusb runtime |
| Transport core | Fastboot response state machine plus asynchronous USB and Boost.Asio-based TCP v1 / reliable UDP v1 internals tested independently |
| Data path | Transfer-ring, buffer-budget, adaptive-tuning, controller-scheduling, and sparse-image validation primitives |
| Flash operations | C, C++23, .NET, and CLI execute cancellable single-device USB download/flash with validated raw or Android sparse inputs |
| Fleet jobs | Versioned schema and scheduler primitives exist; manifest planning and execution are not public features yet |
| Performance and HIL | Acceptance goals are defined; 32-device, throughput, fairness, and soak claims have not been demonstrated yet |

### Target platforms

| Platform | Architecture | Intended public surfaces |
|---|---|---|
| Windows 10/11 | x64, ARM64 | Native SDK/CLI and .NET 10; .NET Framework 4.8 on x64 only |
| Ubuntu 22.04+ / Debian 12+ | x64, ARM64 | Native SDK/CLI and .NET 10 |
| macOS 14+ | x64, ARM64 | Native SDK/CLI and .NET 10 |

These are build and release targets. They do not imply that real-device USB
hardware-in-the-loop acceptance is complete on every target.

## Quick Start

### Inspect the current runtime

After building, the CLI exposes diagnostics plus the experimental flash path:

```sh
./build/kairosboot --version
./build/kairosboot --version --json
./build/kairosboot doctor --json
./build/kairosboot devices
./build/kairosboot devices --json
./build/kairosboot --serial SERIAL flash system images/system.img
```

`doctor --json` checks whether the native runtime and libusb dependency are
usable. `devices` returns the currently visible Fastboot USB interfaces.

### Use the C11 API

The entire C API is declared by one header and owns every returned handle:

```c
#include <kairosboot/kairosboot.h>

#include <stdio.h>

int main(void) {
  kb_context_t *context = NULL;
  kb_device_list_t *devices = NULL;
  kb_error_t *error = NULL;

  if (kb_context_create(NULL, &context, &error) != KB_OK ||
      kb_enumerate_devices(context, &devices, &error) != KB_OK) {
    fprintf(stderr, "%s\n", kb_error_message(error));
    kb_error_release(error);
    kb_context_release(context);
    return 1;
  }

  printf("%zu Fastboot device(s)\n", kb_device_list_count(devices));
  kb_device_list_release(devices);
  kb_context_release(context);
  return 0;
}
```

### Use the C++23 API

The C++ wrapper is header-only and delegates every operation to the stable C
ABI:

```cpp
#include <kairosboot/kairosboot.hpp>

#include <iostream>

int main() {
  auto context = kairosboot::Context::create();
  if (!context) {
    std::cerr << context.error().message() << '\n';
    return 1;
  }

  auto devices = context->devices();
  if (!devices) {
    std::cerr << devices.error().message() << '\n';
    return 1;
  }

  std::cout << devices->size() << " Fastboot device(s)\n";
}
```

Installed CMake packages export `KairosBoot::C` and `KairosBoot::Cxx`.

## API Surface

| Surface | Contract |
|---|---|
| C11 | Single public header, UTF-8 strings, fixed-width integers, opaque handles, explicit ownership, and blocking/asynchronous operation shapes |
| C++23 | Header-only RAII wrapper using `std::expected`, `std::filesystem`, and move-only resources; no separate C++ binary ABI |
| .NET Framework 4.8 | Windows x64 binding using `DllImport`, `SafeHandle`, `Task`, `CancellationToken`, and `IProgress<T>` |
| .NET 10 | `LibraryImport` binding for `win`, `linux`, and `osx` on x64 and ARM64 |
| CLI | C++23 consumer of the public wrapper with version, diagnostics, enumeration, and experimental single-file flash commands |

The managed package uses `kairosboot_native` as its internal P/Invoke library
name to avoid colliding with the managed `KairosBoot.dll` on case-insensitive
filesystems. The installed native SDK remains named `kairosboot`, and the C ABI
is unchanged. See the [.NET binding guide](bindings/dotnet/README.md) for package
and runtime details.

## Architecture

```text
C11 API ───────────────┐
C++23 RAII wrapper ────┼──> versioned C ABI ──> Fastboot / image core
.NET wrapper ──────────┤                              │
CLI (via C++ wrapper) ─┘                    USB / TCP / UDP internals
                                                       │
                                      transfer runtime / fleet primitives
```

One C ABI is the compatibility boundary for every language surface. The CLI is
also a public SDK consumer, which keeps its behavior from drifting into a
private implementation path.

Today, public calls reach versioning, context management, diagnostics, USB
enumeration, and cancellable single-file USB download/flash. The remaining
Fastboot command families, network session selection, and fleet orchestration
are still being connected to the public surfaces.

## Build

KairosBoot requires:

- CMake 3.22 or newer
- Python 3 and `make` for the locked dependency preparation step
- A C11 compiler and a compiler with C++23 support
- Ninja or another supported CMake generator
- Network access on the first configure so CMake `FetchContent` can download the locked Boost, miniz, and yaml-cpp archives

Release builds use exactly libusb 1.0.30 as a dynamically linked dependency.
Prepare it from the repository's locked, hash-verified source archives before
configuring. Select `windows`, `linux`, or `macos` and `x64` or `arm64` for the
target being built:

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

The root `CMakeLists.txt` fetches Boost 1.92.0, the latest stable release locked
for this source revision, from its official CMake archive and verifies the
committed SHA-256 digest. Boost.Asio is an internal transport dependency; Boost
types are not exposed by the installed C, C++, or C# APIs, and the TCP/UDP
implementations do not call native socket APIs directly.

The same dependency lock pins miniz 3.1.2 from its official, hash-verified
release archive. KairosBoot builds `miniz.c` as a hidden, position-independent
private static target with archive-writing, stdio, and zlib-compatible names
disabled while retaining CRC validation. It does not add a miniz or zlib shared
runtime dependency and is not part of the public API.

Fleet manifest parsing uses the locked yaml-cpp 0.9.0 release as another
position-independent private static dependency. Its tools, tests, contrib code,
shared library, and install rules remain disabled, so it adds no runtime library
or public SDK type.

For a multi-configuration generator such as Visual Studio, build and test the
explicit Release configuration:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Install the native SDK and CLI with:

```sh
cmake --install build --prefix "$PWD/install"
```

The install contains the matching libusb runtime, LGPL license and dependency
manifest, plus the Boost, miniz, and yaml-cpp license texts. Release automation
builds with the CMake `Release` configuration; when
`KAIROSBOOT_RELEASE_SYMBOLS=ON`, debug symbols are published separately without
disabling optimization.

## Testing

The repository uses no third-party C++ test framework. Its current automated
coverage includes:

- strict pure-C11 and C++23 public API consumers
- CLI JSON and output contracts
- scripted Fastboot response and transport tests without hardware
- asynchronous libusb runtime and transfer-lifecycle tests
- TCP v1 and reliable UDP v1 protocol tests
- sparse-image validation and malformed-input corpus tests
- .NET contract and NuGet native-layout tests
- install-tree and release-packaging smoke tests

Passing these tests does not establish production flash safety, USB ceiling
performance, or real-device behavior. Those require the later HIL and soak
gates in the roadmap.

## Roadmap

The planned work is deliberately separated from the status table above:

1. Extend the integrated USB flash operation to public TCP and UDP session
   selection, with deterministic cancellation, timeout, poison, drain, and
   reconnect behavior.
2. Complete the remaining Fastboot primitives including upload, erase, boot,
   continue, reboot, getvar, fetch/stage, format, and OEM passthrough.
3. Add update/flashall, ZIP and sparse pipelines, A/B slots, dynamic/super,
   fastbootd, AVB, boot/vendor_boot, logical partitions, snapshots, and GSI.
4. Expose deterministic fleet manifest validation, planning, execution,
   cancellation, and reports across at least 32 devices.
5. Tune against measured raw USB ceilings, compare with the pinned AOSP
   Fastboot baseline, then pass fault-injection, fairness, and 24-hour soak
   gates on the six target combinations.

## Project Layout

```text
include/kairosboot/  C11 API and C++23 wrapper
src/                 ABI implementation, protocol, image, fleet, and transport cores
cli/                 kairosboot command-line consumer
bindings/dotnet/     .NET Framework 4.8 and .NET 10 binding/package
schemas/             versioned job and report schemas
compat/              pinned AOSP compatibility inventory
tests/               native, managed, transport, packaging, and tooling tests
scripts/             dependency preparation and release packaging tools
```

## Contributing

External contributions are welcome through pull requests. All changes to
`main` require owner review; contributors do not receive direct push access.
Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a change, and report
security issues through [SECURITY.md](SECURITY.md).

## License

KairosBoot original source code is licensed under the [MIT License](LICENSE).
libusb is dynamically linked and retains its LGPL license. Boost retains the
Boost Software License 1.0. miniz and yaml-cpp are statically linked under MIT.
Other third-party components retain their own terms; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
