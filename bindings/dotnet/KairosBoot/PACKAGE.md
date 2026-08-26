# KairosBoot for .NET

This package contains the first-class managed API for KairosBoot.

- `net48` supports Windows x64 and uses runtime `DllImport` interop.
- `net10.0` supports Windows, Linux, and macOS on x64 and ARM64 and uses
  source-generated `LibraryImport` interop.

Development packages are managed-only by default. A developer using one must
stage the native SDK library and its dependencies in the operating system's
native library search path, renaming or aliasing the SDK library to the
platform's `kairosboot_native` filename. Release packages include validated
KairosBoot, libusb, and platform runtime assets for all six RIDs.

Within the .NET/NuGet distribution, the native library is named
`kairosboot_native` so it cannot collide with the managed `KairosBoot.dll` on
case-insensitive filesystems. The installed native C/C++ SDK remains named
`kairosboot`; the NuGet-only filename does not change the stable C ABI.

The net48 package supports only x64. Its `build` and `buildTransitive` targets
copy KairosBoot, libusb, and the app-local Microsoft Visual C++ runtime beside
the application; AnyCPU and x86 builds fail before compilation. Libusb license
and dependency manifests are stored under package-root `licenses/`, not as
runtime assets.

The current native foundation intentionally has no device transport. `Devices`
and `FlashFileAsync` therefore throw `KairosBootException` with
`Status == KairosBootStatus.NotSupported`; they never report a successful flash.
