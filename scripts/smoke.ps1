[CmdletBinding()]
param([Parameter(Mandatory)][string]$BundleDirectory)
$ErrorActionPreference = 'Stop'
$bundle = (Resolve-Path -LiteralPath $BundleDirectory).Path
$exe = Join-Path $bundle 'image-tools.exe'
$manifest = Get-Content -LiteralPath (Join-Path $bundle 'build-manifest.json') -Raw | ConvertFrom-Json
$version = & $exe --version
if ($LASTEXITCODE -ne 0 -or $version -ne "image-tools $($manifest.version)") { throw 'Installed CLI version check failed.' }
$metadata = Get-Content -LiteralPath (Join-Path $bundle 'build-metadata.json') -Raw | ConvertFrom-Json
if ($metadata.version -ne $manifest.version) { throw 'Bundle metadata version mismatch.' }
$smokeRoot = Join-Path ([IO.Path]::GetTempPath()) ('image-tools-smoke-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($smokeRoot) | Out-Null
try {
    $inputFile = Join-Path $smokeRoot '颜色 source.png'
    $outputFile = Join-Path $smokeRoot '结果.tga'
    $png = 'iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAAXNSR0IArs4c6QAAAARnQU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAAXSURBVBhXY/j///9/BgYQZmD4zwDkAABdtAn36FUC7wAAAABJRU5ErkJggg=='
    [IO.File]::WriteAllBytes($inputFile, [Convert]::FromBase64String($png))
    & $exe validate --input $inputFile
    if ($LASTEXITCODE -ne 0) { throw 'Installed CLI validation failed.' }
    $convertedFile = Join-Path $smokeRoot '转换.tga'
    & $exe convert --input $inputFile --output $convertedFile
    if ($LASTEXITCODE -ne 0) { throw 'Installed CLI conversion failed.' }
    $converted = [IO.File]::ReadAllBytes($convertedFile)
    if ($converted.Length -ne (18 + 2 * 2 * 4) -or $converted[12] -ne 2 -or $converted[14] -ne 2) {
        throw 'Installed CLI conversion changed the image dimensions.'
    }
    & $exe resize --input $inputFile --output $outputFile --width 4 --height 3 --opaque
    if ($LASTEXITCODE -ne 0) { throw 'Installed CLI resize failed.' }
    $bytes = [IO.File]::ReadAllBytes($outputFile)
    if ($bytes.Length -ne (18 + 4 * 3 * 4) -or $bytes[2] -ne 2 -or $bytes[16] -ne 32 -or $bytes[17] -ne 40) {
        throw 'Installed CLI produced an unexpected TGA.'
    }
}
finally {
    $resolvedSmoke = [IO.Path]::GetFullPath($smokeRoot)
    $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedSmoke.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedSmoke).StartsWith('image-tools-smoke-')) {
        Remove-Item -LiteralPath $resolvedSmoke -Recurse -Force
    }
}
