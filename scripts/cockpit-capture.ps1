<#
.SYNOPSIS
    Puts the player in an aircraft's PILOT SEAT and captures the cockpit interior.

.DESCRIPTION
    The existing capture harness cannot see a cockpit.  farfield-bench.ps1 and
    farfield-movecam.ps1 both drive a FREE camera, and a free camera parked inside a
    helicopter does NOT show the cockpit — it shows the EXTERIOR LOD from the inside,
    whose backfaces are culled (`cull_mode: Some(Face::Back)`, gfx3d/mod.rs).  That looks
    exactly like a see-through fuselage and it is an artefact of the harness, not the bug.

    The cockpit LOD is selected by ONE condition, in World.cpp:1568-1570:

        if (obj == camInsideVehicle)
            _scene.ObjectForDrawing(obj, obj->InsideLOD(_camType), clip);

    and `camInsideVehicle` is only ever set from `cameraVehicle` — i.e. `_cameraOn` — in
    the CamInternal / CamGunner branches (World.cpp:1223, :1229).  So the ONLY way to draw
    a cockpit is to make the aircraft itself the camera-on object with an internal camera
    type.  `Transport::InsideLOD` (TransportCore.cpp:1227-1321) then resolves the player's
    seat to `FindSpecLevel(VIEW_PILOT)` — the pilot view LOD, which is the cockpit.

    This script does that from SQS, with no C++ change, using the same mechanism
    farfield-movecam.ps1 documents: `--test-mission <dir>` stages the mission DIRECTORY and
    DisplayUI.cpp RunInitScript() executes the `init.sqs` beside mission.sqm.  The generated
    script:

        _veh = "OH58" createVehicle [...]      VehCreate      (GameStateExt.cpp:1335)
        player moveInDriver _veh               ObjMoveInDriver(GameStateExtUi.cpp:2694)
                                               ...which already calls SwitchCameraTo(veh,...)
        _veh switchCamera "INTERNAL"           ObjSwitchCamera(GameStateExtUi.cpp:1617)
                                               -> SwitchCameraTo(veh, CamInternal)

    then parks: it holds the aircraft still, fuel at 0, and re-posts the heading every
    frame so the pose is IDENTICAL between A/B arms.  A stopped aircraft on the ground with
    a human driver has no AI and no input, so nothing moves.

    THREE CAPTURES PER RUN.  --auto-screenshot takes a comma-separated list of
    "<trigger>:<path>" (GameApplication.cpp ParseAutoScreenshots, :718) and closes the app
    two frames after the LAST one fires.  Three shots a few seconds apart cost nothing and
    mean a slow mission load cannot waste the run — all three should be identical, and if
    they are not, the pose was not settled.

.PARAMETER Vehicle
    Config class name.  Verified present in the retail install:
      OH58   oh58.pbo   Kiowa    — BEST REPRO.  Its interior maps (kiowa_insidtga,
                                   kiowa_inspanl/r/d, kiowa_sedacky, kiowa_in_doors,
                                   kiowa_panel_sid, kiowa_knipl) are AI88 and every one of
                                   them classifies AlphaStats::Blend.
      AH64   Apac.pbo   Apache   — second best (apach_kokpit1/2 are ARGB4444, both Blend).
      CH47   ch47.pbo   Chinook  — 44 of its 60 textures are DXT1 (which short-circuits to
                                   Cutout and is never scanned), so most of its hold and
                                   airframe should stay solid while its five AI88
                                   `ch47_palub_*` panel maps do not.  Useful as a
                                   WITHIN-FRAME control: the contrast is the signal.

.PARAMETER Dir
    Aircraft heading in degrees.  The internal camera is vehicle-relative and cannot be
    aimed from SQS, so this is how the view is aimed: rotating the aircraft rotates the
    whole cockpit view.  Use it to put something identifiable BEHIND the solid structure —
    0 (north, open terrain) vs 180 is usually enough to prove you are seeing through it.

