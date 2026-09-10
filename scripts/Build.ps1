<#
.SYNOPSIS
    Configure + build with a CMake preset, setting the toolchain env first so it
    works from any shell.
.PARAMETER Preset         CMake preset (default win-x64-clang-rwdi).
.PARAMETER Target         Build only the given target(s) (cmake --target).
.PARAMETER Clean          Delete build/<preset> first for a fresh configure.
.PARAMETER ConfigureOnly  Stop after configure.
.PARAMETER BuildArgs      Extra args passed to `cmake --build` (e.g. --parallel 8).
.EXAMPLE
    .\scripts\Build.ps1
.EXAMPLE
    .\scripts\Build.ps1 -Preset win-x64-clang-dbg -Target PoseidonGame
#>
[CmdletBinding()]
param(
    [string]$Preset = 'win-x64-clang-rwdi',
    [string[]]$Target,
    [switch]$Clean,
    [switch]$ConfigureOnly,
    # Position=0 makes the other params named-only, so cmake passthrough flags
    # (e.g. --parallel) flow here instead of binding to -Preset.
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] $BuildArgs
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent

# vcpkg toolchain (the base preset reads $env{VCPKG_ROOT}).
$vcpkgCmake = if ($env:VCPKG_ROOT) { Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake' } else { $null }
if (-not ($vcpkgCmake -and (Test-Path $vcpkgCmake)))
{
    $fallback = 'C:\dev\vcpkg'
    if (Test-Path (Join-Path $fallback 'scripts\buildsystems\vcpkg.cmake'))
    {
        $env:VCPKG_ROOT = $fallback
    }
    else
    {
        throw "VCPKG_ROOT is not set to a valid vcpkg checkout and none was found at $fallback. " +
              "Set VCPKG_ROOT to your vcpkg directory and retry."
    }
}

# LLVM tools (clang-format / clang-tidy) must be discoverable at configure time.
# Both drives: a Program Files install is not always on C:.
foreach ($llvmBin in @('C:\Program Files\LLVM\bin', 'D:\Program Files\LLVM\bin'))
{
    if ((Test-Path $llvmBin) -and (($env:Path -split ';') -notcontains $llvmBin))
    {
        $env:Path = "$env:Path;$llvmBin"
    }
}

# Windows SDK tools. CMake's compiler check links a test program through
# `cmake -E vs_link_exe`, which needs the manifest tool; without it configure dies at
# "The C++ compiler is not able to compile a simple test program", with the real reason
# ("CMAKE_MT-NOTFOUND ... no such file or directory") buried in the try-compile output.
#
# mt.exe lives in the SDK, not in LLVM, and is NOT on PATH in a normal shell — only in a
# Developer Command Prompt. So this script's promise to work "from any shell" held only for
# anyone who happened to already be in a developer shell, which is exactly the person who did
# not need the script. Found via the Kits registry rather than a hardcoded version, taking the
# newest SDK that actually ships an x64 mt.exe.
if (-not (Get-Command mt.exe -ErrorAction SilentlyContinue))
{
    $kitsRoot = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' `
                 -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
    if ($kitsRoot)
    {
        $sdkBin = Get-ChildItem (Join-Path $kitsRoot 'bin') -Directory -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match '^10\.' -and (Test-Path (Join-Path $_.FullName 'x64\mt.exe')) } |
                  Sort-Object { [version]$_.Name } -Descending |
                  Select-Object -First 1
        if ($sdkBin)
        {
            $env:Path = "$env:Path;$(Join-Path $sdkBin.FullName 'x64')"
            Write-Host "Windows SDK : $($sdkBin.Name)"
        }
    }
    if (-not (Get-Command mt.exe -ErrorAction SilentlyContinue))
    {
        Write-Warning ("mt.exe (Windows SDK) not found. Configure will fail with " +
                       "'C++ compiler is not able to compile a simple test program'. " +
                       "Install the Windows 10/11 SDK, or run this from a Developer PowerShell.")
    }
}

Write-Host "Repo       : $repoRoot"
Write-Host "Preset     : $Preset"
Write-Host "VCPKG_ROOT : $env:VCPKG_ROOT"
Write-Host ""

$buildDir = Join-Path $repoRoot "build\$Preset"
if ($Clean -and (Test-Path $buildDir))
{
    Write-Host "Cleaning $buildDir ..."
    Remove-Item -Recurse -Force $buildDir
}

Push-Location $repoRoot
try
{
    # Configure
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Configure failed (exit $LASTEXITCODE)." }

    if ($ConfigureOnly)
    {
        Write-Host "Configure complete (ConfigureOnly)." -ForegroundColor Green
        return
    }

    # Build
    $cmakeBuild = @('--build', $buildDir)
    if ($Target)    { $cmakeBuild += @('--target') + $Target }
    if ($BuildArgs) { $cmakeBuild += $BuildArgs }

    cmake @cmakeBuild
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }

    if ((-not $Target -or 'PoseidonGame' -in $Target) -and $Preset -match '^win-x64-clang-(rwdi|rel|dbg)$')
    {
        # Package owned content into dist, never into original game archives.
        # A targeted client build may not have built the existing asset CLI yet.
        $toolBuild = @('--build', $buildDir, '--target', 'PoseidonTools')
        if ($BuildArgs) { $toolBuild += $BuildArgs }
        cmake @toolBuild
        if ($LASTEXITCODE -ne 0) { throw 'Asset tool build failed.' }
        & "$PSScriptRoot/Build-DefaultContent.ps1" -Preset $Preset
    }

    Write-Host ""
    Write-Host "Build succeeded: $buildDir" -ForegroundColor Green
    Write-Host "Run with: .\scripts\Start.ps1"
}
finally
{
    Pop-Location
}
