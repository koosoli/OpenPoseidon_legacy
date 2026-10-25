<#
.SYNOPSIS
    Copy the modern menu resources from the free Arma: Cold War Assault Remastered Demo into
    the assets folder this script sits in.

.DESCRIPTION
    Ships inside a precompiled OpenPoseidon download as assets\Install-Menus.ps1 (run it through
    Install-Menus.cmd). The Demo's BIN, fonts, dtaExt and AddOns\cwr_logo.pbo are Bohemia
    Interactive data and are not part of the download; the engine reads them from assets\ when
    the game folder does not have them. Without them the game uses the classic menus.

.PARAMETER Force
    Overwrite files already in assets\ (default: skip).
#>
[CmdletBinding()]
param([switch]$Force)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\CwaCommon.ps1"

$demo = Get-CwaDemoDir
if (-not $demo) {
    throw ("The Arma: Cold War Assault Remastered Demo (Steam App $CwaDemoAppId) is not installed. " +
           "Install it from Steam (it is free), then run this again.")
}
Write-Host "Demo:   $demo"
Write-Host "Assets: $PSScriptRoot`n"

$copied = 0; $skipped = 0
foreach ($rel in @('fonts', 'dtaExt', 'AddOns\cwr_logo.pbo', 'BIN')) {
    $src = Join-Path $demo $rel
    if (-not (Test-Path -LiteralPath $src)) { Write-Warning "The Demo is missing '$rel' - skipping."; continue }
    $files = if ((Get-Item -LiteralPath $src).PSIsContainer) { Get-ChildItem -LiteralPath $src -Recurse -File } else { Get-Item -LiteralPath $src }
    foreach ($file in $files) {
        $dest = Join-Path $PSScriptRoot $file.FullName.Substring($demo.Length).TrimStart('\')
        if ((Test-Path -LiteralPath $dest) -and -not $Force) { $skipped++; continue }
        New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
        $copied++
    }
}
Write-Host "Copied $copied files, skipped $skipped already present."