.PARAMETER Env
    Environment for the child process.  The arms that matter here:
      @{ WGR_LEGACY_TEXTURE_TRACE = '1' }   one log line per texture, carrying alphaClass
                                            and the blend mode the draw actually used.
                                            THIS IS THE MEASUREMENT — see .NOTES.
      @{ WGR_COCKPIT_BLEND_LEGACY = '1' }   restores pre-MAT-048 depth-write on the
                                            object-blend pass.
      @{ WGR_TEXTURE_STREAM_STATS = '1' }   alphaScans / alphaBlockScans counters.

.EXAMPLE
    # The one run to do first.  Kiowa pilot seat, with the texture trace on.
    .\scripts\cockpit-capture.ps1 -Label kiowa-trace -Vehicle OH58 `
        -Env @{ WGR_LEGACY_TEXTURE_TRACE = '1' }

.EXAMPLE
    # A/B of the MAT-048 depth-write change, same pose both arms.
    .\scripts\cockpit-capture.ps1 -Label kiowa-new    -Vehicle OH58
    .\scripts\cockpit-capture.ps1 -Label kiowa-legacy -Vehicle OH58 `
        -Env @{ WGR_COCKPIT_BLEND_LEGACY = '1' }
    python .\scripts\compare_preview0_captures.py --help   # or just eyeball the two PNGs

.NOTES
    Windows PowerShell 5.1.  No &&, ||, ternary or ?? anywhere in this file.

    READING THE TRACE.  With WGR_LEGACY_TEXTURE_TRACE=1 the log carries, once per texture:

      Legacy texture draw trace: path=<p> vegetation=<v> alphaClass=<c> alphaRef=<r> blend=<b>

    alphaClass: 0=Opaque 1=Cutout 2=Blend   (Poseidon::AlphaStats::Kind)
    blend:      0=OPAQUE 1=ALPHA 2=ADDITIVE 3=SHADOW  (WgrBlend, wgpu_renderer.hpp:125)

    A line reading `path=...kiowa_insidtga.paa ... alphaClass=2 ... blend=1` is the bug in
    one line: a SOLID cockpit interior panel, classified Blend, submitted alpha-blended.
    This script greps those lines out of the log for you at the end.
#>
[CmdletBinding()]
param(
    # Output sub-directory name and log/PNG stem.
    [Parameter(Mandatory = $true)]
    [string]$Label,

    # Config class of the aircraft to sit in.  See .PARAMETER Vehicle.
    [string]$Vehicle = 'OH58',

    # Driver = pilot view LOD.  Gunner = gunner view LOD (Apache/Kiowa have distinct ones).
    [ValidateSet('Driver', 'Gunner')]
    [string]$Seat = 'Driver',

    # Mission DIRECTORY (init.sqs has to sit beside mission.sqm).  The default carries a
    # single WEST player on Abel at 7800,10100 and nothing else, so the frame is the
    # aircraft and the terrain and nothing that could confound the reading.
    [string]$Mission = 'tests/perf/missions/perf_abel.abel',

    # Where to put the aircraft.  Defaults to the perf_abel player's own position, so the
    # player is already there and moveInDriver cannot fail on distance.
    [double[]]$Pos = @(7800.0, 10100.0),

    # Aircraft heading, degrees.  This is how the cockpit view is aimed — see .PARAMETER Dir.
    [double]$Dir = 0,

    # Wall-clock seconds at which each --auto-screenshot fires.  The app closes two frames
    # after the last one.  Must be ascending.
    [double[]]$CaptureSeconds = @(24, 28, 32),

    [int]$Width = 1280,
    [int]$Height = 720,

    [ValidateSet('wgpu', 'gl33')]
    [string]$Backend = 'wgpu',

    [Alias('EnvVars')]
    [hashtable]$Env = @{},

    [string]$Out = '',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',

    # Per-run watchdog.  0 = auto (last capture + 90 s).
    [int]$TimeoutSeconds = 0,

    # Proceed even though another game process is already resident.
    [switch]$AllowConcurrent,

    # Stage the mission and write init.sqs, print the command line, and STOP.  Nothing is
    # launched.  Use it to read the generated SQS before spending a run on it.
    [switch]$GenerateOnly
)

