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
Android Fastboot protocol. One in-process runtime serves C, C++, .NET, and CLI
consumers across Windows, Linux, and macOS. Each SDK call targets one explicit
device; applications may run independent per-device calls concurrently.

**KairosBoot is pre-release software.** The public SDK, Fastboot-compatible CLI,
USB/TCP/UDP transports, image pipelines, and device-isolated operation model are
implemented and covered by GitHub-hosted Release CI and scripted tests. Real
USB hardware qualification has been deliberately deferred from the current
software milestone, so no real-device throughput, 32-device, fault-injection,
or 24-hour soak result is claimed.

[Why](#why-kairosboot) | [Status](#current-status) |
[Quick Start](#quick-start) | [API](#api-surface) |
[Architecture](#architecture) | [Build](#build) | [Roadmap](#roadmap)

</div>

---

> [!IMPORTANT]
> Public USB flashing is integrated, but the current acceptance boundary is
> GitHub-hosted CI plus scripted protocol and transport tests. Real-device HIL,
> fault injection, throughput, 32-device, and soak qualification are explicitly
> deferred. Treat this development build as destructive and experimental; do
> not use it on devices containing irreplaceable data.

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

The `0.1.0-dev` software milestone is implemented and validated without making
hardware-dependent release claims. It does not require a self-hosted runner:
the protected HIL workflow and its fail-closed validators remain dormant until
real hardware qualification is resumed.

| Area | Status today |
|---|---|
| C11 SDK | Versioned opaque-handle API with one isolated `kb_device_t` per DUT and blocking/async Fastboot operations that each accept one device |
| C++23 SDK | Header-only, move-only RAII wrapper over the C ABI using `std::expected` |
| .NET SDK | Thin `net48;net10.0` binding with `SafeHandle`, UTF-8 marshalling, tasks, cancellation, and native error propagation |
| CLI | Fastboot-compatible command names and options for USB/TCP/UDP flashing, device management, boot images, dynamic partitions, staging, and update/flashall |
| USB discovery | Public enumeration of Fastboot USB interfaces through the locked libusb runtime |
| Transport core | Fastboot response state machine plus asynchronous USB and Boost.Asio-based TCP v1 / reliable UDP v1 internals tested independently |
| Data path | Transfer-ring, buffer-budget, adaptive-tuning, controller-scheduling, and sparse-image validation primitives |
| Fastboot operations | C, C++23, .NET, and CLI cover getvar/download/upload, flash/erase/boot/reboot, update/flashall, format, fetch/stage, slots, logical/super, fastbootd, AVB, boot/vendor_boot, snapshot, GSI, flashing, and OEM/raw passthrough |
| Device isolation | Every SDK operation targets one explicit opened device; no C, C++, or .NET API accepts a device array or collection. Applications compose independent calls, while repeated CLI `-s` calls remain CLI-owned orchestration |
| AOSP differential | The last merged baseline matched locked Platform-Tools 37.0.1 on Darwin, Linux, and Windows across 46 normalized host/TCP/UDP scenarios; this API-changing head requires fresh exact-source evidence |
| Performance and HIL | Real hardware qualification is deferred; the dormant fail-closed workflow is retained for a future lab phase, and no throughput, fairness, 32-device, fault-injection, or soak claim is made |

### Target platforms

| Platform | Architecture | Intended public surfaces |
|---|---|---|
| Windows 10/11 | x64, ARM64 | Native SDK/CLI and .NET 10; .NET Framework 4.8 on x64 only |
| Ubuntu 22.04+ / Debian 12+ | x64, ARM64 | Native SDK/CLI and .NET 10 |
| macOS 14+ | x64, ARM64 | Native SDK/CLI and .NET 10 |

These are build and release targets covered by the repository's hosted CI
matrix. They do not imply real-device USB hardware-in-the-loop acceptance on
any target.

## Quick Start

### Use the Fastboot-compatible CLI

The `kairosboot` executable follows the frozen Platform-Tools Fastboot command
syntax; it does not add KairosBoot-specific top-level commands:

```sh
./build/kairosboot --version
./build/kairosboot devices
./build/kairosboot devices -l
./build/kairosboot -s SERIAL getvar product
./build/kairosboot -s SERIAL flash system images/system.img
./build/kairosboot -s SERIAL_A -s SERIAL_B flash system images/system.img
./build/kairosboot -s SERIAL set_active b
./build/kairosboot -s SERIAL get_staged staged.bin
```

Repeated `-s` selectors run the same command concurrently on up to 32 devices.
Commands that write one host output file (`fetch` and `get_staged`) require a
single selector so devices cannot overwrite the same path.

Use `-s tcp:HOST[:PORT]` or `-s udp:HOST[:PORT]` for network Fastboot. Repeat
`-s` to run the same Fastboot-compatible device command concurrently; the C,
C++23, and .NET APIs accept explicit opened `Device` objects for the same model.

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
| C11 | Single public header, UTF-8 strings, fixed-width integers, opaque `Context`/`Device` handles, explicit ownership, and blocking/asynchronous operation shapes |
| C++23 | Header-only RAII wrapper using `std::expected`, `std::filesystem`, and a move-only `Device` per DUT; no separate C++ binary ABI |
| .NET Framework 4.8 | Windows x64 binding using `DllImport`, per-DUT `Device` objects, `SafeHandle`, `Task`, `CancellationToken`, and `IProgress<T>` |
| .NET 10 | `LibraryImport` binding for `win`, `linux`, and `osx` on x64 and ARM64 |
| CLI | C++23 consumer of the public wrapper using Fastboot-compatible commands and options, including repeated `-s` multi-device execution |

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
                                      transfer runtime / device scheduler
```

One C ABI is the compatibility boundary for every language surface. The CLI is
also a public SDK consumer, which keeps its behavior from drifting into a
private implementation path.

Public calls cover versioning, context management, diagnostics, USB discovery,
USB/TCP/UDP devices, Fastboot command families, and update/flashall. Every
operation accepts exactly one device; applications own any cross-device
concurrency, failure policy, and result aggregation. Real-hardware performance
and soak gates remain deferred qualification work outside the current software
milestone.

## Build

KairosBoot requires:

- CMake 3.22 or newer
- Python 3 and `make` for the locked dependency preparation step
- A C11 compiler and a compiler with C++23 support
- Ninja or another supported CMake generator
- Network access on the first configure so CMake `FetchContent` can download the locked Boost and miniz archives

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
metadata, plus the Boost and miniz license texts. Release automation
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
- byte-exact logical-partition metadata resize tests and normalized comparison
  against locked Platform-Tools 37.0.1
- .NET contract and NuGet native-layout tests
- install-tree and release-packaging smoke tests

Passing these tests does not establish production flash safety, USB ceiling
performance, or real-device behavior. Real hardware qualification is
deliberately deferred from the current milestone; no self-hosted runner or HIL
run is required at this stage. The protected workflow remains dormant and
continues to reject synthetic or incomplete evidence. When qualification is
resumed, lab operators must follow the
[hardware qualification contract](docs/HARDWARE_LAB.md).

## Roadmap

The current software milestone is closed by the hosted Release CI matrix,
scripted protocol/transport coverage, package smoke tests, and locked official
Fastboot differentials. Hardware qualification is a separate future phase:

1. Provision dedicated Windows, Linux, and macOS lab hosts only when real USB
   qualification is resumed; ordinary CI continues to use GitHub-hosted runners.
2. Run real USB fault injection, raw-ceiling, single-device, and 32-device
   throughput/fairness measurements without accepting synthetic substitutes.
3. Complete the 24-hour hardware soak before making production hardware or
   performance claims.
4. Choose the public version and signing mode separately before publishing a
   protected release; this development milestone does not imply a signed or
   production-qualified release.

## Project Layout

```text
include/kairosboot/  C11 API and C++23 wrapper
src/                 ABI implementation, protocol, image, scheduling, and transport cores
cli/                 kairosboot command-line consumer
bindings/dotnet/     .NET Framework 4.8 and .NET 10 binding/package
compat/              pinned AOSP compatibility inventory
tests/               native, managed, transport, packaging, and tooling tests
scripts/             dependency preparation and release packaging tools
docs/                bilingual API/CLI guide and hardware qualification contract
```

## Contributing

External contributions are welcome through pull requests. All changes to
`main` require owner review; contributors do not receive direct push access.
Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a change, and report
security issues through [SECURITY.md](SECURITY.md).

## License

KairosBoot original source code is licensed under the [MIT License](LICENSE).
libusb is dynamically linked and retains its LGPL license. Boost retains the
Boost Software License 1.0. miniz is statically linked under MIT.
Other third-party components retain their own terms; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
