<#
.SYNOPSIS
    Phase 5 step 6's equivalence gate: do two runs publish the same frames?

.DESCRIPTION
    Runs the installed game twice under the determinism mode (SIM-816) with the
    per-frame publication hash on, aligns the two frame streams BY SIMULATION TICK,
    and reports whether every frame of the scene is content-identical.

    Exits 0 when they are, 1 when they are not, 2 when a run failed or produced
    nothing to compare. So it is usable as a gate, not only as a readout.

    IT IS A DETERMINISM ORACLE, NOT A CORRECTNESS ORACLE. It asks whether two runs
    publish the same frames -- not whether those frames are RIGHT. A change that
    breaks both runs identically passes at 100%.

    That is not hypothetical. Moving the drains next to the render call left the
    CPU-mesh handle patch running before the creates it resolves, so every mesh
    created and drawn in the same frame lost its draw. This gate returned 901 of
    901 identical through the whole regression, because both runs dropped the same
    draws. What caught it was a counter that refuses to drop a draw silently.

    So: use this to prove a restructuring did not CHANGE anything, and use the
    engine's own anomaly counters and a capture you look at to prove it did not
    BREAK anything. Neither substitutes for the other.

    Two rules are built in, and both were learned the expensive way:

      * ALIGN BY TICK, NEVER BY FRAME INDEX. The mission load takes a variable
        number of frames -- measured spread 23 frames across three runs of one
        build -- so frame N of two runs has not simulated the same world.
        REN-THR-005 records three documents' worth of measurements that were
        really measuring how long the loading screen lasted.

      * A FRAME WITH `draws3d` EMPTY IS NOT A FRAME OF THE SCENE. That excludes
        the loading screen at the front and the teardown frame after the capture
        at the back. Both look like renderer divergence and are neither.

      * NOTHING MAY TOUCH THE WINDOW WHILE A RUN IS MEASURING. The runs open a
        real window and the game takes real input; a stray click or mouse move
        over it moves the camera, and from that tick onward the two runs are
        simulating different things. This is the single most likely reason for a
        large divergence that starts partway through a run and never recovers.

        That rule was learned by getting it wrong. A run that diverged from three
        others in 710 ticks was written up here as "the first run after a new
        binary is not comparable", with a cold-cache-and-streaming-budget
        rationale attached. The owner suggested they had nudged the camera during
        it. The test that settles it: after a fresh deploy, with nobody at the
        keyboard, the first run is bit-identical to the second AND to a run made
        before the deploy. The binary-freshness explanation is refuted; the input
        one stands. A warm-up run is kept below anyway -- it is nearly free and it
        also absorbs first-launch shader compilation -- but it is insurance, not
        a measured requirement, and this comment says so rather than dressing it
        up as one.

    Compares two BUILDS by passing -CompareLogs with two logs captured earlier
    from different binaries (see -KeepLogs). The comparison is identical; only
    the bookkeeping of which binary produced which log is the operator's, because
    DEPLOYED-FROM.txt is stamped from HEAD and cannot see a patched working copy.

.PARAMETER Mission
    Mission to run. Defaults to the combat scene, which is the one with AI,
    vehicles, radio traffic and particles -- i.e. the one whose determinism is
    worth asserting. A static scene passes this gate trivially.

.PARAMETER Frames
    Screenshot delay in frames; the run ends there. Default 900.

.PARAMETER KeepLogs
    Directory to leave the two logs in, for a later cross-build comparison.

.PARAMETER CompareLogs
    Two existing logs to compare instead of running the game.

.EXAMPLE
    .\scripts\Check-FrameEquivalence.ps1
    .\scripts\Check-FrameEquivalence.ps1 -KeepLogs C:\tmp\before
    .\scripts\Check-FrameEquivalence.ps1 -CompareLogs C:\tmp\before\run-1.log, C:\tmp\after\run-1.log