$ErrorActionPreference = 'Stop'
# 1.0 on purpose, matching farfield-bench.ps1: absent properties must read as false, not throw.
Set-StrictMode -Version 1.0

$scriptDir = $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptDir

if ($Pos.Count -ne 2) { throw "-Pos takes exactly 2 values (X Y); got $($Pos.Count)." }
if ($CaptureSeconds.Count -lt 1) { throw '-CaptureSeconds needs at least one value.' }
for ($i = 1; $i -lt $CaptureSeconds.Count; $i++) {
    if ($CaptureSeconds[$i] -le $CaptureSeconds[$i - 1]) {
        throw '-CaptureSeconds must be strictly ascending.'
    }
}

if (-not (Test-Path -LiteralPath $GameDir)) { throw "Game directory not found: $GameDir" }
$gameExe = Join-Path $GameDir 'OpenPoseidon.exe'
if (-not (Test-Path -LiteralPath $gameExe)) {
    throw "Game executable not found: $gameExe`n  Deploy first: .\scripts\Deploy.ps1"
}
# A number nobody can attribute to a commit is not a measurement. The ABI handshake will
# NOT catch a version skew - it only trips on a struct-layout change, so a DLL many commits
# from the exe loads perfectly cleanly and simply lacks those commits.
#
# File mtimes cannot answer this: Copy-Item preserves the source timestamp, so an
# incremental deploy that did not need to rebuild the DLL legitimately leaves it hours
# older than the exe. DEPLOYED-FROM.txt is the authority - Deploy.ps1 stamps it with the
# branch and commit of the pair it just wrote, and it is the ONLY record of what these two
# files actually are. Refuse to benchmark an unstamped install, and put the commit in the
# run metadata so the numbers carry it.
$stampPath = Join-Path $GameDir 'DEPLOYED-FROM.txt'
if (-not (Test-Path -LiteralPath $stampPath)) {
    throw "$GameDir has no DEPLOYED-FROM.txt, so there is no record of which commit these binaries are. Deploy with .\scripts\Deploy.ps1 (which stamps it) before benchmarking."
}
$deployedFrom = (Get-Content -LiteralPath $stampPath -TotalCount 1).Trim()
Write-Host "Measuring: $deployedFrom" -ForegroundColor Cyan

if ($Backend -eq 'wgpu') {
    $rendererDll = Join-Path $GameDir 'wgpu_renderer.dll'
    if (-not (Test-Path -LiteralPath $rendererDll)) {
        throw "wgpu_renderer.dll is missing from $GameDir - the exe will fail with 'Entry Point Not Found'."
    }
}

# The addon that defines the class has to be mounted or createVehicle returns null and the
# run produces three screenshots of open terrain, which looks like a pass.  Check up front.
$vehiclePbo = @{ OH58 = 'oh58.pbo'; AH64 = 'Apac.pbo'; CH47 = 'ch47.pbo' }
if ($vehiclePbo.ContainsKey($Vehicle)) {
    $pboPath = Join-Path $GameDir (Join-Path 'AddOns' $vehiclePbo[$Vehicle])
    if (-not (Test-Path -LiteralPath $pboPath)) {
        throw "-Vehicle $Vehicle needs $($vehiclePbo[$Vehicle]), which is not in $GameDir\AddOns."
    }
}

