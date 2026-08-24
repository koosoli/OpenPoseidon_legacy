<#
.SYNOPSIS
    Launch a built Poseidon binary against the retail (or Demo) game data.
.DESCRIPTION
    Run Install.ps1 once first: the retail game lacks files the remaster needs and
    the engine aborts without them.
.PARAMETER App         Game (default), GameDemo, or Server.
.PARAMETER Preset      CMake preset whose dist output to run (default win-x64-clang-rwdi).
.PARAMETER Demo        Use Demo data instead of retail.
.PARAMETER Fullscreen  Run fullscreen (default windowed).
.PARAMETER LogFile     Also write the engine log here (--log-file).
.EXAMPLE
    .\scripts\Start.ps1
.EXAMPLE
    .\scripts\Start.ps1 -Fullscreen -nosplash
#>
[CmdletBinding()]
param(
    [ValidateSet('Game', 'GameDemo', 'Server')] [string]$App = 'Game',
    [string]$Preset = 'win-x64-clang-rwdi',
    [switch]$Demo,
    [switch]$Fullscreen,
    [string]$LogFile,
    # Position=0 makes the other params named-only, so engine flags (--check,
    # -nosplash, ...) pass through instead of binding to -App.
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] $GameArgs
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\CwaCommon.ps1"

if ($Demo)
{
    $data = Get-CwaDemoDir
    if (-not $data) { throw "Demo install (Steam App $CwaDemoAppId) not found in registry." }
}
else
{
    $data = Get-CwaRetailDir
    if (-not $data) { throw "Retail install (Steam App $CwaRetailAppId) not found in registry." }
}

# dist suffix is the preset's last segment (rwdi/dbg/rel).
$repoRoot = Split-Path $PSScriptRoot -Parent
$suffix   = ($Preset -split '-')[-1]
# The shipped client is OpenPoseidon.exe; the other two apps keep their Poseidon* names.
$exeName  = @{ Game = 'OpenPoseidon.exe'; GameDemo = 'PoseidonGameDemo.exe'; Server = 'PoseidonServer.exe' }[$App]
$exe      = Join-Path $repoRoot "dist\x64-win-$suffix\$exeName"
if (-not (Test-Path $exe)) { throw "Not built: $exe`n  Build first: .\scripts\Build.ps1 -Preset $Preset" }

$argv = @('-C', $data)
if (-not $Fullscreen) { $argv += '--window' }
if ($LogFile)         { $argv += @('--log-file', $LogFile) }
if ($GameArgs)        { $argv += $GameArgs }

Write-Host "Launching: $exe"
Write-Host "Data:      $data`n"

& $exe @argv
exit $LASTEXITCODE
