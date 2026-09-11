<#
.SYNOPSIS
    Moving-camera arm for the farfield benchmark: measures a TRAVERSE, not a pose.

.DESCRIPTION
    scripts/farfield-bench.ps1 can only measure one FIXED camera pose, warmed up until
    asset streaming has settled.  That is the number it was built to make repeatable, and
    it is the wrong number for the question "what does a player see?", because a player
    MOVES, and moving is what re-triggers residency and object streaming.  A settled pose
    can report 35 fps on a world where every moving frame is 200 ms.

    This script adds motion WITHOUT a C++ change.  It does not need one: the engine
    already has everything required.

      * `--test-mission <dir>` stages the whole mission DIRECTORY into temp storage
        (GameApplication.cpp: StageTestMissionForGame -> fs::copy recursive), and
        Poseidon/UI/DisplayUI.cpp RunInitScript() executes `init.sqs` from the staged
        mission at mission start.  So an SQS script dropped beside mission.sqm runs.
      * SQS is the scheduled dialect: `~0.001` suspends until mission time advances,
        which is exactly one rendered frame (Scripts.cpp ProcessLine case '~').
      * `"camera" camCreate` builds a CameraVehicle (VehicleTypes.cpp, simulation
        "camera"), `cameraEffect` hands it the view, and `camSetPos` + `camCommit T`
        makes the ENGINE interpolate the position over T seconds at constant velocity
        every frame (CameraHold.cpp CameraVehicle::Simulate, the !GetManual() branch).
        The motion is therefore engine-smooth and frame-rate independent — the script
        only has to post a new waypoint once a second.
      * `logInfo` writes to --log-file, and every log line carries a millisecond
        wall-clock stamp.  One `logInfo` per frame is therefore a complete per-frame
        frame-time trace, which is what a p99 needs and what the capture JSON (avg /
        p95 / max over a 256-frame ring) cannot give.
      * `triFrameCount` returns the RENDERER's frame counter, so every trace line carries
        the engine's own frame number.  That is what makes the trace trustworthy rather
        than plausible: if the script were ticking twice per frame, or skipping frames
        under load, the deltas would silently be sub-frame or multi-frame and the whole
        measurement would be wrong in the direction that flatters it.  The counter turns
        that into something the analyser checks and reports.

    WHY THE CAMERA IS SCRIPTED IN BOTH ARMS.  `--test-world-freefly` builds a MANUAL
    CameraVehicle, and the manual branch of Simulate() ignores _movePos entirely — a
    scripted camSetPos on it would do nothing.  So the script creates its own
    non-manual camera and takes the view with `cameraEffect`.  -Freefly is still passed,
    because it also grounds the player (an ungrounded player drowns on a substituted
    world and GModeArcade then ENDS THE MISSION, exiting the app with code 0) and sets
    the world hour.  Its camera stays parked and unused.

    ONE RUN, BOTH ARMS.  The generated init.sqs runs three phases off MISSION time:

        settle  (-SettleSeconds)  camera static, streaming fills in.  Discarded.
        static  (-StaticSeconds)  camera static, measured.       FT lines tagged `S`.
        move    (-MoveSeconds)    camera translating at -Speed.   FT lines tagged `M`.

    Static and moving therefore come from the SAME process, the same fill-in, the same
    thermal state and the same driver state.  Two separate runs could not claim that.
    triPerfReset is called at each phase boundary and triPerfStats at each phase end, so
    the log also carries an engine-side CPU phase breakdown per phase.

    triPerfSeries is called at each phase end too, and it is the one that can answer
    "does the engine alternate work between frames?".  The FT lines above are differences
    between LOG TIMESTAMPS, which are whole milliseconds -- roughly 4% of a frame at
    38 fps -- so a steady 26.5 ms frame reads as 26, 27, 26, 27 and looks exactly like a
    strict every-other-frame alternation.  triPerfSeries dumps FrameProfiler's own
    float-millisecond ring (the last 256 frames of the phase) in order, so the question
    is decidable rather than an artefact of the trace's resolution.

.PARAMETER Speed
    Metres per second of horizontal translation along -Azimuth.  0 gives a second static
    phase, which is the control that proves the moving phase's cost is the MOTION and not
    the passage of time.  ~5 = infantry, ~30 = vehicle.

.PARAMETER StartY
    ABSOLUTE altitude of the camera, matching -Freefly's third component.  SQF positions
    are terrain-relative ([x, z, height-above-surface]: GameStateExtGrp.cpp GetPos adds
    SurfaceYAboveWater), so -GroundY is subtracted from this to get the SQF value.  Read
    -GroundY out of a -Freefly run's log line "--test-world-freefly: grounded the player
    at X, <groundY>, Z" and subtract the 1.8 m eye offset it adds.

