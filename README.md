# image-tools

A Windows command-line worker for image validation, resizing, and stage textures.
Each command runs its complete libvips pipeline in one process and writes a TGA
handoff for texture encoders. The application uses the libvips C API.

Download and extract the Windows x64 ZIP from [Releases](https://github.com/ChuniPingu/image-tools/releases).
Keep the executable, runtime DLL, and notices together.

## Quick start

```powershell
.\image-tools.exe --version
.\image-tools.exe validate --input jacket.png
.\image-tools.exe resize --input jacket.png --output jacket.tga --width 512 --height 512 --opaque
```

Output paths must not already exist. See the [CLI contract](docs/cli.md) for
stage textures, concurrency, output format, and exit codes.

## Build

Install Visual Studio with the **Desktop development with C++** workload, CMake,
and PowerShell 7. The build downloads and verifies the official Windows libvips SDK.

```powershell
./scripts/build.ps1
```

The ZIP and its checksum are written to `build/artifacts/`.
See [building and releases](docs/building.md) for SDK overrides and packaging.
Application and dependency versions are pinned in [build-manifest.json](build-manifest.json).

## License

The worker source is [MIT licensed](LICENSE). Bundled native libraries retain
their own licenses; see [third-party notices](THIRD-PARTY-NOTICES.md).
