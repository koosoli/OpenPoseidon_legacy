<#
.SYNOPSIS
    PACE-001 -- run the frame-pacing series: one movecam traverse per fps cap, with the
    per-frame trace (POSEIDON_FRAME_TRACE) on, then analyse each run.

.DESCRIPTION
    Wraps scripts/farfield-movecam.ps1. Every arm runs against a SCRATCH user dir
    (POSEIDON_USER_DIR) whose graphics.cfg this script writes, so the owner's saved settings
    are never touched and every arm differs in exactly one thing: the cap (POSEIDON_FPS_CAP).
    VSync is pinned off (POSEIDON_VSYNC=0). Window mode + resolution come from the CLI
    (--window --width --height via -Width/-Height), MSAA/upscaler/shadows from -Msaa/
    -DlssMode/-Shadows.

    Output per arm: .tmp-movecam\out\<Label>-cap<N>\run-01.{log,png,json} plus
    <Label>-cap<N>.trace.{main,producer,worker}.csv and the analysis (stdout + .pace.png +
    .pace.json) next to them.

    Serialise with the game-folder lock:
        LOCK_OWNER="PACE-001" bash scripts/with-game-lock.sh powershell -NoProfile -File scripts/pace-series.ps1 ...

.NOTES
    Windows PowerShell 5.1. No &&, ||, ternary.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$Label,
    [int[]]$Caps = @(120, 30, 60, 0),
    [string]$Mission = 'tests/perf/missions/perf_field.eden',
    [double[]]$Start = @(6532, 6466),
    [double]$StartY = 2.0,
    [double]$Azimuth = 280,
    [double]$Elevation = -4,
    [double]$Speed = 5,
    [double]$SettleSeconds = 60,
    [double]$StaticSeconds = 30,
    [double]$MoveSeconds = 60,
    [double]$LoadSeconds = 60,
    [int]$Width = 2560,
    [int]$Height = 1440,
    [int]$Msaa = 4,
    [int]$DlssMode = 0,        # 0 off, 1 DLSS, 2 FSR1, -1 auto
    [int]$UpscalerQuality = 100,
    [int]$Shadows = 0,         # tier: 0 off .. 4
    [hashtable]$Env = @{},
    [int]$Repeats = 1,
    [string]$Out = '',
    # -Ride: instead of the scripted camera traverse, run a mission whose OWN init.sqs drives
    # the phases (the player sits in an AI-driven jeep: .tmp-pace/missions/ride.Intro) through
    # farfield-bench.ps1 directly. The FT lines still carry the phase tags.
    [switch]$Ride,
    [string]$RideMission = 'tests/perf/missions/pace_ride.Intro',
    [double]$RideSeconds = 130
)
$ErrorActionPreference = 'Continue'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
if ([string]::IsNullOrWhiteSpace($Out)) { $Out = Join-Path $repoRoot '.tmp-movecam\out' }
$userDir = Join-Path $repoRoot '.tmp-pace\userdir'
New-Item -ItemType Directory -Force -Path $userDir | Out-Null
$ownerCfg = Join-Path $env:APPDATA 'CWR\graphics.cfg'

function Write-ScratchGraphicsCfg {
    param([int]$Cap)
    $lines = @()
    if (Test-Path -LiteralPath $ownerCfg) { $lines = Get-Content -LiteralPath $ownerCfg }
    $out = @()
    $seen = @{}
    $overrides = @{
        'vsync' = '0'; 'fpsCap' = "$Cap"; 'msaaSamples' = "$Msaa"; 'dlssMode' = "$DlssMode";
        'upscalerQuality' = "$UpscalerQuality"; 'shadowQuality' = "$Shadows"; 'renderScale' = '1.000000'
    }
    foreach ($line in $lines) {
        if ($line -match '^\s*([A-Za-z]+)=') {
            $key = $Matches[1]
            if ($overrides.ContainsKey($key)) { $out += ('{0}={1};' -f $key, $overrides[$key]); $seen[$key] = $true; continue }
        }
        $out += $line
    }
    foreach ($key in $overrides.Keys) { if (-not $seen.ContainsKey($key)) { $out += ('{0}={1};' -f $key, $overrides[$key]) } }
    [IO.File]::WriteAllText((Join-Path $userDir 'graphics.cfg'), (($out -join "`n") + "`n"), [Text.Encoding]::ASCII)
    $disp = "monitor=0;`nwindowMode=2;`nresolutionWidth=$Width;`nresolutionHeight=$Height;`nrefreshRate=0;`ndisplayStyle=0;`nultrawideClamp=1;`n"
    [IO.File]::WriteAllText((Join-Path $userDir 'display.cfg'), $disp, [Text.Encoding]::ASCII)
}

