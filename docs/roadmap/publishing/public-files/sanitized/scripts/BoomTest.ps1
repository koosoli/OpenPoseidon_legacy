<#
.SYNOPSIS
  Play-test the ground explosions, craters, wall breaches and snow bullet holes
  (feat/ground-explosions) fullscreen at desktop resolution. The game keeps running until you quit.

.DESCRIPTION
  Runs THIS worktree's build (dist\x64-win-rwdi\OpenPoseidon.exe and the wgpu_renderer.dll next
  to it) against the game data, with the game folder as the working directory. Nothing is copied
  into the game folder: the installed build and DEPLOYED-FROM.txt stay as they are.

  Mission (tests\manual\boom_test.abel, Malden): sunny morning, you carry a LAW and satchels.
  Action menu:
    BOOM: 120 mm shell 40 m ahead         crater, dirt column, fireball, debris
    BOOM: barrage of 6 shells ahead
    BOOM: rocket straight ahead            aim at a wall: hole, flying chunks, dust;
                                           then shoot or look through the hole
    BOOM: rearm LAW + satchels
    BOOM: go to the garages                the verified breach target

.PARAMETER Snow       Lay snow before the mission starts (no falling flakes, clear sky), for
                      bullet holes in the snow and the snow look. Depth in metres via -SnowDepth.
.PARAMETER SnowDepth  Default 0.25 m.
.PARAMETER Window     Windowed instead of fullscreen.
.PARAMETER GameDir    Default D:\SteamLibrary\steamapps\common\ARMA Cold War Assault.

.EXAMPLE
  .\scripts\BoomTest.ps1
  .\scripts\BoomTest.ps1 -Snow
#>
param(
    [switch]$Snow,
    [double]$SnowDepth = 0.25,
    [switch]$Window,
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo 'dist\x64-win-rwdi\OpenPoseidon.exe'
$dll = Join-Path $repo 'dist\x64-win-rwdi\wgpu_renderer.dll'
$mission = Join-Path $repo 'tests\manual\boom_test.abel'

foreach ($p in @($exe, $dll, $mission, $GameDir)) {
    if (-not (Test-Path $p)) { throw "Missing: $p (build first with scripts\Build.ps1)" }
}
# exe and DLL must come from the same build (the development guidelines rule 0)
$skew = [math]::Abs(((Get-Item $exe).LastWriteTime - (Get-Item $dll).LastWriteTime).TotalMinutes)
if ($skew -gt 30) { Write-Warning "exe and wgpu_renderer.dll are $([int]$skew) min apart -- rebuild before trusting what you see." }
if (Get-Process -Name OpenPoseidon -ErrorAction SilentlyContinue) {
    throw 'OpenPoseidon is already running (another session or game). One instance at a time.'
}

$log = Join-Path $env:TEMP 'boom_test.log'
$argv = @('--render=wgpu', '--dev', '--test-mission', "`"$mission`"", '--log-file', "`"$log`"")
if ($Window) { $argv += '--window' }

if ($Snow) {
    $env:POSEIDON_SNOW_TEST_DEPTH = [string]$SnowDepth
} else {
    Remove-Item Env:POSEIDON_SNOW_TEST_DEPTH -ErrorAction SilentlyContinue
}

Write-Host "Build:   $exe ($((Get-Item $exe).LastWriteTime))"
Write-Host "Mission: $mission$(if ($Snow) { "  (snow $SnowDepth m)" })"
Write-Host "Log:     $log"
Start-Process -FilePath $exe -ArgumentList $argv -WorkingDirectory $GameDir | Out-Null
