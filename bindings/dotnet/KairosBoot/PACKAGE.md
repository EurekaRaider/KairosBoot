# KairosBoot for .NET

This package contains the first-class managed API for KairosBoot.

- `net48` supports Windows x64 and uses runtime `DllImport` interop.
- `net10.0` supports Windows, Linux, and macOS on x64 and ARM64 and uses
  source-generated `LibraryImport` interop.

The matching native `kairosboot` shared library must be installed beside the
application or in the operating system's native library search path. This
foundation package does not yet bundle native runtime assets.

The current native foundation intentionally has no device transport. `Devices`
and `FlashFileAsync` therefore throw `KairosBootException` with
`Status == KairosBootStatus.NotSupported`; they never report a successful flash.
