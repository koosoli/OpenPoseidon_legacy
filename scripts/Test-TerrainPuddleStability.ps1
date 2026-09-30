[CmdletBinding()]
param(
    [string]$Camera = '9481.25 2994.25 209.72 0 -18',
    [ValidateRange(0,30)][double]$CameraGroundClearance = 0,
    [ValidateRange(0,50)][double]$BoundaryGroundClearance = 0,
    [ValidateRange(0,30)][double]$NegativeGroundClearance = 0,
    [string]$NegativeCamera = '2756.25 2369.25 10.44 0 -18',
    [string]$BoundaryCamera = '9481.25 3006.25 249.72 0 -85',
    [string]$RoofCamera = '',
    [string]$RoofExposedControlCamera = '',
    [string]$RoofFixtureEvidence = '',
    [ValidateRange(1,15)][int]$SettleSeconds = 3,
    [ValidateRange(1,64)][int]$MaskThreshold = 8,
    [ValidateRange(0.5,1)][double]$MinimumReturnIoU = 0.95,
    [switch]$RequireNumericGates,
    [switch]$LegacyCwa,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'native-stability',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
function Camera-Numbers([string]$value) {
    $tokens = @($value.Trim() -split '\s+')
    if ($tokens.Count -ne 5) { throw 'Camera must contain X Z absolute-height azimuth elevation.' }
    foreach ($token in $tokens) {
        $number = 0.0
        if (![double]::TryParse($token, [Globalization.NumberStyles]::Float, $culture, [ref]$number) -or
            [double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw "Invalid camera number: $token" }
    }
    return $tokens
}
$numbers = @(Camera-Numbers $Camera)
$negative = @(Camera-Numbers $NegativeCamera)
$boundary = @(Camera-Numbers $BoundaryCamera)
$roofEnabled = !!$RoofCamera
if ($roofEnabled -and (!$RoofExposedControlCamera -or !$RoofFixtureEvidence -or !(Test-Path -LiteralPath $RoofFixtureEvidence))) {
    throw 'Roof acceptance requires an exposed eligible-dirt control camera and an existing fixture evidence document.'
}
if (!$roofEnabled -and ($RoofExposedControlCamera -or $RoofFixtureEvidence)) { throw 'Roof parameters must be supplied together.' }
if ($roofEnabled) { $roof = @(Camera-Numbers $RoofCamera); $roofControl = @(Camera-Numbers $RoofExposedControlCamera) }
$root = Split-Path -Parent $PSScriptRoot
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
foreach ($path in $(if ($LegacyCwa) { @($mission) } else { @($mission, $NativeAddons) })) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required native fixture path missing: $path" }
}
function Installed-State {
    # Provenance is the first installed file read, before hashes or launch.
    $stamp = [IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    if (!$stamp) { throw 'Installed deployment provenance is empty.' }
    $files = @('OpenPoseidon.exe', 'wgpu_renderer.dll') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{ name = $_; path = $file.FullName; sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash;
           bytes = $file.Length; writtenUtc = $file.LastWriteTimeUtc.ToString('o') }
    }
    return @{ deployedFrom = $stamp; files = @($files) }
}
$before = Installed-State
$output = Join-Path $root ('build/terrain-puddles/' + $Label + '-' + (Get-Date -Format yyyyMMdd-HHmmss) + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6))
New-Item -ItemType Directory -Force -Path $output | Out-Null
$log = Join-Path $output 'engine.log'
$result = [ordered]@{ status = 'running'; scope = 'controlled installed capture and numeric camera-return diagnostics; no appearance acceptance';
    before = $before; arms = @{}; comparisons = @{}; gates = @{}; error = $null;
    boundary = @{ status = 'candidate-only'; camera = $BoundaryCamera; reason = 'Native harness has no source-material query; semantic boundary needs independent confirmation.' };
    roof = @{ status = 'unavailable'; reason = 'No proven roof-over-eligible-dirt fixture supplied.' } }
$keys = @('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLES',
    'WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_INTERIOR_SKY','WGR_INTERIOR_SKY_DEBUG',
    'WGR_GRASS','POSEIDON_WIND_OVERRIDE','WGR_CLOUD_COVERAGE','WGR_LOD_GOVERNOR_RANGE','WGR_TERRAIN_JITTER')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
