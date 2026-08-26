# Third-party notices

KairosBoot original source code is licensed under MIT. The following dependency
is planned or used under its own license.

## libusb 1.0.30

- Project: https://github.com/libusb/libusb
- Version: 1.0.30
- License: GNU Lesser General Public License 2.1 or later
- Source archive: https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2

KairosBoot links to libusb dynamically. Binary distributions that include
libusb must include its license, corresponding source information, and permit
replacement of the dynamically linked library.

Android Platform-Tools binaries are test oracles only and are not redistributed
as part of KairosBoot.

## Boost 1.92.0

- Project: https://github.com/boostorg/boost
- Version: 1.92.0
- Component: Boost.Asio and its Boost header dependencies
- License: Boost Software License 1.0
- CMake source archive: https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz

KairosBoot fetches the immutable Boost CMake release archive at configure time
and verifies its SHA-256 digest. Boost.Asio is used as an internal networking
implementation dependency; it is not exposed by the installed C or C++ API.
Binary and SDK distributions include `LICENSE_1_0.txt`.

## Microsoft Visual C++ Runtime

Windows binary distributions include the architecture-matching, app-local
Microsoft Visual C++ runtime files selected by CMake from the Visual Studio
redistributable directory. Those files remain subject to the Microsoft Visual
Studio licensing terms and are included so the SDK and CLI do not depend on a
machine-wide Visual C++ Redistributable installation.
