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

The first implementation milestone establishes the public API, protocol test
harness, and cross-platform build before enabling destructive device commands.

## Contributing

External contributions are accepted through pull requests. All changes to
`main` require review by the repository owner. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

KairosBoot original source code is licensed under the [MIT License](LICENSE).
Third-party components retain their respective licenses.
