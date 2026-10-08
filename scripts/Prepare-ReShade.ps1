<#
.SYNOPSIS
    Explicitly download/prepare official ReShade 6.8.0 locally and build our add-on.
    Never runs the installer or installs a global Vulkan layer. Needs Python 3,
    curl, CMake, Ninja and a Windows x64 C++ toolchain (same as the engine build).
#>
[CmdletBinding()]
param(
    [string]$RuntimeDirectory = (Join-Path $env:LOCALAPPDATA 'OpenPoseidon/reshade/6.8.0'),
    [string]$InstallerPath = '',
    [string]$SourceArchivePath = ''
)
$ErrorActionPreference = 'Stop'
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) {
    throw 'Close the game before updating the optional ReShade runtime.'
}
$repo = Split-Path $PSScriptRoot -Parent
$argsPy = @((Join-Path $PSScriptRoot 'Prepare-ReShade.py'), $RuntimeDirectory)
if ($InstallerPath) { $argsPy += @('--installer', $InstallerPath) }
if ($SourceArchivePath) { $argsPy += @('--source-archive', $SourceArchivePath) }
python @argsPy
if ($LASTEXITCODE -ne 0) { throw 'Official ReShade preparation failed.' }
& (Join-Path $PSScriptRoot 'Build-ReShadeBridge.ps1') -RuntimeDirectory $RuntimeDirectory
Write-Host "Prepared opt-in ReShade preview: $RuntimeDirectory"
