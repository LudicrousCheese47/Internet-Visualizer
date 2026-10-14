# build-msi.ps1 - produces InternetVisualizer-<version>.msi from a Release build.
#
# Requirements:
#   * A release build of Internet_Visualizer.exe (default: cmake-build-release)
#   * WiX Toolset v7 CLI (`dotnet tool install --global wix`). The build below
#     passes `-acceptEula wix7` (OSMF EULA, free below US$10k/yr revenue) so it
#     works on fresh machines without a manual EULA step.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File packaging/build-msi.ps1
#   powershell ... -packaging/build-msi.ps1 -Version 1.0.1 -BuildDir cmake-build-release
param(
    [string]$BuildDir = "cmake-build-release",
    [string]$Version  = "1.0.0",
    [string]$OutDir   = "packaging/out",
    [string]$InstallScope = "perMachine"   # perMachine (Program Files) or perUser
)

$ErrorActionPreference = "Stop"
$Root   = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$exe    = Join-Path $Root "$BuildDir/Internet_Visualizer.exe"
if (!(Test-Path $exe)) {
    throw "Release exe not found: $exe - build it first (cmake --build $BuildDir --config Release)"
}

# ---- stage: exe + assets next to each other exactly like a build dir -------
$stage   = Join-Path $Root "$OutDir/stage"
$stageAssets = Join-Path $stage "assets"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stageAssets | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $Root "assets\*") $stageAssets

# MinGW runtime is statically linked, so the exe depends only on system DLLs;
# if that ever changes, copy the missing mingw DLLs into $stage here.

# ---- locate wix ----
$wix = Join-Path $env:USERPROFILE ".dotnet\tools\wix.exe"
if (!(Test-Path $wix)) {
    throw "WiX not installed. Run: dotnet tool install --global wix --add-source https://api.nuget.org/v3/index.json"
}

# ---- build the MSI ----
$wxs    = Join-Path $PSScriptRoot "InternetVisualizer.wxs"
$outDir = Join-Path $Root $OutDir
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$msi    = Join-Path $outDir "InternetVisualizer-$Version.msi"

& $wix build $wxs `
    -acceptEula wix7 `
    -d AppVersion=$Version `
    -d InstallScope=$InstallScope `
    -d "ExePath=$stage/Internet_Visualizer.exe" `
    -d "StageDir=$stage" `
    -d "IconPath=$stage/assets/icon.ico" `
    -d ProductCode="{B3A7F2C8-1D4E-4F6B-9A20-5C6E7D8F9A0B}" `
    -d UpgradeCode="{C4B8E1D9-2E5F-4A7C-8B31-6D7E8F90A1B2}" `
    -d MainExeGuid="{D5C9F2EA-3A60-4B8D-9C42-7E8F90A1B2C3}" `
    -d CoreAssetsGuid="{E6DA03FB-4B71-4C9E-AD53-8F90A1B2C3D4}" `
    -d FontLicenseGuid="{F7EB14FC-5C82-4DAF-BE64-90A1B2C3D4E5}" `
    -arch x64 `
    -o $msi

if ($LASTEXITCODE -ne 0) { throw "WiX build failed (exit $LASTEXITCODE)" }
Write-Output "MSI written:  $msi"
Get-Item $msi | Select-Object FullName, Length, LastWriteTime