#>
[CmdletBinding()]
param(
    [string]$Mission = 'tests/perf/missions/perf_combat.eden',
    [int]$Frames = 900,
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$KeepLogs = '',
    [string[]]$CompareLogs = @(),
    [int]$TimeoutSeconds = 600
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The determinism mode. Each of these was traced to a specific clock reaching the
# published frame; SIM-816 has the measurements and the chain for each.
$DeterminismEnv = @{
    POSEIDON_LOCKSTEP_HZ     = '60'   # pins deltaT; also fixes the RNG seed and the radio drain
    WGR_LOD_GOVERNOR_RANGE   = '1'    # freezes the LOD governor's framerate feedback loop
    WGR_FRAME_HASH           = '1'
    WGR_FRAME_HASH_FIRST     = '0'
    WGR_FRAME_HASH_COUNT     = '100000'
    WGR_FRAME_HASH_MASK_IDS  = '1'    # mesh/texture handles ride load order; hash CONTENT
    POSEIDON_TICK_HASH       = '1'    # the tick counter this alignment depends on
    WGR_DLSS                 = '0'
}

$SliceNames = @('scal','cam','d3d','vert','bat','cmd','pal','shad','ovv','ovi','ovd','tn','tb','lit','wn','wb','grass')

function Read-FrameStream {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw "no log at $Path" }
    # Keys are STRINGS on purpose. An [ordered] dictionary with integer keys indexes by
    # POSITION, not by key, so $frames[902] silently means "the 903rd entry" -- which throws
    # once the streams are shorter than the tick numbers and, worse, would compare the wrong
    # frames if they were not.
    $frames = @{}
    $tick = 0
    $lastDraws3d = -1
    foreach ($line in [System.IO.File]::ReadLines($Path)) {
        if ($line -match 'TickHash') {
            $tick++
            continue
        }
        if ($line -match 'Wgpu frame len\[') {
            if ($line -match '\bd3d=(\d+)') { $lastDraws3d = [int]$Matches[1] }
            continue
        }
        if ($line -match 'Wgpu frame hash: frame\[') {
            # Outside the scene: loading screen at the front, teardown at the back.
            if ($lastDraws3d -le 0) { continue }
            $slices = @{}
            foreach ($m in [regex]::Matches($line, '(\w+)=([0-9a-f]{8})\b')) {
                $slices[$m.Groups[1].Value] = $m.Groups[2].Value
            }
            if ($slices.Count -ge 13) { $frames[[string]$tick] = $slices }
        }
    }
    return $frames
}

