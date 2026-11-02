# CLI contract

Protocol version is recorded in `build-manifest.json`.
Paths are passed as individual command-line arguments; Unicode paths are supported.

```text
image-tools --help
image-tools --version
image-tools validate --input PATH [--threads N]
image-tools convert --input PATH --output PATH [--threads N]
image-tools resize --input PATH --output PATH --width N --height N [--opaque] [--threads N]
image-tools stage --background PATH --background-output PATH --effects-output PATH
                  [--offset N] [--effect0 PATH] [--effect1 PATH] [--effect2 PATH] [--effect3 PATH]
                  [--threads N]
```

The global `--threads` option may appear anywhere, accepts 1..1024, and defaults
to one. It limits libvips threads within this process. The caller controls the
number of simultaneous worker processes. Width and height accept 1..65535.
Offset accepts a signed 32-bit integer and defaults to 160.

`validate` fully decodes the first image frame. `convert` applies orientation
and color management while preserving image dimensions and straight alpha.
Color conversion uses an embedded ICC profile or sRGB declaration when present;
otherwise PNG gAMA values are decoded before conversion to sRGB. Untagged
images use the decoder's color-space interpretation.
`resize` also resizes in linear light. `--opaque` composites
transparency over black. Without it, transparent output retains alpha.

`stage` produces a 1920×1080 background and a 512×512 two-by-two effects atlas.
Each effect tile is 256×256. Missing effect
inputs create transparent tiles. `--offset` shifts the background vertically
and fills exposed pixels with black.

Output files are uncompressed, top-left-origin, 32-bit BGRA TGA files.
Output parent directories must exist. Outputs use exclusive creation: existing
paths, input aliases, and colliding stage output paths are rejected. Callers
should use temporary output paths and promote successful results. A failed
command can leave an output file; callers own cleanup.

| Exit code | Meaning |
|---|---|
| 0 | Command completed |
| 1 | Processing or file-system failure |
| 2 | Invalid arguments |

Errors are written to standard error. Successful processing commands do not
write image data to standard output. `--version` writes `image-tools VERSION`
without initializing libvips. Callers can cancel a job by terminating its
worker process.
