[CmdletBinding()]
param(
    [ValidateRange(8,120)][int]$CaptureDeadlineSeconds = 60,
    [ValidateRange(1,4)][double]$ExpiryAcceleration = 4,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'installed-proof',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons',
    [scriptblock]$FinalPoseProbe,
    [string]$FinalPoseProbeSource,
    [switch]$ArticulationSmoke,
    [switch]$AnatomicalHinges,
    [switch]$FrozenRetirementSmoke
)
$ErrorActionPreference = 'Stop'
if ($ArticulationSmoke -and $FinalPoseProbe) { throw 'Articulation smoke and read-only no-solver diagnostic are separate campaigns.' }
if ($AnatomicalHinges -and !$ArticulationSmoke) { throw 'Anatomical hinges require the complete articulation smoke campaign.' }
if ($FrozenRetirementSmoke -and !$ArticulationSmoke) { throw 'Frozen retirement requires the complete articulation smoke campaign.' }
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
function Installed-State {
    # First installed file read: preserve the stamped executable/DLL provenance.
    $stamp = [IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    if (!$stamp) { throw 'Installed deployment provenance is empty.' }
    $files = @('OpenPoseidon.exe','wgpu_renderer.dll') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{ name = $_; path = $file.FullName; sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash;
           bytes = $file.Length; writtenUtc = $file.LastWriteTimeUtc.ToString('o') }
    }
    return @{ deployedFrom = $stamp; files = @($files) }
}
$before = Installed-State
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
foreach ($path in @($mission,$NativeAddons)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required native fixture path missing: $path" }
}
$output = Join-Path $root ('build/corpse-pose/' + $Label + '-' + (Get-Date -Format yyyyMMdd-HHmmss) + '-' + [guid]::NewGuid().ToString('N').Substring(0,6))
$profile = Join-Path $output 'user'
New-Item -ItemType Directory -Force -Path $profile | Out-Null
$log = Join-Path $output 'engine.log'
$result = [ordered]@{ status = 'running'; scope = 'installed no-motion affine pose and lifecycle proof; no solver or appearance acceptance';
    before = $before; gates = @{}; observations = @{}; captures = @{}; error = $null;
    saveNetwork = 'Not exercised by this runner; no save/network fields were added.' }
if ($FinalPoseProbe) { $result.scope = 'installed no-motion phase/final-pose measurements and lifecycle proof; no solver, joint or contact admission' }
if ($ArticulationSmoke) { $result.scope = 'installed opt-in stock corpse Box3D articulation, consumed pose and handle cleanup; appearance/contact promotion remains open' }
if ($AnatomicalHinges) { $result.scope = 'installed separate opt-in stock corpse measured elbow/knee hinges, consumed pose and handle cleanup; defaults and broad admission remain open' }
if ($FrozenRetirementSmoke) { $result.scope = 'installed explicit owned affine corpse freeze, exact full consumed pose retention and bounded fixture generation invalidation; ordinary deaths and automatic expiry freeze remain disabled' }
$result.observations.jointPolicy = if ($AnatomicalHinges) { 'measured elbow/knee revolute hinges; six spherical joints' } elseif ($ArticulationSmoke) { 'ten spherical joints' } else { 'solver=none' }
$keys = @('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_GRASS','POSEIDON_WIND_OVERRIDE',
    'WGR_CLOUD_COVERAGE','WGR_LOD_GOVERNOR_RANGE','WGR_TERRAIN_JITTER')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
@{ scriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash; sourceHead = (& git -C $root rev-parse HEAD);
   installed = $before; mission = $mission; nativeAddons = $NativeAddons; lockOwner = $env:LOCK_OWNER;
   stockClass = 'SoldierWB'; spawn = 'getPos player, group player, assigned via createUnit init';
   camera = 'actual getPosASL corpse, four metres behind and 2.7 metres above';
   fixedDate = @(1985,6,21,16,0); pausedImageComparison = $true; captureDeadlineSeconds = $CaptureDeadlineSeconds;
   expiryAcceleration = $ExpiryAcceleration; temporal = 0; exposure = 1; grass = 0; wind = '0 90 0';
   articulationSmoke = [bool]$ArticulationSmoke; anatomicalHinges = [bool]$AnatomicalHinges;
   frozenRetirementSmoke = [bool]$FrozenRetirementSmoke;
   finalPoseProbe = if ($FinalPoseProbeSource) { @{ path = $FinalPoseProbeSource; sha256 = (Get-FileHash -LiteralPath $FinalPoseProbeSource).Hash;
       snowDepth = $env:POSEIDON_SNOW_TEST_DEPTH; snowline = $env:POSEIDON_SNOWLINE; objectSnow = $env:WGR_OBJECT_SNOW } } else { $null };
   gates = @('live-refusal','phase-consumer-equivalence','status-identity','explicit-clear','30-second-expiry','hide-invalidation','delete-invalidation') } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