.EXAMPLE
    .\scripts\farfield-movecam.ps1 -Label everon-walk -Speed 5 `
        -Mission "D:\...\dev-missions\devtest-dawn.abel" `
        -World "C:\...\Temp\rf\world\ev_DALL.wrp" `
        -Start 5200,7000 -StartY 220 -GroundY 12.4 -Azimuth 45 -Elevation -8 `
        -WorldHour 10 -SettleSeconds 130 -StaticSeconds 45 -MoveSeconds 150 `
        -Width 1600 -Height 900
    python .\scripts\farfield-frametimes.py .tmp-movecam\out\everon-walk\run-01.log

.NOTES
    Windows PowerShell 5.1.  No &&, ||, ternary or ?? anywhere in this file.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Label,

    [Parameter(Mandatory = $true)]
    [string]$Mission,

    [string]$World = '',

    # X,Z of the start pose — the first two components of -Freefly, same order.
    [Parameter(Mandatory = $true)]
    [double[]]$Start,

    # Absolute altitude (the third -Freefly component).
    [double]$StartY = 220,

    # Terrain height under -Start.  Converts -StartY into the terrain-relative altitude
    # SQF wants.  0 is honest but means the camera flies -StartY metres ABOVE the ground
    # instead of at absolute -StartY; on flat coastal ground the difference is small.
    [double]$GroundY = 0,

    [double]$Azimuth = 45,
    [double]$Elevation = -8,

    # Metres per second.  0 = a static control phase.
    [Parameter(Mandatory = $true)]
    [double]$Speed,

    # Mission-time seconds.  Settle must exceed the streaming fill-in (grep the log for
    # `pending=0`); Everon at 1600x900 is ~107 s.
    [double]$SettleSeconds = 130,
    [double]$StaticSeconds = 45,
    [double]$MoveSeconds = 150,

    # Seconds between waypoints.  The ENGINE interpolates between them, so this is not
    # the update rate of the motion — it only has to be slower than the script can tick.
    [double]$WaypointSeconds = 1.0,

    # How far AHEAD of the current position each waypoint is aimed, in units of
    # -WaypointSeconds.  This is not a tuning knob, it is a divide-by-zero guard, and it
    # must stay above 1.
    #
    # CameraVehicle::Simulate steers with `_speed = offset / (_movePosTime - Glob.time)`
    # (CameraHold.cpp).  That divisor is the time left until the commit deadline, so if a
    # waypoint is committed over exactly one interval and then re-posted one interval
    # later, the last frame before the re-post divides by ~0 and the camera is flung
    # millions of metres away.  On a world where a frame can be 1.5 s that is not a rare
    # race, it is routine: it happened twice in 112 s on the first 30 m/s run, and each
    # teleport dragged the residency window to the map corner and back, manufacturing
    # streaming churn that no player would ever cause.
    #
    # Aiming N intervals ahead and re-posting every interval leaves (N-1) intervals of
    # slack on the divisor at all times.  The commanded speed is unchanged: the target is
    # N intervals of travel away AND N intervals in the future, so offset/remaining is
    # still exactly -Speed.
    [ValidateRange(2, 20)]
    [int]$WaypointLead = 4,

    # Horizontal distance to the look-at point.  Larger = less elevation wobble from
    # terrain relief under the target, because camSetTarget is terrain-relative too.
    [double]$LookDistance = 2000,

    [double]$WorldHour = -1,
    [string]$AddonRoot = '',
    [string[]]$Mod = @(),

    [int]$Width = 1600,
    [int]$Height = 900,

    [string]$Out = '',
    [Alias('EnvVars')]
    [hashtable]$Env = @{},
    [int]$Repeats = 1,
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',

    # Wall-clock second at which --auto-screenshot fires.  0 = auto: estimated mission
    # load time + settle + static + most of the move phase, so the capture's 256-frame
    # profiler ring holds MOVING frames only.
    [double]$CaptureAt = 0,

    # Estimated wall seconds from process start to the first gameplay frame.  Only used
    # to place -CaptureAt.  Read "Mission time zero" out of a previous run's log.
    [double]$LoadSeconds = 75,

    [switch]$AllowConcurrent,
    [switch]$SampleMemory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 1.0

$scriptDir = $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptDir
$bench = Join-Path $scriptDir 'farfield-bench.ps1'
if (-not (Test-Path -LiteralPath $bench)) { throw "farfield-bench.ps1 not found next to this script: $bench" }

if ($Start.Count -ne 2) { throw "-Start takes exactly 2 values (X Z); got $($Start.Count)." }
if ($Speed -lt 0) { throw '-Speed cannot be negative.' }
if ($WaypointSeconds -le 0) { throw '-WaypointSeconds must be positive.' }

if ([string]::IsNullOrWhiteSpace($Out)) { $Out = Join-Path $repoRoot '.tmp-movecam\out' }
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $repoRoot $Out }

# Resolve the source mission the same way farfield-bench.ps1 does, so a relative path
# behaves identically in both scripts.
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

# ----------------------------------------------------------------------------------------
# Stage a private copy of the mission and generate its init.sqs
# ----------------------------------------------------------------------------------------
$stageRoot = Join-Path $repoRoot '.tmp-movecam\missions'
$missionName = Split-Path -Leaf $sourceMission
# Keep the .abel/.eden suffix: ResolveMissionFile and the world name both come off it.
# Verbose capture labels must not exceed the legacy mission identifier's 79-byte limit.
$stagedName = 'mc_' + [guid]::NewGuid().ToString('N').Substring(0,12) + [IO.Path]::GetExtension($missionName)
$stagedMission = Join-Path $stageRoot $stagedName
if (Test-Path -LiteralPath $stagedMission) { throw "Unique staging directory already exists: $stagedMission" }
New-Item -ItemType Directory -Force -Path $stagedMission | Out-Null
Copy-Item -Path (Join-Path $sourceMission '*') -Destination $stagedMission -Recurse -Force

# InvariantCulture on every number: this host is a comma-decimal locale, and "5,5" in an
# SQS array is two array elements, not one number.  That failure is silent — the array
# parses, the camera goes somewhere else, and the run looks valid.
function Fmt {
    param([double]$Value)
    return [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:0.####}', $Value)
}

$sqfAlt = $StartY - $GroundY
$sqs = @"
; GENERATED by scripts/farfield-movecam.ps1 for arm '$Label' — do not edit by hand.
;
; Phases run off MISSION time (_time), not wall clock, because the wall clock includes a
; mission load whose length depends on the world.  One `logInfo` per frame per phase is
; the frame-time trace; the log's own millisecond stamp is the measurement, the mission
; time in the line is only there to prove the two agree.
_x0 = $(Fmt $Start[0])
_z0 = $(Fmt $Start[1])
_alt = $(Fmt $sqfAlt)
_azi = $(Fmt $Azimuth)
_ele = $(Fmt $Elevation)
_spd = $(Fmt $Speed)
_settle = $(Fmt $SettleSeconds)
_statdur = $(Fmt $StaticSeconds)
_movedur = $(Fmt $MoveSeconds)
_wpsec = $(Fmt $WaypointSeconds)
_lead = $(Fmt ($WaypointSeconds * $WaypointLead))
_look = $(Fmt $LookDistance)

; Unit heading vector.  OFP trig is in DEGREES and azimuth is compass-style, so X is the
; sine and Z the cosine — the same convention GameApplication.cpp uses to turn
; --test-world-freefly's azimuth into a direction.
_dx = sin _azi
_dz = cos _azi
_lx = _look * _dx
_lz = _look * _dz
; Vertical drop of the look-at point.  tan is not in the evaluator; sin/cos is.
_ly = _look * (sin _ele) / (cos _ele)

logInfo format ["FFCAM cfg x=%1 z=%2 alt=%3 azi=%4 ele=%5 spd=%6 settle=%7 static=%8 move=%9", _x0, _z0, _alt, _azi, _ele, _spd, _settle, _statdur, _movedur]

; Let the world exist before creating anything in it.
~1
_cam = "camera" camCreate [_x0, _z0, _alt]
? isNull _cam : logInfo "FFCAM FATAL camCreate returned null"
_cam cameraEffect ["internal", "back"]
_cam camSetTarget [_x0 + _lx, _z0 + _lz, _alt + _ly]
_cam camSetPos [_x0, _z0, _alt]
_cam camCommit 0
logInfo format ["FFCAM camera created at %1", getPos _cam]

; ---------------------------------------------------------------- settle (discarded) ---
#settle
logInfo format ["FT W %1 %2", _time, triFrameCount]
~0.001
? _time < _settle : goto "settle"
_r = triPerfReset 0
logInfo format ["FFCAM phase=static t=%1 pos=%2", _time, getPos _cam]
_c = triPerfCapture 16384
_statend = _time + _statdur

; ------------------------------------------------------------------- static (measured) -
#stat
logInfo format ["FT S %1 %2", _time, triFrameCount]
~0.001
? _time < _statend : goto "stat"
_c = triPerfCapture 0
_s = triPerfStats 0
_q = triPerfSeries 0
logInfo format ["FFCAM staticend t=%1 pos=%2", _time, getPos _cam]
_r = triPerfReset 0

; ------------------------------------------------------------------- moving (measured) -
_t0 = _time
_moveend = _time + _movedur
_nextwp = 0
_wpn = 0
logInfo format ["FFCAM phase=move t=%1", _time]
_c = triPerfCapture 16384

#mov
logInfo format ["FT M %1 %2", _time, triFrameCount]
? _time >= _nextwp : goto "waypoint"
#resume
~0.001
? _time < _moveend : goto "mov"
_c = triPerfCapture 0
_s = triPerfStats 0
_q = triPerfSeries 0
logInfo format ["FFCAM moveend t=%1 pos=%2 waypoints=%3", _time, getPos _cam, _wpn]
logInfo "FFCAM done"
exit

; Post the position the camera should occupy _lead seconds from NOW and commit over the
; same _lead seconds, then re-post every _wpsec.  offset/remaining is therefore always
; exactly _spd, while `remaining` never falls below (_lead - _wpsec) — see -WaypointLead
; for why a divisor that reaches zero throws the camera off the map.
;
; Distance is computed from elapsed MISSION time rather than from a waypoint counter, so
; a late script tick does not slow the camera down: the speed stays -Speed m/s whatever
; the frame rate does.
#waypoint
_nextwp = _time + _wpsec
_wpn = _wpn + 1
_d = (_time - _t0 + _lead) * _spd
_cx = _x0 + _dx * _d
_cz = _z0 + _dz * _d
_cam camSetPos [_cx, _cz, _alt]
_cam camSetTarget [_cx + _lx, _cz + _lz, _alt + _ly]
_cam camCommit _lead
; Log the ACTUAL position often enough to catch a camera that has left the world. A
; traverse that teleported is not a traverse, and the frame times it produced belong to
; nothing; without this the only symptom is an odd residency trace.
? _wpn % 5 == 1 : logInfo format ["FFCAM wp %1 t=%2 d=%3 pos=%4", _wpn, _time, _d, getPos _cam]
goto "resume"
"@

# ASCII, no BOM: the SQS reader is a byte reader, and a UTF-8 BOM lands on the first line,
# which is a comment here only by luck.  Set-Content -Encoding UTF8 writes a BOM on 5.1.
[IO.File]::WriteAllText((Join-Path $stagedMission 'init.sqs'), ($sqs -replace "`r`n", "`n"), [Text.Encoding]::ASCII)