@{ scriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash;
   metricsSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs')).Hash;
   sourceHead = (& git -C $root rev-parse HEAD); installed = $before; mission = $mission;
   nativeAddons = $(if ($LegacyCwa) { $null } else { $NativeAddons }); legacyCwa = [bool]$LegacyCwa;
   camera = $Camera; cameraGroundClearance = $CameraGroundClearance; boundaryGroundClearance = $BoundaryGroundClearance;
   negativeCamera = $NegativeCamera; negativeGroundClearance = $NegativeGroundClearance; boundaryCamera = $BoundaryCamera;
   roofCamera = $RoofCamera; roofControl = $RoofExposedControlCamera; roofEvidence = $RoofFixtureEvidence;
   lockOwner = $env:LOCK_OWNER; fixedSimTime = 100; fixedDate = @(1985,6,21,16,0); fixedExposure = 1; roi = 'central 60% of width and height';
   maskThreshold = $MaskThreshold; minimumReturnIoU = $MinimumReturnIoU; requireNumericGates = [bool]$RequireNumericGates;
   wetnessOverrides = @(0,1); actualRain = 0; temporal = 0;
   grass = 0; windOverride = '0 90 0'; cloudCoverage = 0; fixedLodRange = 1; terrainJitter = 0;
   isolation = 'Terrain response controls exclude grass and pin wind/clouds; ordinary grass-on appearance is assessed separately.' } |
   ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