function Invoke-Run {
    param([string]$LogPath, [string]$PngPath, [int]$Index)
    $exe = Join-Path $GameDir 'OpenPoseidon.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "no OpenPoseidon.exe in $GameDir" }

    $missionPath = if ([System.IO.Path]::IsPathRooted($Mission)) { $Mission }
                   else { Join-Path (Split-Path -Parent $PSScriptRoot) $Mission }

    foreach ($k in $DeterminismEnv.Keys) {
        [Environment]::SetEnvironmentVariable($k, $DeterminismEnv[$k], 'Process')
    }
    $argList = @(
        '--check', '--render=wgpu', '--window', '--width=1600', '--height=900',
        '--test-mission', "`"$missionPath`"",
        '--test-type', 'screenshot', '--screenshot', "`"$PngPath`"",
        '--screenshot-delay', "$Frames",
        '--log-file', "`"$LogPath`"", '--log-level', 'info'
    )
    $label = if ($Index -eq 0) { 'warm-up (discarded)' } else { "run $Index" }
    Write-Host "  $label ..." -NoNewline
    $p = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $GameDir -PassThru
    if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
        try { $p.Kill() } catch {}
        throw "run $Index did not finish within $TimeoutSeconds s"
    }
    # A nonzero exit is normal here: the NGX teardown path exits 139 with DLSS built in,
    # and the capture has already been written by then. The log is the artefact.
    Write-Host " done (exit $($p.ExitCode))"
}

# ---- gather the two streams -------------------------------------------------
if ($CompareLogs.Count -eq 2) {
    Write-Host "Comparing existing logs (cross-build comparison is the operator's bookkeeping):"
    Write-Host "  A: $($CompareLogs[0])"
    Write-Host "  B: $($CompareLogs[1])"
    $a = Read-FrameStream $CompareLogs[0]
    $b = Read-FrameStream $CompareLogs[1]
}
elseif ($CompareLogs.Count -ne 0) {
    Write-Error '-CompareLogs takes exactly two log paths.'
    exit 2
}
else {
    $stamp = Join-Path $GameDir 'DEPLOYED-FROM.txt'
    if (Test-Path -LiteralPath $stamp) {
        Write-Host "Measuring: $((Get-Content -LiteralPath $stamp -Raw).Trim())"
    }
    $outDir = if ($KeepLogs) { $KeepLogs } else { Join-Path ([System.IO.Path]::GetTempPath()) ("frame-equiv-" + [System.Guid]::NewGuid().ToString('N').Substring(0,8)) }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    # Warm-up, discarded. Cheap insurance against first-launch shader compilation
    # and page-cache effects. NOT a measured requirement -- see the header for the
    # claim that was made here, and refuted.
    Invoke-Run (Join-Path $outDir 'warmup.log') (Join-Path $outDir 'warmup.png') 0
    Invoke-Run (Join-Path $outDir 'run-1.log') (Join-Path $outDir 'run-1.png') 1
    Invoke-Run (Join-Path $outDir 'run-2.log') (Join-Path $outDir 'run-2.png') 2
    $a = Read-FrameStream (Join-Path $outDir 'run-1.log')
    $b = Read-FrameStream (Join-Path $outDir 'run-2.log')
    if ($KeepLogs) { Write-Host "logs kept in $outDir" }
}

# ---- compare ----------------------------------------------------------------
$common = $a.Keys | Where-Object { $b.ContainsKey($_) } | Sort-Object { [int]$_ }
if (-not $common -or @($common).Count -eq 0) {
    Write-Error 'no scene-drawing frames in common. Either a run failed, or WGR_FRAME_HASH / POSEIDON_TICK_HASH did not reach the binary (is the installed build current?).'
    exit 2
}

$identical = 0
$perSlice = @{}
foreach ($n in $SliceNames) { $perSlice[$n] = 0 }
$firstBad = $null
foreach ($t in $common) {
    $same = $true
    foreach ($n in $SliceNames) {
        if ($a[$t].ContainsKey($n) -and $b[$t].ContainsKey($n) -and $a[$t][$n] -ne $b[$t][$n]) {
            $perSlice[$n]++
            $same = $false
        }
    }
    if ($same) { $identical++ }
    elseif ($null -eq $firstBad) { $firstBad = $t }
}

$total = @($common).Count
$pct = if ($total) { 100.0 * $identical / $total } else { 0 }
Write-Host ''
Write-Host ("scene frames compared (tick-aligned, draws3d non-empty): {0}" -f $total)
Write-Host ("content-identical across all {0} slices: {1} ({2:N2}%)" -f $SliceNames.Count, $identical, $pct)

$differing = $perSlice.GetEnumerator() | Where-Object { $_.Value -gt 0 } | Sort-Object -Property Value -Descending
if ($differing) {
    Write-Host ''
    Write-Host 'differing slices:'
    foreach ($d in $differing) { Write-Host ("  {0,-6} {1} frames" -f $d.Key, $d.Value) }
    Write-Host ("first differing tick: {0}" -f $firstBad)
    Write-Host ''
    Write-Host 'The two runs did NOT publish the same frames. Before suspecting the change under'
    Write-Host 'test: confirm the installed binary is the one you think (DEPLOYED-FROM.txt is'
    Write-Host 'stamped from HEAD and cannot see a patched working copy), then read SIM-816 for'
    Write-Host 'the three clocks the determinism mode gates and check none has been reintroduced.'
    exit 1
}

Write-Host 'EQUIVALENT: every frame of the scene is identical in both runs.'
exit 0
