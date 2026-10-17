#!/usr/bin/env pwsh
param(
    [string]$Preset,
    [string]$DllPath,
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"
# Self-contained (the scripts\_helpers.ps1 it used to load is not in this repo) and Windows PowerShell 5.1 safe.
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
Push-Location $repoRoot
$onWindows = ($env:OS -eq "Windows_NT")
if (-not $Preset) { $Preset = if ($onWindows) { "win-x64-clang-rwdi" } else { "linux-x64-clang-rwdi" } }
$suffix = ($Preset -split "-")[-1]
$platform = if ($onWindows) { "win" } else { "linux" }
if (-not $DllPath) {
    $leaf = if ($onWindows) { "PoseidonFormats.dll" } else { "PoseidonFormats.so" }
    $DllPath = Join-Path $repoRoot "distd-$platform-$suffix\$leaf"
}

# Prefer an already-built library from dist/.  Release packaging may run on the
# host while the Linux preset was configured inside SteamRT, so forcing a host
# rebuild against that cache can fail even though the needed artifact exists.
if (Test-Path $DllPath) {
    Write-Host ""
    Write-Host "=== Using existing PoseidonFormats library ($Preset) ==="
}
else {
    Write-Host ""
    Write-Host "=== Building PoseidonFormats DLL ($Preset) ==="
    $buildDir = Join-Path $repoRoot "build\$Preset"
    if (-not (Test-Path $buildDir)) {
        Write-Host "  Configuring preset $Preset ..."
        cmake --preset $Preset 2>&1 | Out-Null
    }
    cmake --build $buildDir --target PoseidonFormatsDLL 2>&1 | ForEach-Object {
        if ($_ -match "error|warning|Linking|Building.*PoseidonFormats") { Write-Host "  $_" }
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed (exit code $LASTEXITCODE)"
        exit 1
    }
}

if (-not (Test-Path $DllPath)) {
    Write-Error "DLL not found after build: $DllPath"
    exit 1
}

$buildId = Get-Random -Minimum 100000 -Maximum 999999
$buildTime = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
$dllItem = Get-Item $DllPath
$dllSize = [math]::Round($dllItem.Length / 1MB, 1)
$gitHash = git rev-parse --short HEAD 2>$null
if (-not $gitHash) { $gitHash = "unknown" }

Write-Host ""
Write-Host "=== P3D Blender Addon Package ==="
Write-Host "  Build ID:  $buildId"
Write-Host "  Time:      $buildTime"
Write-Host "  Git:       $gitHash"
Write-Host "  Preset:    $Preset"
Write-Host "  DLL:       $DllPath ($dllSize MB)"
Write-Host "  DLL date:  $($dllItem.LastWriteTime)"

$tempRoot = if ($env:TEMP) { $env:TEMP } else { [System.IO.Path]::GetTempPath() }
$staging = Join-Path $tempRoot "blender_addon_$(Get-Random)"
$addonDir = Join-Path $staging "io_import_p3d"
New-Item -ItemType Directory -Path $addonDir -Force | Out-Null

$pyFiles = @("__init__.py", "p3d_lib.py", "import_p3d.py", "export_p3d.py", "rig.py", "rig_ops.py", "rtm_io.py",
             "mc_joints.json", "helpers.py", "operators.py", "panels.py", "blender_manifest.toml")
foreach ($f in $pyFiles) {
    Copy-Item "apps\tools\BlenderAddon\io_import_p3d\$f" $addonDir
    Write-Host "  + $f"
}

Copy-Item $DllPath $addonDir
Write-Host "  + $($dllItem.Name)"

$initFile = Join-Path $addonDir "__init__.py"
$initContent = Get-Content $initFile -Raw
$initContent = $initContent.Replace(
    '"version": (1, 1, 0)',
    "`"version`": (1, 1, $buildId)")
$stamp = "`nBUILD_ID = `"$buildId`"`nBUILD_TIME = `"$buildTime`"`nBUILD_PRESET = `"$Preset`"`nBUILD_GIT = `"$gitHash`"`n"
$initContent = $initContent.Replace(
    "def register():",
    "${stamp}def register():")
$initContent = $initContent.Replace(
    "for cls in _classes:",
    "print(f'[P3D Addon] Build {BUILD_ID} | {BUILD_PRESET} | {BUILD_GIT} | {BUILD_TIME}')`n    for cls in _classes:")
[System.IO.File]::WriteAllText($initFile, $initContent)  # UTF-8, no BOM, on 5.1 too

$zipPath = Join-Path $OutDir "io_import_p3d.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
# .NET's zip writer: forward-slash entry names (5.1's Compress-Archive writes backslashes)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($staging, (Join-Path $repoRoot $zipPath))

Remove-Item -Recurse -Force $staging

$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host ""
Write-Host "  Output:    $zipPath ($zipSize MB)"
Write-Host "  Install:   Blender > Edit > Preferences > Add-ons > Install from Disk"
Write-Host "================================="
Write-Host ""

Pop-Location