$movecam = Join-Path $scriptDir 'farfield-movecam.ps1'
$analyse = Join-Path $scriptDir 'pace-analyse.py'
$summary = @()
$failed = @()
foreach ($cap in $Caps) {
    for ($rep = 1; $rep -le $Repeats; $rep++) {
        $armLabel = '{0}-cap{1}' -f $Label, $cap
        if ($Repeats -gt 1) { $armLabel = '{0}-r{1}' -f $armLabel, $rep }
        $armOut = Join-Path $Out $armLabel
        New-Item -ItemType Directory -Force -Path $armOut | Out-Null
        $tracePrefix = Join-Path $armOut ($armLabel + '.trace')
        Write-ScratchGraphicsCfg -Cap $cap
        $armEnv = @{}
        foreach ($k in $Env.Keys) { $armEnv[[string]$k] = [string]$Env[$k] }
        $armEnv['POSEIDON_USER_DIR'] = $userDir
        $armEnv['POSEIDON_VSYNC'] = '0'
        $armEnv['POSEIDON_FPS_CAP'] = "$cap"
        $armEnv['POSEIDON_FRAME_TRACE'] = $tracePrefix
        Write-Output ""
        Write-Output ('===== PACE arm {0}: cap {1} fps, {2}x{3}, MSAA {4}, dlssMode {5}, shadows {6} =====' -f $armLabel, $cap, $Width, $Height, $Msaa, $DlssMode, $Shadows)
        $t0 = Get-Date
        if ($Ride) {
            $bench = Join-Path $scriptDir 'farfield-bench.ps1'
            & $bench -Label $armLabel -Mission $RideMission -Repeats 1 -WarmupSeconds ($LoadSeconds + $RideSeconds) `
                -Width $Width -Height $Height -Out $Out -Env $armEnv 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 12
        } else {
        & $movecam -Label $armLabel -Mission $Mission -Start $Start -StartY $StartY -GroundY 0 `
            -Azimuth $Azimuth -Elevation $Elevation -Speed $Speed `
            -SettleSeconds $SettleSeconds -StaticSeconds $StaticSeconds -MoveSeconds $MoveSeconds -LoadSeconds $LoadSeconds `
            -Width $Width -Height $Height -Out $Out -Env $armEnv 2>&1 | ForEach-Object { "$_" } | Select-Object -Last 12
        }
        $dt = (Get-Date) - $t0
        $log = Join-Path $armOut 'run-01.log'
        Write-Output ('arm {0} took {1:N0} s; log {2}' -f $armLabel, $dt.TotalSeconds, $log)
        if (Test-Path -LiteralPath ($tracePrefix + '.main.csv')) {
            $pngOut = Join-Path $armOut ($armLabel + '.pace.png')
            $jsonOut = Join-Path $armOut ($armLabel + '.pace.json')
            & python $analyse $tracePrefix --log $log --png $pngOut --json $jsonOut --label $armLabel 2>&1 | ForEach-Object { "$_" }
            if ($LASTEXITCODE -ne 0) {
                Write-Warning ('arm {0}: analysis reported failure (exit {1}) -- NOT a valid measurement' -f $armLabel, $LASTEXITCODE)
                $failed += $armLabel
            } else {
                $summary += $armLabel
            }
        } else {
            Write-Warning ('no trace written for {0} (expected {1}.main.csv)' -f $armLabel, $tracePrefix)
            $failed += $armLabel
        }
        # The stamp the run measured, so the numbers can never be quoted without it.
        $stamp = Get-Content -LiteralPath 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\DEPLOYED-FROM.txt' -ErrorAction SilentlyContinue
        Write-Output ('measured binary: {0}' -f ($stamp -join ' '))
    }
}
Write-Output ""
Write-Output ('arms done: {0}' -f ($summary -join ', '))
if ($failed.Count -gt 0) {
    Write-Output ('arms FAILED: {0}' -f ($failed -join ', '))
    exit 1
}
exit 0