$p = $null; $client = $null; $writer = $null; $reader = $null
function Assert-RunHealth {
    if ($p -and $p.HasExited) { throw "Owned game exited early: $($p.ExitCode)" }
    if ((Test-Path -LiteralPath $log) -and
        (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|StartAutoTest could not boot' -Quiet)) {
        throw 'Installed runtime or mission failure; reject this run.'
    }
}
function Send($command,[switch]$AllowRefusal) {
    Assert-RunHealth
    $request = $command | ConvertTo-Json -Compress
    $request | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl')
    $writer.WriteLine($request)
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Harness response deadline exceeded.' }
        $line = $reader.ReadLine()
        if ($null -eq $line) { throw 'Harness connection closed.' }
        $line | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl')
        $reply = $line | ConvertFrom-Json
    } while ($null -eq $reply.ok)
    if (!$reply.ok -and !$AllowRefusal) { throw $line }
    return $reply
}
function Eval([string]$code) {
    $reply = Send @{ cmd = 'eval'; code = $code }
    if ($null -eq $reply.result) { throw "Evaluator result missing: $code" }
    # reply.result is an SQF value display, not nested JSON. Strings retain
    # literal model-path backslashes and escape embedded quotes by doubling them.
    $display = ([string]$reply.result).Trim()
    if ($display.StartsWith('"')) {
        if ($display.Length -lt 2 -or !$display.EndsWith('"')) { throw "Malformed SQF string: $code" }
        $inner = $display.Substring(1,$display.Length-2)
        $decoded = [Text.StringBuilder]::new()
        for ($i = 0; $i -lt $inner.Length; ++$i) {
            if ($inner[$i] -eq '"') {
                if ($i+1 -ge $inner.Length -or $inner[$i+1] -ne '"') { throw "Malformed doubled SQF quote: $code" }
                ++$i
            }
            $null = $decoded.Append($inner[$i])
        }
        return $decoded.ToString()
    }
    return ConvertFrom-Json -InputObject $display -NoEnumerate
}
function Query([string]$command) {
    $value = Eval ('triPhysicsShowcase "' + $command + '"')
    if ($value -isnot [string] -or $value -notmatch '^(OK|REFUSED|MISMATCH) corpse-') {
        throw "Missing or unknown corpse diagnostic response for $command`: $value"
    }
    return $value
}
function Require([bool]$condition,[string]$message) { if (!$condition) { throw $message } }
function Simulation-Time {
    $value = Eval 'time'
    $number = 0.0
    if (![double]::TryParse([string]$value,[Globalization.NumberStyles]::Float,$culture,[ref]$number) -or
        [double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw 'Invalid simulation time.' }
    return $number
}
function Set-Acceleration([double]$value) {
    $null = Send @{ cmd = 'exec'; code = ('setAccTime ' + $value.ToString('R',$culture)) }
}
function Assert-ArticulatedBackend([bool]$active,[string]$phase) {
    $actual=Query 'corpse-physics-status'
    $expected=if($active) {'articulatedBodies=11 articulatedJoints=10$'} else {'articulatedBodies=0 articulatedJoints=0$'}
    Require ($actual -match ('^OK corpse-physics-status .*'+$expected)) "Actual articulated handle counts failed at $phase`: $actual"
    $result.gates[$phase]=@{passed=$true;active=$active;actual=$actual}
}
function Start-Articulation([string]$phase) {
    $owner=Query 'corpse-status'
    Require ($owner -match '^OK corpse-status entity=(\S+) ') "Captured identity unavailable at $phase`: $owner"
    $capturedIdentity=$Matches[1]
    $actual=Query $(if ($AnatomicalHinges) { 'corpse-articulate-hinges' } else { 'corpse-articulate' })
    $hingeSuffix=if ($AnatomicalHinges) { ' hinges=4$' } else { '$' }
    Require ($actual -match ('^OK corpse-articulate entity=(\S+) solver=box3d [\d.]+(?:\+954cf87)? bodies=11 joints=10 epoch=\d+ minimumClearance=([\d.eE+-]+) expiresMs=30000 default=off'+$hingeSuffix)) "Actual articulated admission refused at $phase`: $actual"
    Require ($Matches[1] -ceq $capturedIdentity) "Articulated owner changed at $phase`: $actual"
    $clearance=[double]::Parse($Matches[2],$culture)
    Require (![double]::IsNaN($clearance) -and ![double]::IsInfinity($clearance) -and $clearance -ge .003) "Actual collider clearance is invalid: $actual"
    $result.observations[$phase]=$actual
    Assert-ArticulatedBackend $true ($phase+'Handles')
}
function Finite-Vector($values,[int]$length,[string]$name) {
    Require ($values -is [array] -and $values.Count -eq $length) "Missing full numeric array: $name"
    foreach ($value in $values) {
        Require ($null -ne $value -and $value -is [ValueType] -and $value -isnot [bool] -and
            ![double]::IsNaN([double]$value) -and ![double]::IsInfinity([double]$value)) "Invalid numeric array: $name"
    }
}
function Pose-Snapshot([bool]$frozen) {
    $actual=Send @{cmd='corpse_pose_state'}
    Require ($actual.readonly -eq $true -and $actual.frozen -is [bool] -and $actual.frozen -eq $frozen -and
        $actual.solver -is [bool] -and $actual.solver -eq !$frozen -and $actual.retainedBounds -eq $true) 'Missing owned full pose diagnostic.'
    Require ($actual.entity -match '^[0-9A-Fa-f]+$' -and $actual.model -ieq 'data3d\mc vojakw2.p3d' -and
        $actual.bones -is [array] -and $actual.bones.Count -eq 33) 'Actual stock owner/census unavailable.'
    Finite-Vector $actual.objectTransform 12 'object transform'
    Finite-Vector $actual.retainedMinimum 3 'retained minimum'; Finite-Vector $actual.retainedMaximum 3 'retained maximum'
    Finite-Vector @($actual.retainedRadius) 1 'retained radius'
    Require ($actual.retainedRadius -gt 0) 'Retained culling radius is invalid.'
    foreach ($correction in @('headCorrection','gunCorrection','legCorrection')) {Finite-Vector $actual.$correction 12 $correction}
    Require ($actual.levels -is [array] -and $actual.levels.Count -ge 6 -and $actual.levels.Count -le 32 -and $actual.proxies -is [array] -and
        $actual.proxies.Count -gt 0) 'Actual complete levels/proxies unavailable.'
    $seenLevels=@()
    foreach ($level in $actual.levels) {
        Finite-Vector @($level.level,$level.points,$level.compared,$level.skippedPointTails) 4 'level census'
        foreach ($count in @($level.level,$level.points,$level.compared,$level.skippedPointTails)) {Require ([double]$count -eq [int]$count) 'Nonintegral level count.'}
        Require ($level.points -gt 0 -and $level.points -le 4096 -and $level.palette -is [array] -and
            $level.palette.Count -eq 33 -and $level.palettePoints -is [array] -and $level.palettePoints.Count -eq $level.points -and
            $level.consumerPoints -is [array] -and $level.consumerPoints.Count -eq $level.compared -and
            $level.consumerPointIndices -is [array] -and $level.consumerPointIndices.Count -eq $level.compared -and
            $level.compared+$level.skippedPointTails -eq $level.points) 'Full actual point coverage is unavailable.'
        Require ($level.level -ge 0 -and $level.level -lt $actual.levels.Count -and $seenLevels -notcontains $level.level -and
            $level.compared -ge 0 -and $level.skippedPointTails -ge 0) 'Actual level census is incomplete.'
        $seenLevels+=$level.level
        foreach ($matrix in $level.palette) {Finite-Vector $matrix 12 'palette matrix'}
        foreach ($point in $level.palettePoints) {Finite-Vector $point 3 'palette point'}
        foreach ($point in $level.consumerPoints) {Finite-Vector $point 3 'consumed point'}
        $last=-1
        foreach ($index in $level.consumerPointIndices) {
            Require ($null -ne $index -and $index -is [ValueType] -and $index -isnot [bool] -and [double]$index -eq [int]$index -and $index -gt $last -and $index -lt $level.points) 'Invalid consumed point source index.'
            $last=[int]$index
        }
    }
    foreach ($proxy in $actual.proxies) {
        Finite-Vector $proxy.matrix 12 'proxy matrix'
        Require ($seenLevels -contains $proxy.level -and $proxy.selection -is [string] -and $proxy.selection.Length -gt 0) 'Actual proxy source identity unavailable.'
    }
    return $actual
}
function Exact-OwnedPose($actual) {
    # Compare every returned coordinate/matrix, rather than cardinality or hashes
    # alone. Omit only changing clock and explicit solver/frozen status fields.
    $levels=@($actual.levels | ForEach-Object {
        [ordered]@{level=$_.level;role=$_.role;points=$_.points;compared=$_.compared;skippedPointTails=$_.skippedPointTails;
            palette=$_.palette;palettePoints=$_.palettePoints;consumerPoints=$_.consumerPoints;consumerPointIndices=$_.consumerPointIndices}
    })
    return ([ordered]@{entity=$actual.entity;model=$actual.model;bones=$actual.bones;capturedMs=$actual.capturedMs;
        objectTransform=$actual.objectTransform;minimum=$actual.retainedMinimum;maximum=$actual.retainedMaximum;radius=$actual.retainedRadius;
        head=$actual.headCorrection;gun=$actual.gunCorrection;leg=$actual.legCorrection;levels=$levels;proxies=$actual.proxies} |
        ConvertTo-Json -Depth 16 -Compress)
}
function Freeze-OwnedPose([string]$phase) {
    Set-Acceleration 0
    $beforeFreeze=Pose-Snapshot $false
    $frozen=Query 'corpse-freeze'
    Require ($frozen -match '^OK corpse-frozen-status entity=(\S+) bodies=0 joints=0 epoch=\d+ updates=[1-9]\d* capturedMs=\d+ nowMs=\d+ policy=explicit-owned-pose$') "Explicit freeze failed: $frozen"
    Require ($Matches[1] -ceq $beforeFreeze.entity) 'Freeze changed actual owner.'
    $afterFreeze=Pose-Snapshot $true
    Require ((Exact-OwnedPose $beforeFreeze) -ceq (Exact-OwnedPose $afterFreeze)) 'Explicit retirement changed actual palettes, points, proxies, root or retained bounds.'
    Assert-ArticulatedBackend $false ($phase+'Handles')
    Require ((Query 'corpse-freeze') -ceq 'REFUSED corpse-freeze valid-simulating-pose-required') 'Repeated freeze was not refused.'
    Require ((Query 'corpse-impulse') -match '^REFUSED corpse-impulse ') 'Frozen corpse still accepted a solver impulse.'
    $result.gates[$phase]=@{passed=$true;before=$beforeFreeze;after=$afterFreeze;actual=$frozen}
    return $afterFreeze
}
function Screenshot([string]$name) {
    $path = Join-Path $output ($name + '.png')
    $null = Send @{ cmd = 'screenshot'; path = $path }
    $until = [DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) {
        Assert-RunHealth
        if ([DateTime]::UtcNow -gt $until) { throw "Screenshot not written: $path" }
        Start-Sleep -Milliseconds 100
    }
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 24 -or [BitConverter]::ToString($bytes[0..7]) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "Invalid PNG: $path" }
    return @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path).Hash; bytes = $bytes.Length; simulationTime = Simulation-Time }
}
function Test-ExactPausedBaseline([array]$samples) {
    if ($samples.Count -eq 0) { return $false }
    foreach ($sample in $samples) {
        Require ($sample.simulationTime -eq $samples[0].simulationTime) 'Unqueried baseline advanced simulation.'
    }
    if ($samples.Count -lt 3) { return $false }
    $last = $samples.Count - 1
    return ($samples[$last].sha256 -ceq $samples[$last-1].sha256 -and
        $samples[$last].sha256 -ceq $samples[$last-2].sha256)
}
function Wait-ExactPausedBaseline {
    # No corpse capture/compare/final-pose query occurs in this sequence. Render
    # adaptation can still advance while simulation is paused; retain its actual
    # frames rather than attributing pre-existing image drift to the diagnostic.
    $record = @{ passed = $false; samples = @(); maxCaptures = 8; exactConsecutiveRequired = 3 }
    $result.observations.pausedBaseline = $record
    for ($index = 1; $index -le 8; ++$index) {
        $record.samples += (Screenshot ('paused-baseline-{0:00}' -f $index))
        if (Test-ExactPausedBaseline $record.samples) {
            $record.passed = $true
            $selected = $record.samples[-1].Clone()
            $selected.path = Join-Path $output 'before-held.png'
            Copy-Item -LiteralPath $record.samples[-1].path -Destination $selected.path
            Require ((Get-FileHash -LiteralPath $selected.path).Hash -ceq $selected.sha256) 'Baseline artifact copy changed PNG bytes.'
            return $selected
        }
    }
    throw 'Unqueried paused baseline did not produce three exact consecutive PNGs within eight captures.'
}
function Position-Camera {
    $position = Eval 'getPosASL poseProofCorpse'
    Require ($position -is [array] -and $position.Count -eq 3) 'Corpse getPosASL did not return three coordinates.'
    foreach ($coordinate in $position) {
        Require (![double]::IsNaN([double]$coordinate) -and ![double]::IsInfinity([double]$coordinate)) 'Corpse position is nonfinite.'
    }
    $camera = @([double]$position[0],([double]$position[1]-4),([double]$position[2]+2.7),0,-34)
    $tokens = @($camera | ForEach-Object { ([double]$_).ToString('R',$culture) })
    $reply = Eval ('triFreeFlyPose "' + ($tokens -join ' ') + '"')
    Require ($reply -ceq 'OK') "Actual-corpse camera refused: $reply"
    $result.observations.corpsePosition = $position
    $result.observations.camera = $tokens -join ' '
    Start-Sleep -Seconds 2
}
function Spawn-Stock([switch]$ReuseInitialSite) {
    $spawn = '"SoldierWB" createUnit [getPos player,group player,"poseProofCorpse=this; removeAllWeapons this; this disableAI ""MOVE""; this setCombatMode ""BLUE"""]; poseProofCorpse setDir 0'
    if ($ReuseInitialSite) { $spawn += '; poseProofCorpse setPosASL poseProofFirstLiveASL' }
    $null = Send @{ cmd = 'exec'; code = $spawn }
    Require ((Eval 'isNull poseProofCorpse') -eq $false) 'Stock createUnit did not assign the fixture.'
    Require ((Eval 'poseProofCorpse == player') -eq $false) 'Fixture identity unexpectedly equals player.'
    Require ((Eval 'typeOf poseProofCorpse') -ceq 'SoldierWB') 'Spawned class is not the stock SoldierWB fixture.'
    Require ((Eval 'alive poseProofCorpse') -eq $true) 'Spawned stock fixture is not alive.'
    if (!$ReuseInitialSite) {
        $null = Send @{cmd='exec';code='poseProofFirstLiveASL=getPosASL poseProofCorpse'}
        $result.observations.firstLiveFixturePosition=Eval 'poseProofFirstLiveASL'
    }
}
function Wait-Capture {
    $until = [DateTime]::UtcNow.AddSeconds($CaptureDeadlineSeconds)
    do {
        $capture = Query 'corpse-capture'
        $result.observations.lastAdmission = $capture
        if ($capture -match '^OK corpse-capture entity=\S+ model=.+ distance=[\d.eE+-]+ nearbyCorpses=\d+ eligible=\d+ bones=\d+ primaryMatrices=\d+ secondaryMatrices=\d+ expiresMs=30000 solver=none rootMotion=authored$') { return $capture }
        Require ($capture -match '^REFUSED corpse-capture .*reason=(settled-death-pose-required|dry-ground-contact-required|no-nearby-corpse)$') "Stock capture failed a non-transient admission gate: $capture"
        if ([DateTime]::UtcNow -gt $until) { throw "Stock corpse never met the unchanged capture gate: $capture" }
        Start-Sleep -Milliseconds 500
    } while ($true)
}
function Assert-Compare([string]$name) {
    $compare = Query 'corpse-compare'
    Require ($compare -match '^OK corpse-compare .*paletteEqual=1 .*pointMismatch=0 .*proxyMismatch=0 holdValid=1$') "Production consumer equivalence failed: $compare"
    Require ($compare -match ' points=([1-9]\d*) ') "No actual point consumers compared: $compare"
    Require ($compare -match ' proxies=([1-9]\d*) ') "No actual proxy consumers compared: $compare"
    Require ($compare -match ' maxPointError=0 ') "No-motion point error differs from zero: $compare"
    $result.gates[$name] = @{ passed = $true; actual = $compare }
}
function Require-NoHold([string]$name) {
    $status = Query 'corpse-status'
    Require ($status -ceq 'REFUSED corpse-status no-valid-hold') "Hold survived $name`: $status"
    $result.gates[$name] = @{ passed = $true; actual = $status; simulationTime = Simulation-Time }
    Check-FinalPose $name $false ''
}
function Check-FinalPose([string]$phase,[bool]$held,[string]$identity) {
    if (!$FinalPoseProbe) { return }
    if (!$result.gates.finalPose) { $result.gates.finalPose = @{} }
    $sender = { param($command) Send $command -AllowRefusal }
    $result.gates.finalPose[$phase] = & $FinalPoseProbe $phase $held $identity $sender
}
function Close-Owned {
    if ($p -and !$p.HasExited) {
        Set-Acceleration 1
        $null = Send @{ cmd = 'exit' }
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) { throw 'Owned game failed to exit cleanly.' }
        if (!(Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)) { throw 'Clean shutdown marker missing.' }
    }
    if ($client) { $client.Dispose(); $script:client = $null }
    $script:writer = $null; $script:reader = $null
    if ($p -and $p.HasExited) { $result.ownedExit = @{ pid = $p.Id; exitCode = $p.ExitCode;
        shutdownMarker = [bool](Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) } }
}
try {
    $env:POSEIDON_USER_DIR = $profile
    $env:POSEIDON_REFORGER_WORLD = 'worlds/eden'; $env:POSEIDON_REFORGER_OBJECTS = '1'; $env:POSEIDON_REFORGER_STREAM = '1'
    Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue
    $env:WGR_TEMPORAL = '0'; $env:WGR_AUTO_EXPOSURE = '0'; $env:WGR_EXPOSURE = '1'
    $env:WGR_GRASS = '0'; $env:POSEIDON_WIND_OVERRIDE = '0 90 0'; $env:WGR_CLOUD_COVERAGE = '0'
    $env:WGR_LOD_GOVERNOR_RANGE = '1'; $env:WGR_TERRAIN_JITTER = '0'
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $arguments = @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000',
        '--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$NativeAddons+'"'),
        '--test-world-hour','16','--test-world-freefly','6532','6466','100','0','-30','--log-file',('"'+$log+'"'))
    $result.launchArguments = $arguments
    $p = Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments
    $null = $p.Handle
    $until = [DateTime]::UtcNow.AddSeconds(120)
    do {
        Assert-RunHealth
        $client = [Net.Sockets.TcpClient]::new()
        try { $client.Connect('127.0.0.1',$port) } catch { $client.Dispose(); $client = $null }
        if (!$client) {
            if ([DateTime]::UtcNow -gt $until) { throw 'Harness unavailable after 120 seconds.' }
            Start-Sleep -Milliseconds 250
        }
    } while (!$client)
    $stream = $client.GetStream(); $stream.ReadTimeout = 30000
    $reader = [IO.StreamReader]::new($stream)
    $writer = [IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
    Start-Sleep -Seconds 12
    Require ((Eval 'triSceneReady') -ceq 'OK') 'Mission scene not ready.'
    Require ((Eval 'alive player') -eq $true) 'Fixture mission has no living player.'
    $null = Send @{ cmd = 'exec'; code = 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setDate [1985,6,21,16,0]; setAccTime 1' }
    Require ((Query 'corpse-clear') -ceq 'OK corpse-clear cleared=0') 'Unexpected default active hold.'
    Require-NoHold 'defaultInactive'
    Require ((Query 'corpse-not-a-command') -ceq 'REFUSED corpse-command unknown') 'Unknown command did not fail explicitly.'
    $null = Send @{cmd='exec';code='poseProofOriginalPlayerASL=getPosASL player; poseProofOriginalPlayerDir=getDir player'}
    $result.observations.originalPlayerPlacement=@{position=Eval 'poseProofOriginalPlayerASL';direction=Eval 'poseProofOriginalPlayerDir'}
    Spawn-Stock
    Position-Camera
    $live = Query 'corpse-capture'
    Require ($live -match '^REFUSED corpse-capture .*nearbyCorpses=0 eligible=0 reason=no-nearby-corpse$') "Living fixture was admitted or unrelated nearby corpse exists: $live"
    $result.gates.liveRefusal = @{ passed = $true; actual = $live }
    $null = Send @{ cmd = 'exec'; code = 'poseProofCorpse setDammage 1' }
    $null = Wait-Capture
    Require ((Eval 'alive poseProofCorpse') -eq $false) 'Capture accepted a living fixture.'
    Require ((Query 'corpse-clear') -ceq 'OK corpse-clear cleared=1') 'Readiness capture did not clear.'
    Set-Acceleration 0
    Position-Camera
    $result.captures.before = if ($FinalPoseProbe) { Wait-ExactPausedBaseline } else { Screenshot 'before-held' }
    $capture = Wait-Capture
    $status = Query 'corpse-status'
    Require ($status -match '^OK corpse-status .*remainingMs=30000$') "Fresh paused lease status invalid: $status"
    $identity = [regex]::Match($capture,'entity=(\S+)').Groups[1].Value
    Require ($status -match ('entity=' + [regex]::Escape($identity) + ' ')) "Capture/status owner identity changed: $capture / $status"
    $result.gates.statusIdentity = @{ passed = $true; capture = $capture; status = $status }
    Assert-Compare 'consumerEquivalence'
    Check-FinalPose 'activeHold' $true $identity
    Check-FinalPose 'activePausedRepeat' $true $identity
    $result.captures.held = Screenshot 'held'
    Require ($result.captures.before.simulationTime -eq $result.captures.held.simulationTime) 'Before/held screenshots advanced simulation.'
    if ($ArticulationSmoke) {
        $prepared=Query 'corpse-prepare'
        Require ($prepared -match '^OK corpse-prepare solver=box3d [\d.]+(?:\+954cf87)? epoch=\d+ terrain=\d+x\d+ spacing=[\d.eE+-]+ bodies=\d+ models=\d+$') "Actual physics terrain preparation failed: $prepared"
        $result.observations.preparedPhysics=$prepared
        Assert-ArticulatedBackend $false 'defaultArticulationInactive'
        Start-Articulation 'initialArticulation'
        Assert-Compare 'articulatedZeroMotionEquivalence'
        $result.captures.articulatedAtCapture=Screenshot 'articulated-at-capture'
        Require ($result.captures.held.simulationTime -eq $result.captures.articulatedAtCapture.simulationTime) 'Articulation takeover advanced simulation.'
        Require ((Query 'corpse-impulse') -ceq 'OK corpse-impulse chest=1 impulseY=12') 'Actual chest impulse failed.'
        $motionStart=Simulation-Time; Set-Acceleration 1
        $motionDeadline=[DateTime]::UtcNow.AddSeconds(15)
        do {
            Start-Sleep -Milliseconds 100; $motionNow=Simulation-Time
            if([DateTime]::UtcNow -gt $motionDeadline) {throw 'Articulated motion clock did not advance.'}
        } while($motionNow-$motionStart -lt .6)
        Set-Acceleration 0
        $motion=Query 'corpse-articulation-status'
        Require ($motion -match '^OK corpse-articulation-status .*updates=(\d+) maximumTravel=([\d.eE+-]+) minimumClearance=[\d.eE+-]+$') "Actual articulated readback failed: $motion"
        $updates=[int]$Matches[1];$travel=[double]::Parse($Matches[2],$culture)
        Require ($motion -match ('entity='+[regex]::Escape($identity)+' ')) "Moving owner changed: $motion"
        Require ($updates -gt 1 -and $travel -gt .003 -and $travel -le .5) "Bodies did not move within the bounded lease: $motion"
        $consumer=Query 'corpse-compare'
        Require ($consumer -match '^MISMATCH corpse-compare .*pointMismatch=[1-9]\d* .*holdValid=1$') "Solver motion did not reach actual Man point consumers: $consumer"
        $result.gates.articulatedMotion=@{passed=$true;start=$motionStart;end=Simulation-Time;actual=$motion;consumer=$consumer}
        Assert-ArticulatedBackend $true 'movingArticulationHandles'
        $result.captures.articulatedMotion=Screenshot 'articulated-motion'
        if ($FrozenRetirementSmoke) {
            $activeRetire=Query 'corpse-frozen-world-retire'
            Require ($activeRetire -ceq 'REFUSED corpse-frozen-world-retire owned-bounded-fixture-required') 'Active solver admitted fixture retirement.'
            Assert-ArticulatedBackend $true 'activeWorldRetireRefusal'
            $frozen=Freeze-OwnedPose 'explicitFreeze'
            $frozenStart=Simulation-Time
            Set-Acceleration $ExpiryAcceleration
            $until=[DateTime]::UtcNow.AddSeconds(50)
            do {
                Start-Sleep -Milliseconds 500; $frozenNow=Simulation-Time
                if ([DateTime]::UtcNow -gt $until) {throw 'Frozen persistence clock did not progress.'}
            } while ($frozenNow-$frozenStart -le 31)
            Set-Acceleration 0
            $persistent=Pose-Snapshot $true
            Require ((Exact-OwnedPose $frozen) -ceq (Exact-OwnedPose $persistent)) 'Frozen owned pose changed after its original 30 second lease.'
            $persistStatus=Query 'corpse-status'
            Require ($persistStatus -match '^OK corpse-frozen-status .*policy=explicit-owned-pose$') "Frozen pose expired: $persistStatus"
            Assert-ArticulatedBackend $false 'frozenPersistenceHandles'
            $result.gates.frozenPersistence=@{passed=$true;start=$frozenStart;end=$frozenNow;actual=$persistent;status=$persistStatus}
            $result.captures.frozenPersistent=Screenshot 'frozen-persistent'
        }
    }
    $clear = Query 'corpse-clear'
    Require ($clear -ceq 'OK corpse-clear cleared=1') "Explicit clear failed: $clear"
    Require-NoHold 'explicitClear'
    if($ArticulationSmoke) {Assert-ArticulatedBackend $false 'articulatedClearCleanup'}
    $result.captures.cleared = Screenshot 'cleared'
    if ($FinalPoseProbe) {
        Require ($result.captures.before.simulationTime -eq $result.captures.cleared.simulationTime) 'Diagnostic screenshots advanced simulation.'
        Require ($result.captures.before.sha256 -ceq $result.captures.held.sha256 -and
            $result.captures.before.sha256 -ceq $result.captures.cleared.sha256) 'Read-only final-pose/hold query changed paused authored death pixels.'
        $result.gates.finalPosePixels = @{ passed = $true; before = $result.captures.before.sha256;
            held = $result.captures.held.sha256; cleared = $result.captures.cleared.sha256 }
    }
    if ($FrozenRetirementSmoke) {
        # A distinct fresh owner proves the real backend generation transition.
        # The diagnostic itself must establish exact bounded terrain ownership,
        # zero tracked handles and raw terrain + optional owned player proxy.
        $result.observations.worldRetireCapture=Wait-Capture
        Start-Articulation 'worldRetireArticulation'
        $null=Freeze-OwnedPose 'worldRetireFreeze'
        $retired=Query 'corpse-frozen-world-retire'
        Require ($retired -match '^OK corpse-frozen-world-retire epochBefore=(\d+) epochAfter=(\d+) bodies=0 joints=0 rawBodiesBefore=([12]) rawBodiesBeforeRetire=1 proxyRemoved=([01]) fixture=bounded-empty$') "Bounded empty fixture world retirement refused: $retired"
        Require ([long]$Matches[2] -gt [long]$Matches[1] -and [int]$Matches[3] -eq 1+[int]$Matches[4]) 'Actual backend generation or raw proxy proof is invalid.'
        Require-NoHold 'frozenWorldGenerationInvalidation'
        $getter=Send @{cmd='corpse_pose_state'} -AllowRefusal
        Require ($getter.ok -eq $false -and $getter.error -ceq 'no-valid-hold') 'Frozen point getter survived backend generation invalidation.'
        $retiredBackend=Query 'corpse-physics-status'
        Require ($retiredBackend -ceq 'REFUSED corpse-physics-status no-physics-world' -or
            $retiredBackend -match '^OK corpse-physics-status epoch=\d+ bodies=0 articulatedBodies=0 articulatedJoints=0$') 'Old articulated handles survived fixture retirement.'
        $result.gates.frozenWorldGenerationInvalidation.actualRetirement=$retired
        $result.gates.frozenWorldGenerationInvalidation.backendAfter=$retiredBackend
        # The epoch check correctly invalidates and clears the old pose owner.
        # Establish a fresh held owner before preparing its bounded fixture.
        $result.observations.worldReprepareCapture=Wait-Capture
        $prepared=Query 'corpse-prepare'
        Require ($prepared -match '^OK corpse-prepare solver=box3d ') "Bounded fixture recreation failed: $prepared"
        $result.observations.repreparedPhysics=$prepared
    }
    $result.observations.expiryCapture = Wait-Capture
    if($ArticulationSmoke) {Start-Articulation 'expiryArticulation'}
    $expiryStart = Simulation-Time
    Set-Acceleration $ExpiryAcceleration
    $until = [DateTime]::UtcNow.AddSeconds(50)
    do {
        Start-Sleep -Milliseconds 500
        $expiryStatus = Query 'corpse-status'
        $expiryNow = Simulation-Time
        if ($expiryStatus -ceq 'REFUSED corpse-status no-valid-hold') { break }
        Require ($expiryStatus -match '^OK corpse-status ') "Unexpected expiry status: $expiryStatus"
        if ([DateTime]::UtcNow -gt $until) { throw 'Lease did not expire after simulation progressed.' }
    } while ($true)
    Require ($expiryNow - $expiryStart -gt 30) "Hold cleared before its expiry: elapsed=$($expiryNow-$expiryStart), $expiryStatus"
    Set-Acceleration 0
    $result.gates.expiry = @{ passed = $true; start = $expiryStart; end = $expiryNow; actual = $expiryStatus; acceleration = $ExpiryAcceleration }
    Check-FinalPose 'expiry' $false ''
    if($ArticulationSmoke) {Assert-ArticulatedBackend $false 'articulatedExpiryCleanup'}
    $result.observations.hideCapture = Wait-Capture
    if($ArticulationSmoke) {Start-Articulation 'hideArticulation'}
    if($FrozenRetirementSmoke) {$null=Freeze-OwnedPose 'beforeHideFreeze'}
    Check-FinalPose 'beforeHide' $true $identity
    $null = Send @{ cmd = 'exec'; code = 'poseProofHidden=poseProofCorpse; player action ["HIDEBODY",poseProofCorpse]; setAccTime 1' }
    $until = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 250
        $hideStatus = Query 'corpse-status'
        if ($hideStatus -ceq 'REFUSED corpse-status no-valid-hold') { break }
        Require ($hideStatus -match '^OK corpse-(status|frozen-status) ') "Unexpected hide status: $hideStatus"
        if ([DateTime]::UtcNow -gt $until) { throw "HIDEBODY did not invalidate the hold: $hideStatus" }
    } while ($true)
    Set-Acceleration 0
    Require-NoHold 'hideInvalidation'
    if($ArticulationSmoke) {Assert-ArticulatedBackend $false 'articulatedHideCleanup'}
    $hideRefusal = Query 'corpse-capture'
    Require ($hideRefusal -match '^REFUSED corpse-capture .*reason=(hidden-or-hiding|no-nearby-corpse|removed-or-deleting)$') "Hidden fixture capture was not explicitly refused: $hideRefusal"
    $result.gates.hideInvalidation.captureRefusal = $hideRefusal
    $result.captures.hidden = Screenshot 'hide-invalidated'
    # The hide action and its capture refusal are already proved. Isolate the
    # next lifecycle fixture: the old unit remains a world object while hiding,
    # and the action may have moved the player used by createUnit's placement.
    $staging=@{playerBefore=Eval 'getPosASL player';hiddenBefore=Eval 'getPosASL poseProofHidden';
        requestedPlayerPosition=Eval 'poseProofOriginalPlayerASL';requestedPlayerDirection=Eval 'poseProofOriginalPlayerDir';
        requestedLivePosition=Eval 'poseProofFirstLiveASL'}
    $result.observations.deleteFixtureStaging=$staging
    $null = Send @{cmd='exec';code='deleteVehicle poseProofHidden; player setPosASL poseProofOriginalPlayerASL; player setDir poseProofOriginalPlayerDir'}
    $staging.oldFixtureRemoved=Eval 'isNull poseProofHidden'
    Require ($staging.oldFixtureRemoved -eq $true) 'Old hidden fixture survived delete-case isolation.'
    $staging.restoredPlayerPosition=Eval 'getPosASL player'
    $staging.restoredPlayerDirection=Eval 'getDir player'
    Set-Acceleration 1
    Spawn-Stock -ReuseInitialSite
    $staging.actualLivePosition=Eval 'getPosASL poseProofCorpse'
    $staging.playerDistance=Eval 'player distance poseProofCorpse'
    Position-Camera
    $null = Send @{ cmd = 'exec'; code = 'poseProofCorpse setDammage 1' }
    $result.observations.deleteCapture = Wait-Capture
    Assert-Compare 'secondCorpseEquivalence'
    if($ArticulationSmoke) {Start-Articulation 'deleteArticulation'}
    if($FrozenRetirementSmoke) {$null=Freeze-OwnedPose 'beforeDeleteFreeze'}
    $secondIdentity = [regex]::Match($result.observations.deleteCapture,'entity=(\S+)').Groups[1].Value
    Check-FinalPose 'secondCorpseHold' $true $secondIdentity
    $null = Send @{ cmd = 'exec'; code = 'deleteVehicle poseProofCorpse' }
    Require-NoHold 'deleteInvalidation'
    if($ArticulationSmoke) {Assert-ArticulatedBackend $false 'articulatedDeleteCleanup'}
    $null = Send @{ cmd = 'exec'; code = 'deleteVehicle poseProofHidden' }
    Require ((Query 'corpse-clear') -ceq 'OK corpse-clear cleared=0') 'Cleanup found a surviving lease.'
    Require ((Eval 'alive player') -eq $true) 'Runner damaged or deleted the player.'
    Close-Owned
    $result.status = 'passed'
} catch {
    $result.status = 'failed'; $result.error = $_.Exception.Message
    $result.errorSource = @{ line = $_.InvocationInfo.ScriptLineNumber; source = $_.InvocationInfo.Line; stack = $_.ScriptStackTrace }
    throw
} finally {
    if ($p -and !$p.HasExited) {
        try { Close-Owned } catch {}
        if (!$p.HasExited) { $p.Kill(); $null = $p.WaitForExit(5000); $result.forcedOwnedProcessStop = $true }
    }
    if ($client) { $client.Dispose() }
    try {
        $after = Installed-State; $result.after = $after
        $changed = $after.deployedFrom -ne $before.deployedFrom
        for ($i = 0; $i -lt $before.files.Count; ++$i) {
            foreach ($field in @('name','path','sha256','bytes','writtenUtc')) {
                if ($after.files[$i][$field] -ne $before.files[$i][$field]) { $changed = $true }
            }
        }
        if ($changed) { $result.status = 'failed'; $result.error = 'Installed provenance or binaries changed during the campaign.' }
    } catch { $result.status = 'failed'; $result.error = $_.Exception.Message }
    foreach ($key in $keys) {
        if ($null -eq $savedEnv[$key]) { Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process') }
    }
    $result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Corpse pose evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
    Write-Host 'Installed shared affine pose/lifecycle gates passed; screenshots remain available for appearance review.'
}