# ----------------------------------------------------------------------------------------
# Hand off to farfield-bench.ps1
# ----------------------------------------------------------------------------------------
$capture = $CaptureAt
if ($capture -le 0) {
    # Land the capture 15 s before the traverse ends: late enough that the profiler's
    # 256-frame ring holds only moving frames even at 4 fps (256/4 = 64 s), early enough
    # that a slow load has not pushed it past the end of the phase.
    $capture = $LoadSeconds + $SettleSeconds + $StaticSeconds + $MoveSeconds - 15
}

Write-Output "=== farfield-movecam: arm '$Label' ==="
Write-Output "  staged mission : $stagedMission"
Write-Output "  start          : $(Fmt $Start[0]), $(Fmt $Start[1]) at absolute Y $(Fmt $StartY) (SQF alt $(Fmt $sqfAlt), groundY $(Fmt $GroundY))"
Write-Output "  heading        : azimuth $(Fmt $Azimuth) deg, elevation $(Fmt $Elevation) deg"
Write-Output "  speed          : $(Fmt $Speed) m/s  ->  $(Fmt ($Speed * $MoveSeconds)) m over the move phase"
Write-Output "  phases (mission s): settle $(Fmt $SettleSeconds) | static $(Fmt $StaticSeconds) | move $(Fmt $MoveSeconds)"
Write-Output "  capture at     : $(Fmt $capture) s wall"
Write-Output "  resolution     : ${Width}x${Height}"

$benchArgs = @{
    Label         = $Label
    Mission       = $stagedMission
    Repeats       = $Repeats
    WarmupSeconds = $capture
    Width         = $Width
    Height        = $Height
    Out           = $Out
    GameDir       = $GameDir
    Env           = $Env
}
if (-not [string]::IsNullOrWhiteSpace($World)) {
    $benchArgs['World'] = $World
    # The freefly pose is passed for its SIDE EFFECTS, not its camera: it grounds the
    # player so a drowned player cannot end the mission, and it is what --test-world-hour
    # is gated behind.  The script's own camera owns the view from the first second.
    $benchArgs['Freefly'] = @($Start[0], $Start[1], $StartY, $Azimuth, $Elevation)
}
if ($WorldHour -ge 0) { $benchArgs['WorldHour'] = $WorldHour }
if (-not [string]::IsNullOrWhiteSpace($AddonRoot)) { $benchArgs['AddonRoot'] = $AddonRoot }
if ($Mod.Count -gt 0) { $benchArgs['Mod'] = $Mod }
if ($AllowConcurrent) { $benchArgs['AllowConcurrent'] = $true }
if ($SampleMemory) { $benchArgs['SampleMemory'] = $true }

& $bench @benchArgs
exit $LASTEXITCODE
