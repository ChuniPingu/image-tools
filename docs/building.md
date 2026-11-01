# Building and releases

The supported target is Windows x64, using an MSVC-compatible C++ compiler.
CMake sets the static MSVC runtime and links the libvips C API through its import
library. The replaceable libvips DLL remains alongside the executable.

## Local build

Run `scripts/build.ps1` from PowerShell 7. It locates CMake on PATH or in Visual
Studio, builds the worker and native tests, runs CTest, installs and smoke-tests
the bundle, then creates the ZIP and SHA256SUMS.

CMake downloads the SDK into the build directory and verifies the SHA-256 from
[the manifest](../build-manifest.json). An existing archive with a different hash
is rejected. The SDK includes native codecs; separate JPEG or WebP downloads
are unnecessary.

For an already extracted copy of the same official SDK:

```powershell
./scripts/build.ps1 -VipsRoot C:\sdk\vips-dev-8.18
```

Alternatively set the `VIPS_ROOT` environment variable or CMake cache option.
This local override checks required files and the libvips version but does not
verify the original archive hash; use the unmodified manifest-matching web-static
SDK. Build metadata records use of the override. Release CI uses the verified download.

Equivalent commands:

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix build/install
./scripts/smoke.ps1 -BundleDirectory build/install
cmake --build build --config Release --target package
```

## Release contents

The ZIP contains the executable, libvips DLL, MIT notice, upstream notices and
GNU license texts, upstream SDK license, dependency versions, build manifest,
and build metadata. It excludes SDK headers, import libraries, and development
tools. `build-metadata.json` records the source commit, compiler, SDK URL and
expected hash without embedding local filesystem paths.

## Publishing

Update the manifest and commit the source. Push a version tag matching its
application version. The GitHub workflow builds and tests on Windows, smoke-tests
the extracted ZIP, and creates the tagged release with the ZIP and SHA256SUMS.
Consumers should pin a release URL and the ZIP's SHA-256.

The SDK build configuration and matching native source are linked from the
manifest. Retain bundled notices when redistributing the release. Additional
formats such as JPEG XL or HEVC require a separate dependency configuration
and corresponding validation; the default profile follows the official web build.
