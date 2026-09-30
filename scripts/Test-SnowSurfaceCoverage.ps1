# Installed snow material/roof falsifier; invoke through with-game-lock.sh.
[CmdletBinding()]
param(
    [ValidateRange(0.04,0.5)][double]$Depth = 0.18,
    [ValidateRange(1,10)][int]$SettleSeconds = 3,
    [ValidateRange(1,64)][int]$DifferenceThreshold = 8,
    [switch]$PowderOff,
    [switch]$OnlyDeposited,
    [switch]$AutoExposure,
    [switch]$LegacyCwa,
    [switch]$WeatherCoverOff,
    [switch]$InteriorSkyOff,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'stock-surface',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
function Installed-State {
    # Provenance is the first installed file read, before hashing or launch.
    $stamp = [IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    if (!$stamp) { throw 'Installed deployment provenance is empty.' }
    $files = @('OpenPoseidon.exe','wgpu_renderer.dll') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{ name = $_; path = $file.FullName; sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash;
           bytes = $file.Length; writtenUtc = $file.LastWriteTimeUtc.ToString('o') }
    }
    return @{ deployedFrom = $stamp; files = @($files) }
}
function Require([bool]$condition,[string]$message) { if (!$condition) { throw $message } }
function Camera-Pose([double]$X,[double]$Z,[double]$Height,[double]$Azimuth,[double]$Elevation) {
    $values = @($X,$Z,$Height,$Azimuth,$Elevation)
    foreach ($value in $values) {
        Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Camera contains a nonfinite number.'
    }
    return ($values | ForEach-Object { $_.ToString('R',$culture) }) -join ' '
}
function Assert-SnowState($state,[bool]$enabled,[double]$depth,[Nullable[int]]$chunks,[bool]$falling = $false) {
    foreach ($name in @('enabled','falling','geometry','depth','chunks')) {
        Require ($null -ne $state.$name) "Snow response lacks actual $name."
    }
    Require ($state.enabled -is [bool] -and $state.enabled -eq $enabled) 'Actual enabled state differs.'
    Require ($state.falling -is [bool] -and $state.falling -eq $falling) 'Actual falling state differs from fixed surface or disabled reset fixture.'
    Require ($state.geometry -is [bool] -and $state.geometry) 'Snow geometry is disabled.'
    Require (![double]::IsNaN([double]$state.depth) -and ![double]::IsInfinity([double]$state.depth) -and
        [Math]::Abs([double]$state.depth-$depth) -lt 0.00001) 'Actual snow depth differs.'
    Require ([double]$state.chunks -ge 0 -and [double]$state.chunks -eq [Math]::Floor([double]$state.chunks)) 'Invalid snow chunk count.'
    if ($null -ne $chunks) { Require ($state.chunks -eq $chunks) 'Stored tracks changed during disabled or camera-only interval.' }
}
function Decode-Eval([string]$display) {
    # SQF displayed strings have literal backslashes and doubled quotes, not JSON escapes.
    $display = $display.Trim()
    if ($display.StartsWith('"')) {
        Require ($display.Length -ge 2 -and $display.EndsWith('"')) 'Malformed SQF string.'
        $inner = $display.Substring(1,$display.Length-2); $decoded = [Text.StringBuilder]::new()
        for ($i=0; $i -lt $inner.Length; ++$i) {
            if ($inner[$i] -eq '"') {
                Require ($i+1 -lt $inner.Length -and $inner[$i+1] -eq '"') 'Malformed doubled SQF quote.'
                ++$i
            }
            $null = $decoded.Append($inner[$i])
        }
        return $decoded.ToString()
    }
    return ConvertFrom-Json -InputObject $display -NoEnumerate
}
function Assert-ObjectTelemetry([string]$line,[bool]$enabled,[double]$depth) {
    $pattern = 'wgpu object snow: enabled=([01]) deposit=([\d.eE+-]+) snowlineHeight=([\d.eE+-]+) snowlineRange=([\d.eE+-]+) snowlineDepth=([\d.eE+-]+)'
    Require ($line -match $pattern) 'Installed effective object-snow telemetry missing or unknown.'
    $values = @($Matches[1],$Matches[2],$Matches[3],$Matches[4],$Matches[5])
    Require ([int]$values[0] -eq [int]$enabled) 'Installed object-snow switch differs from arm.'
    $deposit = [double]::Parse($values[1],$culture); $height = [double]::Parse($values[2],$culture)
    $range = [double]::Parse($values[3],$culture); $lineDepth = [double]::Parse($values[4],$culture)
    foreach ($value in @($deposit,$height,$range,$lineDepth)) {
        Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite uploaded snow parameters.'
    }
    $expected = if ($enabled) { $depth } else { 0.0 }
    Require ([Math]::Abs($deposit-$expected) -le 0.000051) 'Effective uploaded object deposit differs from selected arm.'
    Require ($height -lt 0 -and $range -eq 0 -and $lineDepth -eq 0) 'Effective object snowline was not disabled.'
    return @{ line = $line; enabled = $enabled; deposit = $deposit; snowlineHeight = $height; snowlineRange = $range; snowlineDepth = $lineDepth }
}
$before = Installed-State
$selectedArms = if ($OnlyDeposited) { @('deposit-object-on') } else { @('dry-object-on','deposit-object-on','deposit-object-off') }
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
foreach ($path in $(if ($LegacyCwa) { @($mission) } else { @($mission,$NativeAddons) })) {
    Require (Test-Path -LiteralPath $path) "Required fixture path missing: $path"
}
$output = Join-Path $root ('build/snow-surface/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Force -Path $output | Out-Null
$result = [ordered]@{ status = 'running'; before = $before; arms = @{}; comparisons = @{}; error = $null;
    scope = 'Capture and lifecycle evidence; difference pixels do not identify snow or accept roof/ceiling/wall/road appearance.' }
$keys = @('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOWLINE',
    'WGR_OBJECT_SNOW','WGR_SNOW_POWDER','WGR_SNOW_SURFACE_FIXTURE','WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_GRASS','POSEIDON_WIND_OVERRIDE',
    'WGR_CLOUD_COVERAGE','WGR_LOD_GOVERNOR_RANGE','WGR_TERRAIN_JITTER','WGR_INTERIOR_SKY','WGR_INTERIOR_SKY_DEBUG',
    'WGR_WEATHER_COVER','WGR_WEATHER_COVER_TRACE',
    'POSEIDON_INTERIOR_SKY_PROBE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_TERRAIN_PUDDLES')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
@{ sourceHead = (& git -C $root rev-parse HEAD); scriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash;
   helperSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')).Hash;
   metricsSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs')).Hash;
   installed = $before; mission = $mission; nativeAddons = $(if ($LegacyCwa) { $null } else { $NativeAddons });
   legacyCwa = [bool]$LegacyCwa; powderOff = [bool]$PowderOff; lockOwner = $env:LOCK_OWNER;
   depth = $Depth; arms = $selectedArms; autoExposure = [bool]$AutoExposure; snowlineOverride = 'off';
   stockRoof = 'CampEastC / stan_eastC.p3d'; roofCentre = @(9486,3006); openCentre = @(9475.25,3018.25);
   fixedDate = @(1985,6,21,16,0); fixedTime = 100; exposure = 1; temporal = 0; grass = 0; cloud = 0;
   wind = '0 90 0'; terrainJitter = 0; differenceThreshold = $DifferenceThreshold;
   roi = 'central 60% diagnostic only; tent geometry is not a semantic snow mask';
   roadFixture = $(if ($LegacyCwa) { 'snow-road.eden authored stock Eden road near/far poses' } else { 'Supplementary native-world views at stock-road poses; roads not yet verified here' });
   effectiveObjectToggle = 'Requires installed feature telemetry and pixel positive controls; environment assignment alone is not proof.' } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
if (!('TerrainPuddleMetrics' -as [type])) {
    $refs = @([Drawing.Bitmap].Assembly.Location,[Drawing.Color].Assembly.Location)
    $refs += @([Drawing.Bitmap].Assembly.GetReferencedAssemblies() | Where-Object Name -like 'System.Private.Windows.*' |
        ForEach-Object { [Reflection.Assembly]::Load($_).Location })
    Add-Type -Path (Join-Path $PSScriptRoot 'TerrainPuddleMetrics.cs') -ReferencedAssemblies $refs
}
. "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
$p = $null; $client = $null; $reader = $null; $writer = $null; $log = $null
function Assert-RunHealth {
    if ($p -and $p.HasExited) { throw "Owned game exited early: $($p.ExitCode)" }
    if ($log -and (Test-Path -LiteralPath $log) -and
        (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|StartAutoTest could not boot' -Quiet)) {
        throw 'Installed runtime or mission failure; reject run.'
    }
}
function Send($command) {
    Assert-RunHealth
    $request = $command | ConvertTo-Json -Compress
    $request | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl'); $writer.WriteLine($request)
    do {
        $line = $reader.ReadLine(); if ($null -eq $line) { throw 'Harness connection closed.' }
        $line | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl'); $reply = $line | ConvertFrom-Json
    } while ($null -eq $reply.ok)
    if (!$reply.ok) { throw $line }; return $reply
}
function Eval([string]$code) {
    $reply = Send @{ cmd = 'eval'; code = $code }
    Require ($null -ne $reply.result) "Evaluator returned no value: $code"
    return Decode-Eval ([string]$reply.result)
}
function Set-FixedScene {
    # Production commands include the actual value in their success strings.
    Require ((Eval 'triSetSimTime 100') -ceq 'OK:100000') 'Deterministic simulation time refused.'
    Require ((Eval 'triSetBrightness 1') -ceq 'OK:1.000') 'Deterministic brightness refused.'
}
function Screenshot([string]$name) {
    $path = Join-Path $armDir ($name+'.png'); $null = Send @{ cmd = 'screenshot'; path = $path }
    $until = [DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) {
        Assert-RunHealth; Require ([DateTime]::UtcNow -lt $until) "Screenshot not written: $path"
        Start-Sleep -Milliseconds 100
    }
    $bytes = [IO.File]::ReadAllBytes($path)
    Require ($bytes.Length -ge 24 -and [BitConverter]::ToString($bytes[0..7]) -eq '89-50-4E-47-0D-0A-1A-0A') "Invalid PNG: $path"
    $sim = Eval 'time'; Require ([Math]::Abs([double]$sim-100) -lt 0.001) 'Paused simulation time changed.'
    return @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path).Hash; bytes = $bytes.Length; simulationTime = $sim }
}
function Capture-Views([string]$stage,$views) {
    $captures = @{}
    foreach ($view in $views.GetEnumerator()) {
        Require ((Eval ('triFreeFlyPose "'+$view.Value+'"')) -ceq 'OK') "Camera refused: $($view.Key)"
        Start-Sleep -Seconds $SettleSeconds
        $capture = Screenshot ($stage+'-'+$view.Key); $capture.pose = $view.Value; $captures[$view.Key] = $capture
    }
    return $captures
}
function Snow-State([bool]$enabled,[double]$depth,[Nullable[int]]$chunks) {
    $state = Send @{ cmd = 'dev_snow'; action = 'state' }; Assert-SnowState $state $enabled $depth $chunks; return $state
}
function Close-Owned {
    if ($p -and !$p.HasExited) {
        $null = Send @{ cmd = 'exec'; code = 'setAccTime 1' }; $null = Send @{ cmd = 'exit' }
        Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game failed normal exit.'
        Require (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) 'Clean shutdown marker missing.'
    }
    if ($client) { $client.Dispose(); $script:client = $null }
    $script:writer = $null; $script:reader = $null
}
function Pair($a,$b) { return [TerrainPuddleMetrics]::Compare($a.path,$b.path,$DifferenceThreshold) }
try {
    if ($LegacyCwa) {
        foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM')) {
            Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue
        }
    } else {
        $env:POSEIDON_REFORGER_WORLD = 'worlds/eden'; $env:POSEIDON_REFORGER_OBJECTS = '1'; $env:POSEIDON_REFORGER_STREAM = '1'
    }
    $env:POSEIDON_SNOWLINE = 'off'; $env:POSEIDON_SNOW_TEST_DEPTH = '0'
    # RATE's mere presence enables falling; omit it to keep actual falling=false.
    foreach ($key in @('POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_INTERIOR_SKY_PROBE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')) {
        Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue
    }
    $env:WGR_TEMPORAL = '0'; $env:WGR_AUTO_EXPOSURE = if ($AutoExposure) { '1' } else { '0' }; $env:WGR_EXPOSURE = '1'
    $env:WGR_GRASS = '0'; $env:POSEIDON_WIND_OVERRIDE = '0 90 0'; $env:WGR_CLOUD_COVERAGE = '0'
    $env:WGR_LOD_GOVERNOR_RANGE = '1'; $env:WGR_TERRAIN_JITTER = '0'; $env:WGR_INTERIOR_SKY = $(if ($InteriorSkyOff) {'0'} else {'1'})
    $env:WGR_WEATHER_COVER = $(if ($WeatherCoverOff) {'0'} else {'1'}); $env:WGR_WEATHER_COVER_TRACE = '1'
    $env:WGR_INTERIOR_SKY_DEBUG = '0'; $env:WGR_TERRAIN_PUDDLES = '0'
    $env:WGR_SNOW_POWDER = if ($PowderOff) { '0' } else { '1' }
    $env:WGR_SNOW_SURFACE_FIXTURE = '1'
    foreach ($arm in $selectedArms) {
        Require (!(Get-Process OpenPoseidon -ErrorAction SilentlyContinue)) 'Unexpected game process between arms.'
        $armDir = Join-Path $output $arm; $profile = Join-Path $armDir 'user'
        New-Item -ItemType Directory -Force -Path $profile | Out-Null
        $log = Join-Path $armDir 'engine.log'; $env:POSEIDON_USER_DIR = $profile
        $env:WGR_OBJECT_SNOW = if ($arm -eq 'deposit-object-off') { '0' } else { '1' }
        [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
        $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
        $arguments = @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",
            '--test-mission',('"'+$mission+'"'),'--test-world-hour','16',
            '--test-world-freefly','9486','3006','240','0','-70','--log-file',('"'+$log+'"'))
        if (!$LegacyCwa) { $arguments += @('--test-world',('"'+$NativeAddons+'"')) }
        $stdout = Join-Path $armDir 'stdout.txt'; $stderr = Join-Path $armDir 'stderr.txt'
        $p = Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $result.arms[$arm] = @{ status = 'starting'; pid = $p.Id; log = $log; arguments = $arguments }
        $null = $p.Handle; $until = [DateTime]::UtcNow.AddSeconds(120)
        do {
            Assert-RunHealth; $client = [Net.Sockets.TcpClient]::new()
            try { $client.Connect('127.0.0.1',$port) } catch { $client.Dispose(); $client = $null }
            if (!$client) { Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable after 120 seconds.'; Start-Sleep -Milliseconds 250 }
        } while (!$client)
        $stream = $client.GetStream(); $stream.ReadTimeout = 30000
        $reader = [IO.StreamReader]::new($stream); $writer = [IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush = $true
        Start-Sleep -Seconds 12; Require ((Eval 'triSceneReady') -ceq 'OK') 'Mission scene not ready.'
        $null = Send @{ cmd = 'exec'; code = 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0; setDate [1985,6,21,16,0]' }
        Set-FixedScene
        $null = Send @{ cmd = 'dev_snow'; action = 'disable' }; $initial = Snow-State $false 0 0
        $fixture = New-TerrainPuddleStockRoof -SendCommand { param($request) Send $request } -X 9486 -Z 3006 -ControlX 9475.25 -ControlZ 3018.25
        $g = [double]$fixture.centreGroundHeight
        $views = [ordered]@{
            roofTop = (Camera-Pose 9486 2998 ($g+12) 0 -45)
            roofFar = (Camera-Pose 9486 2974 ($g+28) 0 -35)
            roofLong = (Camera-Pose 9486 2806 ($g+90) 0 -23.8)
            roofDistant = (Camera-Pose 9486 2506 ($g+130) 0 -14.4)
            underside = (Camera-Pose 9486 3004.5 ($g+1.0) 0 70)
            wall = (Camera-Pose 9494 3006 ($g+1.8) 270 0)
            coveredGround = $fixture.coveredCamera
            exposedGround = $fixture.exposedCamera
            terrainGrain = (Camera-Pose 9475.25 3016.75 ([double]$fixture.exposedGroundHeight+0.4) 0 -55)
            roadNear = '5083.67 3988.25 17.63 145.4 -18.5'
            roadFar = '5020 4080 55 145.4 -18.5'
        }
        # Dynamic UAZ exercises the ordinary stock object path, separately from retained tents.
        $vehicleGround = Get-TerrainPuddleFixtureHeight { param($request) Send $request } 9465.25 3018.25
        $null = Send @{ cmd = 'exec'; code = 'snowSurfaceCar="UAZ" createVehicle [9465.25,3018.25,0]; snowSurfaceCar setDir 0; snowSurfaceCar setPos [9465.25,3018.25,0]; snowSurfaceCar allowDamage false; snowSurfaceCar engineOn false' }
        Require ((Eval 'typeOf snowSurfaceCar') -ceq 'UAZ') 'Stock vehicle did not spawn.'
        $carPosition = Eval 'getPosASL snowSurfaceCar'
        Require ($carPosition -is [array] -and $carPosition.Count -eq 3) 'Stock vehicle ASL missing.'
        # Stock createVehicle may seat away from the request before pausing.
        # Camera and recorded provenance use its actual position, not the request.
        Require ([Math]::Abs([double]$carPosition[0]-9465.25) -lt 1.5 -and [Math]::Abs([double]$carPosition[1]-3018.25) -lt 1.5) 'Vehicle placement differs.'
        $views.vehicle = Camera-Pose ([double]$carPosition[0]+4) ([double]$carPosition[1]-5) ([double]$carPosition[2]+4) 321 -27
        $captures = @{}; $result.arms[$arm].captures = $captures; $result.arms[$arm].fixture = $fixture
        $captures.disabledInitial = Capture-Views 'disabled-initial' $views
        $expectedDepth = if ($arm -eq 'dry-object-on') { 0.0 } else { $Depth }
        $null = Send @{ cmd = 'dev_snow'; action = 'enable' }
        if ($expectedDepth -gt 0) { $null = Send @{ cmd = 'dev_snow'; action = 'deposit'; metres = $Depth } }
        $enabled = Snow-State $true $expectedDepth $null
        $captures.enabled = Capture-Views 'enabled' $views
        $telemetry = @(Select-String -LiteralPath $log -Pattern 'wgpu object snow: enabled=' | ForEach-Object { $_.Line })
        Require ($telemetry.Count -gt 0) 'No installed effective object snow parameter upload telemetry.'
        $effectiveToggle = Assert-ObjectTelemetry $telemetry[-1] ($arm -ne 'deposit-object-off') $expectedDepth
        # This sweep travels kilometres to the road and returns to the exact roof pose.
        $returnViews = [ordered]@{ roofTop = $views.roofTop; roofLong = $views.roofLong; roofDistant = $views.roofDistant; coveredGround = $views.coveredGround; terrainGrain = $views.terrainGrain }
        $captures.return = Capture-Views 'return' $returnViews
        $returned = Snow-State $true $expectedDepth ([int]$enabled.chunks)
        # Independent positive shelter control at the identical ground pixels:
        # delete actual geometry, then re-create the same verified placement.
        Remove-TerrainPuddleStockRoof -SendCommand { param($request) Send $request }
        $captures.roofRemoved = Capture-Views 'roof-removed' ([ordered]@{ coveredGround = $views.coveredGround })
        $replacedFixture = New-TerrainPuddleStockRoof -SendCommand { param($request) Send $request } -X 9486 -Z 3006 -ControlX 9475.25 -ControlZ 3018.25
        for ($coordinate=0; $coordinate -lt 3; ++$coordinate) {
            Require ([Math]::Abs([double]$replacedFixture.actualPosition[$coordinate]-[double]$fixture.actualPosition[$coordinate]) -lt 0.001) 'Restored roof placement changed.'
        }
        $captures.roofReplaced = Capture-Views 'roof-replaced' ([ordered]@{ coveredGround = $views.coveredGround })
        $null = Send @{ cmd = 'dev_snow'; action = 'disable' }; $disabled = Snow-State $false $expectedDepth ([int]$enabled.chunks)
        $captures.disabledAfter = Capture-Views 'disabled-after' $views
        $null = Send @{ cmd = 'dev_snow'; action = 'enable' }; $restored = Snow-State $true $expectedDepth ([int]$enabled.chunks)
        $captures.restored = Capture-Views 'restored' $returnViews
        # Real stock infantry motion creates persistent terrain depressions, never synthetic stamps.
        $tracks = @{ status = 'dry-arm-no-track-gate' }
        if ($expectedDepth -gt 0) {
            $null = Send @{ cmd = 'exec'; code = '"SoldierWB" createUnit [[9475.25,3018.25,0],group player,"snowSurfaceWalker=this; removeAllWeapons this; this allowDamage false"]; snowSurfaceWalker setPos [9475.25,3018.25,0]; snowSurfaceWalker setDir 0; snowSurfaceWalker setUnitPos "DOWN"; snowSurfaceWalker setBehaviour "CARELESS"; snowSurfaceWalker doMove [9475.25,3024.25,0]' }
            Require ((Eval 'typeOf snowSurfaceWalker') -ceq 'SoldierWB' -and (Eval 'alive snowSurfaceWalker') -eq $true -and
                (Eval 'snowSurfaceWalker == player') -eq $false) 'Stock track actor identity invalid.'
            $null = Send @{ cmd = 'exec'; code = 'setAccTime 1' }
            $deadline = [DateTime]::UtcNow.AddSeconds(30)
            do {
                Start-Sleep -Milliseconds 500; $walkerPosition = Eval 'getPosASL snowSurfaceWalker'
                Require ($walkerPosition -is [array] -and $walkerPosition.Count -eq 3) 'Actual stock walker position missing.'
                if ([double]$walkerPosition[1] -gt 3020.25) { break }
                Require ([DateTime]::UtcNow -lt $deadline) 'Stock walker did not move along track fixture.'
            } while ($true)
            $null = Send @{ cmd = 'exec'; code = 'doStop snowSurfaceWalker; setAccTime 0' }
            Require ((Eval 'triSetSimTime 100') -ceq 'OK:100000') 'Track freeze refused.'
            $trackState = Snow-State $true $expectedDepth $null
            Require ($trackState.chunks -gt $enabled.chunks) 'Real stock motion created no snow track chunks.'
            # The unit origin need not coincide with a stamped contact cell. Seek
            # an actual positive sample in the bounded trailing body footprint.
            $samples = @()
            foreach ($dx in @(-0.35,0,0.35)) {
                foreach ($dz in @(-1,-0.5,0)) {
                    $sx = [double]$walkerPosition[0]+$dx; $sz = [double]$walkerPosition[1]+$dz
                    $probe = Send @{ cmd = 'dev_snow'; action = 'sample'; x = $sx; z = $sz }
                    Require ($null -ne $probe.deficit -and [double]$probe.deficit -ge 0 -and
                        ![double]::IsNaN([double]$probe.deficit) -and ![double]::IsInfinity([double]$probe.deficit)) 'Invalid actual track sample.'
                    $samples += @{ x = $sx; z = $sz; deficit = [double]$probe.deficit }
                }
            }
            $sample = $samples | Sort-Object deficit -Descending | Select-Object -First 1
            Require ($sample.deficit -gt 0) 'No actual positive contact deficit near the moved stock actor.'
            $null = Send @{ cmd = 'exec'; code = 'snowSurfaceWalker setPos [9450,2990,0]' }
            $captures.track = Capture-Views 'track' ([ordered]@{ terrainGrain = $views.terrainGrain })
            $null = Send @{ cmd = 'dev_snow'; action = 'disable' }; $trackDisabled = Snow-State $false $expectedDepth ([int]$trackState.chunks)
            $captures.trackDisabled = Capture-Views 'track-disabled' ([ordered]@{ terrainGrain = $views.terrainGrain })
            $null = Send @{ cmd = 'dev_snow'; action = 'enable' }; $trackRestored = Snow-State $true $expectedDepth ([int]$trackState.chunks)
            $captures.trackRestored = Capture-Views 'track-restored' ([ordered]@{ terrainGrain = $views.terrainGrain })
            $again = Send @{ cmd = 'dev_snow'; action = 'sample'; x = $sample.x; z = $sample.z }
            Require ($null -ne $sample.deficit -and $sample.deficit -eq $again.deficit) 'Sampled track changed across disabled interval.'
            $tracks = @{ status = 'stored-track-lifecycle-verified-pixels-require-inspection'; position = $walkerPosition;
                state = $trackState; disabled = $trackDisabled; restored = $trackRestored; samples = $samples; sample = $sample; resample = $again }
            $null = Send @{ cmd = 'exec'; code = 'deleteVehicle snowSurfaceWalker' }
        }
        $weather = Send @{ cmd = 'weather_visibility' }
        Require ($weather.rain -le 0.001 -and $weather.fog -le 0.001) 'Actual weather invalidates captures.'
        # Reset settings disables cover but must not erase actual stored depth
        # or track chunks. Default falling=true is inert while disabled.
        $beforeReset = Send @{ cmd = 'dev_snow'; action = 'state' }
        $reset = Send @{ cmd = 'dev_snow'; action = 'reset-settings' }
        Assert-SnowState $reset $false $expectedDepth ([int]$beforeReset.chunks) $true
        Require ($null -ne $reset.rate -and $null -ne $reset.maxDepth -and $null -ne $reset.flakes -and
            [Math]::Abs([double]$reset.rate-0.01) -lt 0.00001 -and $reset.maxDepth -eq 0.5 -and $reset.flakes -eq 1) 'Reset settings missed documented snow defaults.'
        $captures.resetDisabled = Capture-Views 'reset-disabled' $returnViews
        $telemetry = @(Select-String -LiteralPath $log -Pattern '(?i)object.*snow|snow.*object' | ForEach-Object { $_.Line })
        $result.arms[$arm] = @{ status = 'captured'; pid = $p.Id; arguments = $arguments; objectSnowRequested = $env:WGR_OBJECT_SNOW; featureTelemetry = $telemetry;
            powderRequested = $env:WGR_SNOW_POWDER;
            effectiveToggle = $effectiveToggle; fixture = $fixture; carPosition = $carPosition; carGround = $vehicleGround;
            vehicleScope = 'Exploratory: dynamic/animated/skinned mesh admission may exclude UAZ; positive pixels alone do not establish owner admission.';
            views = $views; captures = $captures; initial = $initial; enabled = $enabled; returned = $returned;
            disabled = $disabled; restored = $restored; tracks = $tracks; actualWeather = $weather; log = $log; stdout = $stdout; stderr = $stderr }
        $result.arms[$arm].beforeReset = $beforeReset; $result.arms[$arm].resetDisabled = $reset
        $result.arms[$arm].replacedFixture = $replacedFixture
        $result.comparisons[$arm+'-roof-shelter'] = @{ removedPresent = Pair $captures.roofRemoved.coveredGround $captures.enabled.coveredGround;
            replacedPresent = Pair $captures.roofReplaced.coveredGround $captures.enabled.coveredGround }
        foreach ($view in $returnViews.Keys) {
            $result.comparisons[$arm+'-'+$view] = @{ cameraReturn = Pair $captures.enabled[$view] $captures.return[$view];
                restore = Pair $captures.enabled[$view] $captures.restored[$view] }
        }
        foreach ($view in $views.Keys) {
            $result.comparisons[$arm+'-disabled-'+$view] = Pair $captures.disabledInitial[$view] $captures.disabledAfter[$view]
        }
        Remove-TerrainPuddleStockRoof -SendCommand { param($request) Send $request }
        $null = Send @{ cmd = 'exec'; code = 'deleteVehicle snowSurfaceCar' }; Require ((Eval 'isNull snowSurfaceCar') -eq $true) 'Vehicle cleanup failed.'
        Close-Owned
    }
    if (!$OnlyDeposited) {
    foreach ($view in $result.arms['dry-object-on'].views.Keys) {
        $dry = $result.arms['dry-object-on'].captures.enabled[$view]
        $on = $result.arms['deposit-object-on'].captures.enabled[$view]; $off = $result.arms['deposit-object-off'].captures.enabled[$view]
        $result.comparisons[$view] = @{ depositDry = Pair $dry $on; objectOnOff = Pair $on $off }
    }
    $result.comparisons.roofRemovedGround = @{
        depositDry = Pair $result.arms['dry-object-on'].captures.roofRemoved.coveredGround $result.arms['deposit-object-on'].captures.roofRemoved.coveredGround
        objectOnOff = Pair $result.arms['deposit-object-on'].captures.roofRemoved.coveredGround $result.arms['deposit-object-off'].captures.roofRemoved.coveredGround
    }
    }
    $result.status = 'captured-not-visually-accepted'
} catch { $result.status = 'failed'; $result.error = $_.Exception.Message; throw }
finally {
    if ($p -and !$p.HasExited) {
        try { Close-Owned } catch {}
        if (!$p.HasExited) { $p.Kill(); $null = $p.WaitForExit(5000); $result.forcedOwnedProcessStop = $true }
    }
    if ($client) { $client.Dispose() }
    try {
        $after = Installed-State; $result.after = $after; $changed = $after.deployedFrom -ne $before.deployedFrom
        for ($i=0; $i -lt $before.files.Count; ++$i) {
            foreach ($field in @('name','path','sha256','bytes','writtenUtc')) {
                if ($after.files[$i][$field] -ne $before.files[$i][$field]) { $changed = $true }
            }
        }
        if ($changed) { $result.status = 'failed'; $result.error = 'Installed provenance or binaries changed during campaign.' }
    } catch { $result.status = 'failed'; $result.error = $_.Exception.Message }
    foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process') }
    $result | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Snow surface evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
    Write-Host 'Captures complete; inspect actual roof, ceiling, wall, road and track controls before accepting appearance.'
}
