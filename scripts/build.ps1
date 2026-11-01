[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [string]$BuildDirectory = 'build',
    [string]$VipsRoot = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildPath = [IO.Path]::GetFullPath($BuildDirectory, $repoRoot)
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmake = $cmakeCommand.Source }
else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install CMake or Visual Studio C++ CMake tools.' }
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'Install the Visual Studio Desktop development with C++ workload.' }
    $cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $cmake)) { throw 'Install the Visual Studio C++ CMake tools component.' }
}
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
function Invoke-BuildTool([string]$Tool, [string[]]$ToolArguments) {
    & $Tool @ToolArguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with exit code $LASTEXITCODE." }
}
$configure = @('-S', $repoRoot, '-B', $buildPath, '-A', 'x64', '-DBUILD_TESTING=ON')
if ($VipsRoot) { $configure += "-DVIPS_ROOT=$VipsRoot" }
Invoke-BuildTool $cmake $configure
Invoke-BuildTool $cmake @('--build', $buildPath, '--config', $Configuration, '--parallel')
Invoke-BuildTool $ctest @('--test-dir', $buildPath, '-C', $Configuration, '--output-on-failure')
$installPath = Join-Path $buildPath 'install'
Invoke-BuildTool $cmake @('--install', $buildPath, '--config', $Configuration, '--prefix', $installPath)
& (Join-Path $PSScriptRoot 'smoke.ps1') -BundleDirectory $installPath
Invoke-BuildTool $cmake @('--build', $buildPath, '--config', $Configuration, '--target', 'package')
$archive = Join-Path $buildPath 'artifacts\image-tools-win-x64.zip'
$checksum = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumText = "$checksum  image-tools-win-x64.zip" + [Environment]::NewLine
[IO.File]::WriteAllText((Join-Path (Split-Path -Parent $archive) 'SHA256SUMS'), $checksumText, [Text.UTF8Encoding]::new($false))
Write-Output $archive