if (!('TerrainPuddleMetrics' -as [type])) {
    $drawingRefs = @([Drawing.Bitmap].Assembly.Location,[Drawing.Color].Assembly.Location)
    # .NET 10 split Bitmap's native interfaces into these assemblies. Earlier
    # runtimes have no such references; derive them rather than pinning a version.
    $drawingRefs += @([Drawing.Bitmap].Assembly.GetReferencedAssemblies() |
        Where-Object Name -like 'System.Private.Windows.*' | ForEach-Object { [Reflection.Assembly]::Load($_).Location })
    Add-Type -Path (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs') -ReferencedAssemblies $drawingRefs
}
$p = $null; $client = $null; $writer = $null; $reader = $null
function Assert-RunHealth {
    if ($p -and $p.HasExited) { throw "Owned game exited early: $($p.ExitCode)" }
    if ((Test-Path -LiteralPath $log) -and
        (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|StartAutoTest could not boot' -Quiet)) {
        throw 'Installed runtime or mission failure; reject this run.'
    }
}
function Send($command) {
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
    if (!$reply.ok) { throw $line }
    return $reply
}
function Simulation-Time {
    $reply = Send @{ cmd = 'eval'; code = 'time' }
    $value = 0.0
    if (![double]::TryParse(([string]$reply.result).Trim('"'), [Globalization.NumberStyles]::Float, $culture, [ref]$value) -or
        [double]::IsNaN($value) -or [double]::IsInfinity($value)) { throw 'Harness returned invalid simulation time.' }
    return $value
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
    if ($bytes.Length -lt 24 -or [BitConverter]::ToString($bytes[0..7]) -ne '89-50-4E-47-0D-0A-1A-0A') { throw "Invalid PNG capture: $path" }
    return @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path).Hash; bytes = $bytes.Length }
}
function Pose-Capture([string]$name, [string[]]$tokens) {
    $reply = Send @{ cmd = 'eval'; code = ('triFreeFlyPose "' + ($tokens -join ' ') + '"') }
    if ($reply.result -notmatch 'OK') { throw "Camera refused: $name" }
    Start-Sleep -Seconds $SettleSeconds
    $capture = Screenshot $name
    $capture.pose = ($tokens -join ' '); $capture.simulationTime = Simulation-Time
    if ([Math]::Abs($capture.simulationTime - 100) -gt 0.001) { throw 'Paused simulation time changed.' }
    return $capture
}
function Uploaded-Wetness([double]$expected) {
    $rows = @(Select-String -LiteralPath $log -Pattern 'Wgpu terrain puddle weather: time=([\d.eE+-]+) rain=([\d.eE+-]+) accumulated=([\d.eE+-]+) uploaded=([\d.eE+-]+)')
    if (!$rows.Count) { throw 'No installed terrain wetness telemetry.' }
    $sample = $rows[-1]; $groups = $sample.Matches[0].Groups
    $uploaded = [double]::Parse($groups[4].Value,$culture)
    $time = [double]::Parse($groups[1].Value,$culture)
    if ([Math]::Abs($time-100) -gt 0.001 -or [Math]::Abs($uploaded-$expected) -gt 0.0001) {
        throw 'Installed terrain uploaded wetness or frozen time differs from the selected diagnostic arm.'
    }
    return @{ time = $time; uploaded = $uploaded; line = $sample.Line; lineNumber = $sample.LineNumber }
}
function Shift-Camera([string[]]$tokens, [double]$dx, [double]$dz, [double]$dh) {
    $copy = [string[]]$tokens.Clone()
    $copy[0] = ([double]::Parse($copy[0],$culture)+$dx).ToString('R',$culture)
    $copy[1] = ([double]::Parse($copy[1],$culture)+$dz).ToString('R',$culture)
    $copy[2] = ([double]::Parse($copy[2],$culture)+$dh).ToString('R',$culture)
    return $copy
}
function Close-Owned {
    if ($p -and !$p.HasExited) {
        $null = Send @{ cmd = 'exec'; code = 'setAccTime 1' }
        $null = Send @{ cmd = 'exit' }
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) { throw 'Owned game failed to exit cleanly.' }
        if (!(Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)) { throw 'Clean shutdown marker missing.' }
    }
    if ($client) { $client.Dispose(); $script:client = $null }
    $script:writer = $null; $script:reader = $null
}
function Pair-Metrics($a,$b) { return [TerrainPuddleMetrics]::Compare($a.path,$b.path,$MaskThreshold) }
try {
    if ($LegacyCwa) {
        foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM')) {
            Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue
        }
    } else {
        $env:POSEIDON_REFORGER_WORLD = 'worlds/eden'; $env:POSEIDON_REFORGER_OBJECTS = '1'; $env:POSEIDON_REFORGER_STREAM = '1'
    }
    Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue
    $env:WGR_TERRAIN_PUDDLE_FIXTURE = '1'; $env:WGR_TEMPORAL = '0'
    $env:WGR_AUTO_EXPOSURE = '0'; $env:WGR_EXPOSURE = '1'; $env:WGR_INTERIOR_SKY = '1'
    # Phase-integrated grass/wind retains pre-pause history across process starts.
    # Exclude grass from terrain-only pixel gates rather than treating vegetation
    # changes as puddle response or relaxing the independent master-off control.
    $env:WGR_GRASS = '0'; $env:POSEIDON_WIND_OVERRIDE = '0 90 0'
    $env:WGR_CLOUD_COVERAGE = '0'; $env:WGR_LOD_GOVERNOR_RANGE = '1'
    # Geography initializes terrain UV offsets from wall-clock-seeded GRandGen.
    # Native detail-noise still consumes tile_uv, so fixed time cannot reproduce
    # its phase across processes. Keep detail, but remove only UV displacement.
    $env:WGR_TERRAIN_JITTER = '0'
    $armNames = @('dry','wet','master-off')
    if ($roofEnabled) { $armNames += 'raw-sky' }
    foreach ($arm in $armNames) {
        if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Unexpected game process between arms.' }
        $armDir = Join-Path $output $arm; $armProfile = Join-Path $armDir 'user'
        New-Item -ItemType Directory -Force -Path $armProfile | Out-Null
        $log = Join-Path $armDir 'engine.log'
        $env:POSEIDON_USER_DIR = $armProfile
        $env:WGR_TERRAIN_PUDDLE_WETNESS = if ($arm -eq 'dry') { '0' } else { '1' }
        $env:WGR_TERRAIN_PUDDLES = if ($arm -eq 'master-off') { '0' } else { '1' }
        $env:WGR_INTERIOR_SKY_DEBUG = if ($arm -eq 'raw-sky') { '1' } else { '0' }
        [IO.File]::WriteAllText((Join-Path $armProfile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
        $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
        $arguments = @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000',
            '--harness',"$port",'--test-mission',('"'+$mission+'"'))
        if (!$LegacyCwa) { $arguments += @('--test-world',('"'+$NativeAddons+'"')) }
        $arguments += @('--test-world-hour','16','--test-world-freefly') + $numbers + @('--log-file',('"'+$log+'"'))
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
        $ready = Send @{ cmd = 'eval'; code = 'triSceneReady' }
        if ($ready.result -notmatch 'OK') { throw 'Mission scene not ready.' }
        $null = Send @{ cmd = 'exec'; code = 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0; setDate [1985,6,21,16,0]' }
        foreach ($expression in @('triSetSimTime 100','triSetBrightness 1')) {
            $reply = Send @{ cmd = 'eval'; code = $expression }
            if ($reply.result -notmatch 'OK') { throw "Deterministic setup refused: $expression" }
        }
        $perf = Send @{ cmd = 'eval'; code = 'triPerfStats 0' }
        $result.arms[$arm] = @{ perfStats = $perf.result }
        if ($CameraGroundClearance -gt 0 -or $BoundaryGroundClearance -gt 0 -or $NegativeGroundClearance -gt 0) {
            . "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
            foreach ($spec in @(@{tokens=$numbers; clearance=$CameraGroundClearance},
                                 @{tokens=$boundary; clearance=$BoundaryGroundClearance},
                                 @{tokens=$negative; clearance=$NegativeGroundClearance})) {
                if ($spec.clearance -le 0) { continue }
                $ground = Get-TerrainPuddleFixtureHeight -SendCommand { param($request) Send $request } -X ([double]::Parse($spec.tokens[0],$culture)) -Z ([double]::Parse($spec.tokens[1],$culture))
                $spec.tokens[2] = ($ground+$spec.clearance).ToString('R',$culture)
            }
        }
        $captures = @{}
        if ($arm -ne 'raw-sky') {
            $captures.dirt = Pose-Capture ($arm+'-dirt') $numbers
            $captures.dirtStatic = Pose-Capture ($arm+'-dirt-static') $numbers
            $captures.moved = Pose-Capture ($arm+'-moved') (Shift-Camera $numbers 80 0 35)
            $captures.dirtReturn = Pose-Capture ($arm+'-dirt-return') $numbers
            $captures.boundary = Pose-Capture ($arm+'-boundary') $boundary
            $captures.boundaryShift = Pose-Capture ($arm+'-boundary-shift') (Shift-Camera $boundary 25 0 0)
            $captures.boundaryReturn = Pose-Capture ($arm+'-boundary-return') $boundary
            $captures.negative = Pose-Capture ($arm+'-beachgrass') $negative
            $captures.negativeStatic = Pose-Capture ($arm+'-beachgrass-static') $negative
            $captures.negativeMoved = Pose-Capture ($arm+'-beachgrass-moved') (Shift-Camera $negative 35 0 25)
            $captures.negativeReturn = Pose-Capture ($arm+'-beachgrass-return') $negative
        }
        if ($roofEnabled) {
            $captures.roof = Pose-Capture ($arm+'-roof') $roof
            $captures.roofControl = Pose-Capture ($arm+'-roof-exposed-control') $roofControl
        }
        $weather = Send @{ cmd = 'weather_visibility' }
        if ($weather.rain -gt 0.001 -or $weather.fog -gt 0.001) { throw 'Actual rain/fog invalidates fixed dry/wet image arms.' }
        $expectedWetness = if ($arm -eq 'dry' -or $arm -eq 'master-off') { 0.0 } else { 1.0 }
        $uploaded = Uploaded-Wetness $expectedWetness
        if (!(Select-String -LiteralPath $log -Pattern 'Wgpu: interior sky gate: enabled=true' -Quiet)) {
            throw 'Interior sky geometry map was not enabled in the installed renderer.'
        }
        $result.arms[$arm] = @{ pid = $p.Id; captures = $captures; actualWeather = $weather; log = $log;
            wetnessOverride = $env:WGR_TERRAIN_PUDDLE_WETNESS; master = $env:WGR_TERRAIN_PUDDLES; uploadedWetness = $uploaded;
            perfStats = $perf.result }
        Close-Owned
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) { throw 'Runtime failure invalidates captures.' }
    }
    $dry = $result.arms.dry.captures; $wet = $result.arms.wet.captures; $off = $result.arms['master-off'].captures
    foreach ($view in @('dirt','dirtStatic','dirtReturn','boundary','boundaryShift','boundaryReturn','negative','negativeStatic','negativeReturn')) {
        $result.comparisons[$view] = @{ wetDry = Pair-Metrics $dry[$view] $wet[$view]; masterOffDry = Pair-Metrics $dry[$view] $off[$view] }
    }
    $staticNoise = [Math]::Max((Pair-Metrics $wet.dirt $wet.dirtStatic).MeanAbsChannel,
        (Pair-Metrics $dry.dirt $dry.dirtStatic).MeanAbsChannel)
    foreach ($view in @('dirt','boundary','negative')) {
        $returnName = $view+'Return'
        $result.comparisons[$view].returnImage = Pair-Metrics $wet[$view] $wet[$returnName]
        $result.comparisons[$view].returnMask = [TerrainPuddleMetrics]::MaskOverlap($dry[$view].path,$wet[$view].path,
            $dry[$returnName].path,$wet[$returnName].path,$MaskThreshold)
    }
    $controlMax = ($result.comparisons.Values | ForEach-Object { $_.masterOffDry.MeanAbsChannel } | Measure-Object -Maximum).Maximum
    $noiseLimit = $staticNoise + 0.75
    $result.gates.staticNoise = @{ value = $staticNoise; maximum = 0.75; passed = ($staticNoise -le 0.75) }
    $result.gates.masterOff = @{ value = $controlMax; maximum = $noiseLimit; passed = ($controlMax -le $noiseLimit) }
    $fraction = $result.comparisons.dirt.wetDry.ChangedFraction
    $result.gates.dirtResponse = @{ fraction = $fraction; minimum = 0.001; passed = ($fraction -ge 0.001) }
    foreach ($view in @('dirt','boundary')) {
        $mask = $result.comparisons[$view].returnMask
        $image = $result.comparisons[$view].returnImage
        $result.gates[$view+'Return'] = @{ mask = $mask; minimumIoU = $MinimumReturnIoU; meanAbsChannel = $image.MeanAbsChannel;
            maximumMean = $noiseLimit; passed = ($mask.NonEmpty -and $mask.IoU -ge $MinimumReturnIoU -and $image.MeanAbsChannel -le $noiseLimit) }
    }
    $negativeNoise = [Math]::Max((Pair-Metrics $dry.negative $dry.negativeStatic).MeanAbsChannel,
        (Pair-Metrics $wet.negative $wet.negativeStatic).MeanAbsChannel)
    $negativeMaxMean = (@('negative','negativeStatic','negativeReturn') | ForEach-Object {
        $result.comparisons[$_].wetDry.MeanAbsChannel } | Measure-Object -Maximum).Maximum
    $negativeMaxFraction = (@('negative','negativeStatic','negativeReturn') | ForEach-Object {
        $result.comparisons[$_].wetDry.ChangedFraction } | Measure-Object -Maximum).Maximum
    $result.gates.negative = @{ maximumMeanAbsChannel = $negativeMaxMean;
        maximum = ($negativeNoise+0.75); maximumChangedFraction = $negativeMaxFraction;
        returnMeanAbsChannel = $result.comparisons.negative.returnImage.MeanAbsChannel;
        passed = ($negativeMaxMean -le $negativeNoise+0.75 -and $negativeMaxFraction -le 0.005 -and
            $result.comparisons.negative.returnImage.MeanAbsChannel -le $negativeNoise+0.75) }
    if ($roofEnabled) {
        $result.roof = @{ status = 'captured-not-accepted'; fixtureEvidence = $RoofFixtureEvidence;
            fixtureEvidenceSha256 = (Get-FileHash -LiteralPath $RoofFixtureEvidence).Hash;
            roofWetDry = Pair-Metrics $dry.roof $wet.roof; exposedWetDry = Pair-Metrics $dry.roofControl $wet.roofControl;
            diffuseSkyDebugRoofBrightness = [TerrainPuddleMetrics]::Brightness($result.arms['raw-sky'].captures.roof.path);
            reason = 'Diffuse greyscale debug includes non-terrain geometry and display tonemapping; it is not the vertical rain query. Verify exposed terrain pixel ROIs before roof pixel acceptance.' }
    }
    $failed = @($result.gates.Keys | Where-Object { !$result.gates[$_].passed })
    $result.numericStatus = if ($failed.Count) { 'inconclusive-or-failed' } else { 'passed-controlled-gates' }
    $result.failedGates = $failed; $result.status = 'captured'
} catch {
    $result.status = 'failed'; $result.error = $_.Exception.Message
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
    Write-Host "Puddle stability evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
    Write-Host "Numeric status: $($result.numericStatus). Boundary semantic identity and real-roof pixel acceptance remain separate."
    if ($RequireNumericGates -and $result.failedGates.Count) { throw "Controlled numeric gates did not pass: $($result.failedGates -join ', '). Captures are retained for diagnosis." }
}