function Resolve-MissionPath {
    param([string]$Path)
    $candidates = @()
    if ([IO.Path]::IsPathRooted($Path)) { $candidates += $Path }
    else {
        $candidates += (Join-Path $repoRoot $Path)
        $candidates += (Join-Path $GameDir $Path)
        $candidates += (Join-Path (Get-Location).Path $Path)
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "Mission not found: '$Path' (looked in: $($candidates -join '; '))"
}

$sourceMission = Resolve-MissionPath -Path $Mission
if (-not (Test-Path -LiteralPath $sourceMission -PathType Container)) {
    throw "-Mission must be a mission DIRECTORY (init.sqs has to sit beside mission.sqm): $sourceMission"
}

if ([string]::IsNullOrWhiteSpace($Out)) { $Out = Join-Path $repoRoot '.tmp-cockpit\out' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $repoRoot $Out }
$armDir = Join-Path $Out $Label
New-Item -ItemType Directory -Force -Path $armDir | Out-Null

# ----------------------------------------------------------------------------------------
# Stage a private copy of the mission and generate its init.sqs
# ----------------------------------------------------------------------------------------
$stageRoot = Join-Path $repoRoot '.tmp-cockpit\missions'
$missionName = Split-Path -Leaf $sourceMission
$stagedName = ($Label -replace '[^A-Za-z0-9_-]', '_') + '_' + $missionName
$stagedMission = Join-Path $stageRoot $stagedName
if (Test-Path -LiteralPath $stagedMission) { Remove-Item -LiteralPath $stagedMission -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stagedMission | Out-Null
Copy-Item -Path (Join-Path $sourceMission '*') -Destination $stagedMission -Recurse -Force

# InvariantCulture on every number.  This host is a comma-decimal locale and "7800,5" in an
# SQS array is TWO array elements, not one number — a failure that parses cleanly, puts the
# aircraft somewhere else, and leaves the run looking valid.  (Same trap farfield-movecam
# documents; do not simplify this away.)
function Fmt {
    param([double]$Value)
    return [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.####}', $Value)
}

$moveIn = 'moveInDriver'
if ($Seat -eq 'Gunner') { $moveIn = 'moveInGunner' }

$sqs = @"
; GENERATED by scripts/cockpit-capture.ps1 for arm '$Label' - do not edit by hand.
;
; Goal: make the aircraft itself the camera-on object with CamInternal, which is the ONLY
; condition under which World.cpp:1568 draws its VIEW_PILOT (cockpit) LOD.
_x = $(Fmt $Pos[0])
_y = $(Fmt $Pos[1])
_dir = $(Fmt $Dir)

logInfo format ["COCKPIT cfg veh=$Vehicle seat=$Seat x=%1 y=%2 dir=%3", _x, _y, _dir]

; Let the world and the player exist before creating anything in it.
~2

_veh = "$Vehicle" createVehicle [_x, _y, 0]
? isNull _veh : goto "fatal"
_veh setPos [_x, _y, 0]
_veh setDir _dir
; No fuel and no AI crew: a parked airframe cannot drift, and a drifting airframe would
; make the two arms of an A/B non-comparable.
_veh setFuel 0
logInfo format ["COCKPIT created %1 at %2", _veh, getPos _veh]

~1
player $moveIn _veh
~2
_veh switchCamera "INTERNAL"
~1
logInfo format ["COCKPIT seated driver=%1 gunner=%2 pos=%3", driver _veh, gunner _veh, getPos _veh]

; Hold the pose for the rest of the run.  Re-posting position/heading every frame is what
; makes two arms bit-comparable; without it a metre of settle drift moves every edge in the
; frame and a pixel diff means nothing.
#hold
_veh setPos [_x, _y, 0]
_veh setDir _dir
_veh setVelocity [0, 0, 0]
logInfo format ["COCKPIT hold t=%1 frame=%2", _time, triFrameCount]
~0.001
goto "hold"

#fatal
logInfo "COCKPIT FATAL createVehicle returned null - is the addon mounted?"
exit
"@

# ASCII, no BOM: the SQS reader is a byte reader and a UTF-8 BOM lands on the first line.
[IO.File]::WriteAllText((Join-Path $stagedMission 'init.sqs'), ($sqs -replace "`r`n", "`n"), [Text.Encoding]::ASCII)

# ----------------------------------------------------------------------------------------
# Build the command line
# ----------------------------------------------------------------------------------------
function Quote-Arg {
    param([string]$Value)
    return '"' + $Value + '"'
}

$stem = Join-Path $armDir 'shot'
$logPath = Join-Path $armDir 'run.log'
$jsonPath = Join-Path $armDir 'run.json'
$stdoutPath = Join-Path $armDir 'run.stdout.txt'
$stderrPath = Join-Path $armDir 'run.stderr.txt'
$metaPath = Join-Path $armDir 'run.meta.json'

$pngPaths = @()
$shotSpecs = @()
$shotIndex = 0
foreach ($second in $CaptureSeconds) {
    $shotIndex++
    $png = '{0}-{1:d2}.png' -f $stem, $shotIndex
    $pngPaths += $png
    $shotSpecs += ([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0}s', $second) + ':' + $png)
}
Remove-Item -LiteralPath ($pngPaths + @($logPath, $jsonPath, $stdoutPath, $stderrPath, $metaPath)) -ErrorAction SilentlyContinue

# Timed auto-screenshot mode, byte-for-byte the shape farfield-bench.ps1 uses for a
# substituted world: NO --check (without --test-type screenshot the bounded smoke exits the
# moment gameplay starts and the timed capture never fires), --window, and this script's own
# watchdog as the bound.
$argumentParts = @()
$argumentParts += "--render=$Backend"
$argumentParts += '--window'
$argumentParts += "--width=$Width"
$argumentParts += "--height=$Height"
$argumentParts += '--test-mission'
$argumentParts += Quote-Arg $stagedMission
$argumentParts += '--auto-screenshot'
$argumentParts += Quote-Arg ($shotSpecs -join ',')
$argumentParts += '--capture-metrics'
$argumentParts += Quote-Arg $jsonPath
$argumentParts += '--log-file'
$argumentParts += Quote-Arg $logPath
$argumentParts += '--log-level'
$argumentParts += 'info'
$argumentString = ($argumentParts -join ' ')

$timeout = $TimeoutSeconds
if ($timeout -le 0) { $timeout = [int]($CaptureSeconds[$CaptureSeconds.Count - 1] + 90) }

# ----------------------------------------------------------------------------------------
# Contention gate + process hygiene (RND-033): own PIDs only, never by name.
# ----------------------------------------------------------------------------------------
function Get-ForeignGameProcessIds {
    param([hashtable]$Owned)
    $ids = @()
    $running = @(Get-Process -Name 'ColdWarAssault', 'PoseidonGame' -ErrorAction SilentlyContinue)
    foreach ($process in $running) {
        if (-not $Owned.ContainsKey([int]$process.Id)) { $ids += [int]$process.Id }
    }
    return $ids
}

function Stop-OwnedProcess {
    param([int]$RootProcessId, [hashtable]$Protected)
    $victims = @()
    try {
        $children = Get-CimInstance -ClassName Win32_Process -Filter "ParentProcessId=$RootProcessId" -ErrorAction SilentlyContinue
        foreach ($child in $children) { $victims += [int]$child.ProcessId }
    }
    catch {
        Write-Verbose "Could not enumerate children of PID ${RootProcessId}: $($_.Exception.Message)"
    }
    $victims += $RootProcessId
    foreach ($victim in $victims) {
        if ($Protected.ContainsKey($victim)) {
            Write-Warning "Refusing to stop PID $victim - it was running before this capture started."
            continue
        }
        Stop-Process -Id $victim -Force -ErrorAction SilentlyContinue
    }
}

if ($GenerateOnly) {
    Write-Output "=== cockpit-capture: -GenerateOnly, nothing launched ==="
    Write-Output "  staged mission : $stagedMission"
    Write-Output "  init.sqs       : $(Join-Path $stagedMission 'init.sqs')"
    Write-Output ''
    Write-Output '--- init.sqs ---'
    Write-Output $sqs
    Write-Output ''
    Write-Output '--- command line ---'
    Write-Output "  `"$gameExe`" $argumentString"
    exit 0
}

$ownedPids = @{}
$protectedPids = @{}
$foreignAtStart = @(Get-ForeignGameProcessIds -Owned $ownedPids)
foreach ($foreignPid in $foreignAtStart) { $protectedPids[[int]$foreignPid] = $true }
if ($foreignAtStart.Count -gt 0) {
    $message = "Another game process is already running (PID(s): $($foreignAtStart -join ', '))."
    if (-not $AllowConcurrent) { throw "$message Close it, or re-run with -AllowConcurrent." }
    Write-Warning "$message Continuing because -AllowConcurrent was given."
}

$envOverrides = @{}
if ($Env) {
    foreach ($key in $Env.Keys) { $envOverrides[[string]$key] = [string]$Env[$key] }
}

Write-Output "=== cockpit-capture: arm '$Label' ==="
Write-Output "  game      : $gameExe"
Write-Output "  mission   : $stagedMission"
Write-Output "  vehicle   : $Vehicle ($Seat seat) at $(Fmt $Pos[0]), $(Fmt $Pos[1]) heading $(Fmt $Dir) deg"
Write-Output "  captures  : $($CaptureSeconds -join 's, ')s"
Write-Output "  resolution: ${Width}x${Height}  backend: $Backend"
Write-Output "  out       : $armDir"
if ($envOverrides.Count -eq 0) {
    Write-Output '  env       : (none - engine defaults)'
}
else {
    foreach ($key in ($envOverrides.Keys | Sort-Object)) { Write-Output "  env       : $key=$($envOverrides[$key])" }
}

# $Env is a parameter name here; never use $env: syntax below, always [Environment].
$savedEnv = @{}
foreach ($key in $envOverrides.Keys) {
    $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
    [Environment]::SetEnvironmentVariable($key, $envOverrides[$key], 'Process')
}

$exitCode = -1
$timedOut = $false
$elapsed = 0.0
try {
    Write-Verbose "OpenPoseidon.exe $argumentString"
    $startedAt = Get-Date
    $process = Start-Process -FilePath $gameExe -ArgumentList $argumentString -WorkingDirectory $GameDir `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
    $ownedPids[[int]$process.Id] = $true
    $exited = $process.WaitForExit($timeout * 1000)
    if (-not $exited) {
        $timedOut = $true
        Write-Warning "Run exceeded the ${timeout}s watchdog; stopping PID $($process.Id) only."
        Stop-OwnedProcess -RootProcessId ([int]$process.Id) -Protected $protectedPids
        $process.WaitForExit(10000) | Out-Null
    }
    $elapsed = ((Get-Date) - $startedAt).TotalSeconds
    try { $exitCode = [int]$process.ExitCode } catch { $exitCode = -1 }
}
finally {
    foreach ($key in $savedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($key, $savedEnv[$key], 'Process')
    }
    foreach ($ownedPid in @($ownedPids.Keys)) {
        $leftover = Get-Process -Id $ownedPid -ErrorAction SilentlyContinue
        if ($leftover) {
            Write-Warning "Cleaning up leftover capture process PID $ownedPid."
            Stop-OwnedProcess -RootProcessId ([int]$ownedPid) -Protected $protectedPids
        }
    }
}

# ----------------------------------------------------------------------------------------
# Validate.  Fail loudly; a run that produced pretty pictures of the wrong thing is the
# failure mode this whole script exists to make impossible.
# ----------------------------------------------------------------------------------------
$problems = @()
$logText = ''
if (Test-Path -LiteralPath $logPath) { $logText = (Get-Content -LiteralPath $logPath -Raw -ErrorAction SilentlyContinue) }

foreach ($png in $pngPaths) {
    if (-not (Test-Path -LiteralPath $png)) { $problems += "no screenshot at $png" }
}
if ($logText -match 'COCKPIT FATAL') {
    $problems += "createVehicle returned null - '$Vehicle' is not a mounted class in this install"
}
if ($logText -notmatch 'COCKPIT created') {
    $problems += 'init.sqs never reached "COCKPIT created" - the mission init script did not run'
}
if ($logText -notmatch 'COCKPIT seated') {
    $problems += 'init.sqs never reached "COCKPIT seated" - moveIn/switchCamera did not complete'
}
# The decisive structural check: a cockpit-only texture in the draw trace proves the
# VIEW_PILOT LOD really rendered.  Only meaningful when the trace is on, so it is a
# warning rather than a failure when it is off.
$interiorMarkers = @{
    OH58 = 'kiowa_insidtga|kiowa_inspan|kiowa_sedacky|kiowa_in_doors'
    AH64 = 'apach_kokpit|apach_in_pal|apach_in_sedlo'
    CH47 = 'ch47_palub|ch-47_in_'
}
$traceOn = ($envOverrides.ContainsKey('WGR_LEGACY_TEXTURE_TRACE') -and $envOverrides['WGR_LEGACY_TEXTURE_TRACE'] -ne '0')

Write-Output ''
if ($problems.Count -gt 0) {
    foreach ($problem in $problems) { Write-Warning $problem }
    if (Test-Path -LiteralPath $logPath) {
        Write-Output "  last log lines:"
        Write-Output ((Get-Content -LiteralPath $logPath -Tail 15 -ErrorAction SilentlyContinue) -join "`n")
    }
}
else {
    Write-Output "=== arm '$Label': $($pngPaths.Count) captures in $([math]::Round($elapsed,1))s -> $armDir ==="
    foreach ($png in $pngPaths) { Write-Output "  $png" }
}

# ----------------------------------------------------------------------------------------
# Report the measurement, not just the pictures.
# ----------------------------------------------------------------------------------------
if ($traceOn -and $logText) {
    $marker = $interiorMarkers[$Vehicle]
    Write-Output ''
    Write-Output '--- draw trace for this aircraft (alphaClass: 0=Opaque 1=Cutout 2=Blend | blend: 0=OPAQUE 1=ALPHA) ---'
    $lines = @(Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue |
        Where-Object { $_ -match 'Legacy texture draw trace' })
    $shown = 0
    foreach ($line in $lines) {
        if ($marker -and $line -notmatch $marker) { continue }
        Write-Output "  $line"
        $shown++
    }
    if ($shown -eq 0) {
        Write-Warning ('No interior texture of ' + $Vehicle + ' appears in the draw trace. ' +
            'The VIEW_PILOT LOD did not render - the capture is of something else and must not be read as a cockpit.')
        Write-Output '  (all traced textures, for triage:)'
        foreach ($line in ($lines | Select-Object -First 25)) { Write-Output "  $line" }
    }
    else {
        $blended = @($lines | Where-Object { $marker -and $_ -match $marker -and $_ -match 'blend=1' })
        Write-Output ''
        Write-Output ("  interior sections traced: $shown | drawn ALPHA-BLENDED: " + $blended.Count)
        if ($blended.Count -gt 0) {
            Write-Output '  ^ a solid interior panel submitted with blend=1 IS the translucent-cockpit bug.'
        }
    }
}
elseif (-not $traceOn) {
    Write-Output ''
    Write-Output 'Tip: re-run with -Env @{ WGR_LEGACY_TEXTURE_TRACE = ''1'' } to get the per-texture'
    Write-Output '     alphaClass/blend trace, which decides the diagnosis without reading a pixel.'
}

$meta = [PSCustomObject][ordered]@{
    label            = $Label
    generated_at     = (Get-Date).ToString('o')
    game_exe         = $gameExe
    exe_written      = (Get-Item -LiteralPath $gameExe).LastWriteTime.ToString('o')
    vehicle          = $Vehicle
    seat             = $Seat
    position         = @($Pos)
    heading          = $Dir
    mission          = $stagedMission
    backend          = $Backend
    width            = $Width
    height           = $Height
    capture_seconds  = @($CaptureSeconds)
    env              = $envOverrides
    arguments        = $argumentString
    exit_code        = $exitCode
    timed_out        = $timedOut
    elapsed_seconds  = [math]::Round($elapsed, 2)
    screenshots      = @($pngPaths)
    log_path         = $logPath
    problems         = @($problems)
}
($meta | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $metaPath -Encoding UTF8

if ($problems.Count -gt 0) {
    Write-Error -ErrorAction Continue -Message "Arm '$Label' failed: $($problems -join '; ')"
    exit 2
}
exit 0
