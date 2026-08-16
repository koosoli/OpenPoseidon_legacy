# Two-arm A/B for the cascade gate fix (commit bc59281), against the acceptance criteria in
# docs/roadmap/decisions/SHD-001-shadow-regression-acceptance-criteria-20260815.md
#
# Arm "fixed"  - default behaviour, the gate also opens on a non-empty retained set.
# Arm "legacy" - WGR_SHADOW_LEGACY_GATE=1, the old !_shadowCasters.empty() gate.
#
# The legacy arm is NOT optional. If it also renders correct shadows then the gate was never the
# cause at this pose and the fixed arm looking right proves nothing (criterion B1).
#
# World is Arma 2 Takistan because the owner's reference screenshot is Arma 2 mud-brick
# architecture. Pose is lifted from a prior working run's own log
# (.tmp-perf-modernisation/final-cross-a2-road.log) rather than from notes, per the
# substituted-world recipe: handover notes drift, logs do not.
#
# Sun is put at hour 8, NOT the 12 that prior run used. At noon the sun is near zenith and
# building shadows collapse to a footprint; the owner's reference has a long low shaft. A shadow
# test at noon can pass while the shadows are wrong.

[CmdletBinding()]
param(
    [string]$Out      = "$PSScriptRoot\out",
    [int]   $Hour     = 8,
    # X Z Y az el -- the engine takes X, then Z, then the ALTITUDE. Getting this wrong put the
    # first run's camera at 6099 m and photographed the tops of the clouds. The prior working
    # run's log prints two different lines: a GROUNDED PLAYER position (x, altitude, z) and a
    # CAMERA line (x, altitude, z); neither is in CLI order, so both must be transposed here.
    # Camera was x=6770.69 z=6099.78 alt=312.86.
    [string]$Freefly  = "6770.69 6099.78 312.86 230.7 -6.8",
    [int]   $ShotFrame = 900,
    [int]   $Width    = 1600,
    [int]   $Height   = 900,
    # World is swappable because the discriminating condition is cpuCasters == 0, and which world
    # produces that is an empirical question, not something to assume. The Takistan EXTERIOR pose
    # has 7 CPU casters in view, so the legacy gate stays open and both arms render identically —
    # a true null, but not a test of the fix.
    [string]$World  = 'C:\Users\mail\AppData\Local\Temp\poseidon-a2oa-takistan-inspect\takistan.wrp',
    [string]$Addons = 'C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.tmp-perf-modernisation\a2-addons'
)

$ErrorActionPreference = 'Stop'

$game  = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
$exe   = Join-Path $game 'ColdWarAssault.exe'
$world = $World
$addons = $Addons
# --test-mission needs an ABSOLUTE path; a relative one resolves against the repo and fails
# with "does not resolve to a mission.sqm".
$mission = Join-Path $game 'dev-missions\devtest-dawn.abel'

foreach ($p in @($exe, $world, $mission)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}
# Reforger Everon carries its own deployed data and takes NO --addon-root; pointing it at another
# generation's PBOs is worse than omitting it, because ProductList refuses unlisted products
# silently and the symptom is a world that loads and then finds no models.
if ($addons -and -not (Test-Path $addons)) { throw "missing: $addons" }
New-Item -ItemType Directory -Force $Out | Out-Null

# The four worlds live under %TEMP% on a machine whose C: has been at 100%. Storage Sense deletes
# temp directories. Report the size so a run against a truncated world is obvious rather than
# looking like a renderer fault.
$wsize = [math]::Round((Get-Item $world).Length / 1MB, 1)
Write-Host "world: $world ($wsize MB)"

function Invoke-Arm {
    param([string]$Name, [hashtable]$Env)

    $png = Join-Path $Out "$Name.png"
    $log = Join-Path $Out "$Name.log"
    Remove-Item $png, $log -ErrorAction SilentlyContinue

    # Set env for the child. The game is launched detached via cmd /c start, which inherits the
    # current process environment, so setting it here is what reaches the game.
    foreach ($k in $Env.Keys) { Set-Item -Path "Env:$k" -Value $Env[$k] }
    # WGR_SHADOW_GATE_TRACE goes on in BOTH arms: the arm that reproduces the bug is the one whose
    # cpuCasters count settles criterion B2, so silencing it there would waste the run.
    $env:WGR_SHADOW_GATE_TRACE = '1'

    $ff = $Freefly -split '\s+'
    # NOT $args: that is a read-only automatic variable, and assigning it throws under
    # $ErrorActionPreference = 'Stop', which killed the first run before it launched anything.
    $cmdArgs = @(
        '--render wgpu', '--window', '--dev',
        "--width $Width", "--height $Height",
        "--test-mission `"$mission`"",
        "--test-world `"$world`"",
        $(if ($addons) { "--addon-root `"$addons`"" } else { '' }),
        "--test-world-freefly $($ff[0]) $($ff[1]) $($ff[2]) $($ff[3]) $($ff[4])",
        "--test-world-hour $Hour",
        # --auto-screenshot accepts exactly one capture ("At Most 1 required").
        "--auto-screenshot `"${ShotFrame}:$png`"",
        "--log-file `"$log`"", '--log-level info'
    ) -join ' '

    # PowerShell 5.1 Start-Process -ArgumentList mangles quoting on paths with spaces and the game
    # dies BEFORE opening its log — symptom is no PNG and no .log at all. Launch through cmd start
    # with explicit quotes. Never redirect stdout/stderr: that kills the renderer (os error 232).
    Write-Host "`n=== arm: $Name ===" -ForegroundColor Cyan
    cmd /c "start `"shadow-$Name`" /D `"$game`" `"$exe`" $cmdArgs"

    # Start.ps1 and this launch both return while the game keeps running. Poll for the artefact.
    $deadline = (Get-Date).AddMinutes(8)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $png) { Start-Sleep -Seconds 3; break }
        Start-Sleep -Seconds 5
    }
    # Do not leave a process resident: a later benchmark must never share the machine with a game.
    Get-Process ColdWarAssault -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2

    foreach ($k in $Env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    Remove-Item Env:WGR_SHADOW_GATE_TRACE -ErrorAction SilentlyContinue

    if (-not (Test-Path $png)) {
        Write-Host "  NO PNG - check $log" -ForegroundColor Red
        return
    }
    Write-Host "  png: $png" -ForegroundColor Green
    # The gate trace is the whole point of the run; surface it without needing to open the log.
    $trace = Select-String -Path $log -Pattern 'shadow gate:' -ErrorAction SilentlyContinue
    if ($trace) { $trace | ForEach-Object { Write-Host "  $($_.Line.Substring($_.Line.IndexOf('Wgpu shadow gate')))" } }
    else        { Write-Host "  no gate trace in log - did the build include it?" -ForegroundColor Yellow }
}

Invoke-Arm -Name 'fixed'  -Env @{}
Invoke-Arm -Name 'legacy' -Env @{ WGR_SHADOW_LEGACY_GATE = '1' }

Write-Host "`nCompare the two PNGs against SHD-001:" -ForegroundColor Cyan
Write-Host "  A3/B1 - ground shadows from buildings present in 'fixed', ABSENT in 'legacy'."
Write-Host "  B2    - 'legacy' must report cpuCasters=0; if not, the gate is exonerated."
