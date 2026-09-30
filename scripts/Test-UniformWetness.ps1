# Real simulation clothing lifecycle, run only through with-game-lock.sh.
[CmdletBinding()]
param(
    [ValidateRange(1,4)][double]$Acceleration = 4,
    [ValidateRange(30,180)][int]$WetDeadlineSeconds = 90,
    [ValidateRange(120,240)][double]$DrySimulationSeconds = 120,
    [ValidateRange(1,8)][int]$SettleSeconds = 2,
    [switch]$LegacyCwa,
    [switch]$WetOff,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label = 'stock-uniform',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$NativeAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture = [Globalization.CultureInfo]::InvariantCulture
$root = Split-Path -Parent $PSScriptRoot
function Require([bool]$condition,[string]$message) { if (!$condition) { throw $message } }
function Installed-State {
    # First installed file read: preserve the stamped EXE/DLL provenance.
    $stamp = [IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim()
    Require ([bool]$stamp) 'Installed deployment provenance is empty.'
    $files = @('OpenPoseidon.exe','wgpu_renderer.dll') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{ name = $_; path = $file.FullName; sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash;
           bytes = $file.Length; writtenUtc = $file.LastWriteTimeUtc.ToString('o') }
    }
    return @{ deployedFrom = $stamp; files = @($files) }
}
function Decode-Eval([string]$display) {
    $display = $display.Trim()
    if ($display.StartsWith('"')) {
        Require ($display.Length -ge 2 -and $display.EndsWith('"')) 'Malformed SQF string.'
        $inner = $display.Substring(1,$display.Length-2); $decoded = [Text.StringBuilder]::new()
        for ($i=0; $i -lt $inner.Length; ++$i) {
            if ($inner[$i] -eq '"') {
                Require ($i+1 -lt $inner.Length -and $inner[$i+1] -eq '"') 'Malformed doubled SQF quote.'; ++$i
            }
            $null = $decoded.Append($inner[$i])
        }
        return $decoded.ToString()
    }
    return ConvertFrom-Json -InputObject $display -NoEnumerate
}
function Normalized-Wetness($value) {
    Require ($value -is [ValueType] -and $value -isnot [bool]) 'Uniform query did not return a numeric scalar.'
    $number = [double]$value
    Require (![double]::IsNaN($number) -and ![double]::IsInfinity($number) -and $number -ge 0 -and $number -le 1) 'Actual uniform wetness is invalid or not normalized.'
    return $number
}
function Assert-Weather($weather,[double]$minimum,[double]$maximum) {
    Require ($null -ne $weather.rain -and $null -ne $weather.fog) 'Actual weather response is missing.'
    $rain = [double]$weather.rain; $fog = [double]$weather.fog
    Require (![double]::IsNaN($rain) -and ![double]::IsInfinity($rain) -and $rain -ge $minimum -and $rain -le $maximum) 'Actual rain differs from the requested lifecycle arm.'
    Require (![double]::IsNaN($fog) -and ![double]::IsInfinity($fog) -and $fog -ge 0 -and $fog -le 0.001) 'Actual fog invalidates matched clothing views.'
}
function Assert-Stationary($actual,$expected,[double]$heading,[string]$pose) {
    Require ($actual -is [array] -and $actual.Count -eq 3 -and $expected -is [array] -and $expected.Count -eq 3) 'Actor actual ASL position is missing.'
    for ($i=0; $i -lt 3; ++$i) {
        $number = [double]$actual[$i]
        Require (![double]::IsNaN($number) -and ![double]::IsInfinity($number) -and [Math]::Abs($number-[double]$expected[$i]) -le 0.02) 'Actor moved during the clothing fixture.'
    }
    Require (![double]::IsNaN($heading) -and ![double]::IsInfinity($heading) -and [Math]::Abs($heading-180) -lt 0.01) 'Actor heading changed.'
    Require ($pose -ceq 'civil') 'Actor left the authored static pose.'
}
function Parse-ClothTrace([string]$line) {
    $pattern='UNIFORM_CLOTH model=(.+?) texture=(\S+) kind=(\d+) enabled=(true|false|[01]) wet=([\d.eE+-]+) encoded=([\d.eE+-]+)'
    Require ($line -match $pattern) 'Unknown installed cloth material trace grammar.'
    $model=$Matches[1].Replace('/','\').ToLowerInvariant(); $texture=$Matches[2].Replace('/','\').ToLowerInvariant()
    $kind=[int]$Matches[3]; $enabled=$Matches[4] -in @('true','1')
    $wet=Normalized-Wetness ([double]::Parse($Matches[5],$culture)); $encoded=[double]::Parse($Matches[6],$culture)
    Require (![double]::IsNaN($encoded) -and ![double]::IsInfinity($encoded)) 'Nonfinite cloth encoded material parameter.'
    return @{line=$line;model=$model;texture=$texture;kind=$kind;enabled=$enabled;wet=$wet;encoded=$encoded}
}
function Assert-WbCloth($row,[bool]$enabled,[double]$actualWet) {
    Require ($row.model -ceq 'data3d\mc vojakw2.p3d' -and $row.texture -ceq 'merged\00007mc_vojakw2.paa' -and $row.kind -eq 2) 'Trace does not admit the actual stock WB atlas and material mask.'
    # Producer emits at decile transitions, not every scalar change. The exact
    # paused scalar comes from triUniformWetness; this row proves actual upload
    # in its current band, including four-decimal trace rounding.
    Require ($row.enabled -eq $enabled -and $row.wet -ge 0.2 -and
        [Math]::Floor($row.wet*10) -eq [Math]::Floor($actualWet*10) -and
        $row.wet -le $actualWet+0.00015) 'Cloth trace differs from the paused wetness band or render switch.'
    $expected=if ($enabled -and $actualWet -gt 0) { 2+$row.wet } else { 0 }
    Require ([Math]::Abs($row.encoded-$expected) -le 0.00015) 'Actual encoded stock WB material differs from cloth-only ON/OFF contract.'
    return $row
}
function Camera-Pose($position,[double]$distance = 3,[double]$height = 1.5) {
    Require ($position -is [array] -and $position.Count -eq 3) 'Camera owner has no actual ASL.'
    $values = @([double]$position[0],([double]$position[1]-$distance),([double]$position[2]+$height),0,-6)
    foreach ($value in $values) { Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite actor camera.' }
    return ($values | ForEach-Object { $_.ToString('R',$culture) }) -join ' '
}
$before = Installed-State
$mission = Join-Path $root 'tests/perf/missions/perf_field.eden'
foreach ($path in $(if ($LegacyCwa) { @($mission) } else { @($mission,$NativeAddons) })) { Require (Test-Path -LiteralPath $path) "Required fixture path missing: $path" }
$output = Join-Path $root ('build/uniform-wetness/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$profile = Join-Path $output 'user'; New-Item -ItemType Directory -Force -Path $profile | Out-Null
$log = Join-Path $output 'engine.log'
$result = [ordered]@{ status = 'running'; before = $before; samples = @(); captures = @{}; gates = @{}; error = $null;
    scope = 'Real Person wetting/drying/pause/shelter state and descriptive stock clothing captures; no pixel quality or cloth segmentation acceptance.' }
$keys = @('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM',
    'POSEIDON_TEST_RAIN','POSEIDON_SNOWLINE','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES',
    'POSEIDON_UNIFORM_WET_TRACE','WGR_UNIFORM_WET','WGR_OBJECT_SNOW','WGR_GRASS','WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE',
    'POSEIDON_WIND_OVERRIDE','WGR_CLOUD_COVERAGE','WGR_TERRAIN_JITTER','WGR_LOD_GOVERNOR_RANGE',
    'WGR_TERRAIN_PUDDLES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_INTERIOR_SKY_DEBUG','POSEIDON_INTERIOR_SKY_PROBE')
$savedEnv = @{}
foreach ($key in $keys) { $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
@{ sourceHead = (& git -C $root rev-parse HEAD); scriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash;
   helperSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')).Hash;
   installed = $before; mission = $mission; nativeAddons = $(if ($LegacyCwa) { $null } else { $NativeAddons }); legacyCwa = [bool]$LegacyCwa;
   wetOff = [bool]$WetOff; renderOverride = $(if ($WetOff) { 'WGR_UNIFORM_WET=0' } else { 'WGR_UNIFORM_WET=1' });
   lockOwner = $env:LOCK_OWNER; realRain = 'SQF weather plus weather_visibility actual GLandscape density; no override';
   acceleration = $Acceleration; minimumWetSimSeconds = 20; wetThreshold = 0.2; drySimSeconds = $DrySimulationSeconds;
   stockClass = 'SoldierWB'; pose = 'civil'; heading = 180; openCentre = @(9475.25,3018.25); roofCentre = @(9486,3006);
   fixedDate = @(1985,6,21,16,0); exposure = 1; grass = 0; temporal = 0; cloud = 0; wind = '0 90 0'; terrainJitter = 0;
   snowline = 'off'; objectSnow = 0; query = 'triUniformWetness <object>'; trace = 'POSEIDON_UNIFORM_WET_TRACE=1';
   captureLimits = 'Static authored state/world transform verified; live affine palette equivalence and cloth ROI require independent material evidence.' } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
. "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
$p = $null; $client = $null; $reader = $null; $writer = $null
function Assert-RunHealth {
    if ($p -and $p.HasExited) { throw "Owned game exited early: $($p.ExitCode)" }
    if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|StartAutoTest could not boot' -Quiet)) {
        throw 'Installed runtime or mission failure; reject run.'
    }
}
function Send($command) {
    Assert-RunHealth; $request = $command | ConvertTo-Json -Compress
    $request | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl'); $writer.WriteLine($request)
    $until = [DateTime]::UtcNow.AddSeconds(30)
    do {
        Require ([DateTime]::UtcNow -lt $until) 'Harness response deadline exceeded.'
        $line = $reader.ReadLine(); Require ($null -ne $line) 'Harness connection closed.'
        $line | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl'); $reply = $line | ConvertFrom-Json
    } while ($null -eq $reply.ok)
    if (!$reply.ok) { throw $line }; return $reply
}
function Eval([string]$code) {
    $reply = Send @{ cmd = 'eval'; code = $code }; Require ($null -ne $reply.result) "Evaluator missing result: $code"
    return Decode-Eval ([string]$reply.result)
}
function Set-Acceleration([double]$value) { $null = Send @{ cmd = 'exec'; code = ('setAccTime '+$value.ToString('R',$culture)) } }
function Sample([string]$stage) {
    $time = [double](Eval 'time'); Require (![double]::IsNaN($time) -and ![double]::IsInfinity($time)) 'Invalid simulation time.'
    $sample = @{ stage = $stage; time = $time; weather = (Send @{ cmd = 'weather_visibility' }); actors = @{} }
    foreach ($entry in $actors.GetEnumerator()) {
        $name = $entry.Value.variable
        Require ((Eval ('alive '+$name)) -eq $true) 'Fixture soldier died.'
        $position = Eval ('getPosASL '+$name); $heading = [double](Eval ('getDir '+$name)); $pose = Eval ('getMove '+$name)
        Assert-Stationary $position $entry.Value.position $heading $pose
        $sample.actors[$entry.Key] = @{ wetness = (Normalized-Wetness (Eval ('triUniformWetness '+$name))); position = $position; heading = $heading; pose = $pose }
    }
    $result.samples += $sample; return $sample
}
function Capture([string]$stage) {
    $captures = @{}
    foreach ($entry in $actors.GetEnumerator()) {
        Require ((Eval ('triFreeFlyPose "'+$entry.Value.camera+'"')) -ceq 'OK') 'Actual-actor camera refused.'
        Start-Sleep -Seconds $SettleSeconds
        $path = Join-Path $output ($stage+'-'+$entry.Key+'.png'); $null = Send @{ cmd = 'screenshot'; path = $path }
        $until = [DateTime]::UtcNow.AddSeconds(10)
        while (!(Test-Path -LiteralPath $path)) { Assert-RunHealth; Require ([DateTime]::UtcNow -lt $until) 'Screenshot not written.'; Start-Sleep -Milliseconds 100 }
        $bytes = [IO.File]::ReadAllBytes($path)
        Require ($bytes.Length -ge 24 -and [BitConverter]::ToString($bytes[0..7]) -eq '89-50-4E-47-0D-0A-1A-0A') 'Invalid PNG.'
        $captures[$entry.Key] = @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path).Hash; bytes = $bytes.Length; camera = $entry.Value.camera }
    }
    $result.captures[$stage] = $captures; return $captures
}
function Assert-Paused($a,$b) {
    Require ([Math]::Abs($a.time-$b.time) -lt 0.001) 'Simulation advanced while paused.'
    Require ($a.weather.rain -eq $b.weather.rain) 'Actual rain changed while paused.'
    foreach ($name in $a.actors.Keys) { Require ($a.actors[$name].wetness -eq $b.actors[$name].wetness) 'Uniform wetness advanced while paused.' }
}
function Close-Owned {
    if ($p -and !$p.HasExited) {
        Set-Acceleration 1; $null = Send @{ cmd = 'exit' }
        Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game failed normal exit.'
        Require (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) 'Clean shutdown marker missing.'
    }
    if ($client) { $client.Dispose(); $script:client = $null }; $script:reader = $null; $script:writer = $null
}
try {
    $env:POSEIDON_USER_DIR = $profile
    if ($LegacyCwa) {
        foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM')) { Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue }
    } else { $env:POSEIDON_REFORGER_WORLD='worlds/eden'; $env:POSEIDON_REFORGER_OBJECTS='1'; $env:POSEIDON_REFORGER_STREAM='1' }
    foreach ($key in @('POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','POSEIDON_INTERIOR_SKY_PROBE')) {
        Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue
    }
    $env:POSEIDON_SNOWLINE='off'; $env:POSEIDON_UNIFORM_WET_TRACE='1'; $env:WGR_OBJECT_SNOW='0'; $env:WGR_GRASS='0'
    $env:WGR_UNIFORM_WET = if ($WetOff) { '0' } else { '1' }
    $env:WGR_TEMPORAL='0'; $env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE='1'; $env:POSEIDON_WIND_OVERRIDE='0 90 0'
    $env:WGR_CLOUD_COVERAGE='0'; $env:WGR_TERRAIN_JITTER='0'; $env:WGR_LOD_GOVERNOR_RANGE='1'; $env:WGR_TERRAIN_PUDDLES='0'; $env:WGR_INTERIOR_SKY_DEBUG='0'
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0); $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $arguments = @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),
        '--test-world-hour','16','--test-world-freefly','9475.25','3018.25','230','0','-50','--log-file',('"'+$log+'"'))
    if (!$LegacyCwa) { $arguments += @('--test-world',('"'+$NativeAddons+'"')) }; $result.arguments = $arguments
    $p = Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments
    $null = $p.Handle; $result.pid = $p.Id; $until = [DateTime]::UtcNow.AddSeconds(120)
    do {
        Assert-RunHealth; $client = [Net.Sockets.TcpClient]::new()
        try { $client.Connect('127.0.0.1',$port) } catch { $client.Dispose(); $client=$null }
        if (!$client) { Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable after 120 seconds.'; Start-Sleep -Milliseconds 250 }
    } while (!$client)
    $stream=$client.GetStream(); $stream.ReadTimeout=30000; $reader=[IO.StreamReader]::new($stream)
    $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush=$true
    Start-Sleep -Seconds 12; Require ((Eval 'triSceneReady') -ceq 'OK') 'Mission scene not ready.'
    $null = Send @{ cmd='exec'; code='player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setDate [1985,6,21,16,0]; setAccTime 0' }
    Require ((Eval 'triSetBrightness 1') -ceq 'OK:1.000') 'Fixed brightness refused.'
    $fixture = New-TerrainPuddleStockRoof -SendCommand { param($request) Send $request } -X 9486 -Z 3006 -ControlX 9475.25 -ControlZ 3018.25
    $result.fixture=$fixture
    Require ((Eval 'triUniformWetness objNull') -eq -1) 'Invalid-object uniform query did not return -1.'
    Require ((Eval 'triUniformWetness puddleStockRoof') -eq -1) 'Non-person uniform query did not return -1.'
    $actors = [ordered]@{ exposed = @{ variable='uniformOpen'; x=9475.25; z=3018.25 }; covered = @{ variable='uniformCovered'; x=9486; z=3006 } }
    foreach ($entry in $actors.GetEnumerator()) {
        $actor=$entry.Value; $xz=$actor.x.ToString('R',$culture)+','+$actor.z.ToString('R',$culture); $name=$actor.variable
        $code='"SoldierWB" createUnit [['+$xz+',0],group player,"'+$name+'=this; removeAllWeapons this; this allowDamage false"]; '+$name+' setPos ['+$xz+',0]; '+$name+' setDir 180; '+$name+' disableAI "MOVE"; '+$name+' disableAI "TARGET"; '+$name+' setUnitPos "UP"; doStop '+$name+'; '+$name+' switchMove "civil"'
        $null=Send @{cmd='exec';code=$code}
        Require ((Eval ('typeOf '+$name)) -ceq 'SoldierWB' -and (Eval ($name+' == player')) -eq $false) 'Actual stock actor identity differs.'
    }
    # Weather and pose commands take effect through actual simulation, not render frames.
    Set-Acceleration 1; Start-Sleep -Seconds 3; Set-Acceleration 0
    foreach ($entry in $actors.GetEnumerator()) {
        $entry.Value.position=Eval ('getPosASL '+$entry.Value.variable)
        $entry.Value.objectDebugName=(Send @{cmd='eval';code=$entry.Value.variable}).result
        # Allow the stock actor's initial collision/ground seating. Subsequent
        # samples still require the actual settled transform to stay within 2cm.
        Require ([Math]::Abs([double]$entry.Value.position[0]-$entry.Value.x) -lt 1.5 -and
            [Math]::Abs([double]$entry.Value.position[1]-$entry.Value.z) -lt 1.5) 'Actor actual horizontal placement differs.'
        $entry.Value.camera=Camera-Pose $entry.Value.position $(if ($entry.Key -eq 'covered') { 1.5 } else { 3 })
    }
    $result.actors=$actors; $dry=Sample 'dry'; Assert-Weather $dry.weather 0 0.02
    Require ($dry.actors.exposed.wetness -le 0.001 -and $dry.actors.covered.wetness -le 0.001) 'New dry actors started wet.'
    $null=Capture 'dry'; $result.gates.dry=$dry
    $null=Send @{cmd='exec';code='0 setOvercast 1; 0 setRain 1'}; Set-Acceleration $Acceleration
    $wetStart=$dry.time; $deadline=[DateTime]::UtcNow.AddSeconds($WetDeadlineSeconds)
    do {
        Start-Sleep -Milliseconds 500; $sample=Sample 'wetting'
        if ($sample.time-$wetStart -ge 20 -and $sample.weather.rain -ge 0.3 -and $sample.actors.exposed.wetness -gt 0.2) { break }
        Require ([DateTime]::UtcNow -lt $deadline) 'Real rain did not produce exposed uniform wetness above 0.2 after 20 simulation seconds.'
    } while ($true)
    Set-Acceleration 0; $wet=Sample 'wet-paused'; Assert-Weather $wet.weather 0.3 1
    Require ($wet.time-$wetStart -ge 20 -and $wet.actors.exposed.wetness -gt 0.2) 'Paused wet state no longer satisfies the real-rain positive control.'
    Require ($wet.actors.covered.wetness -le 0.001) 'Stock roof did not shelter the actual covered soldier from wetting.'
    $null=Capture 'wet'; $pauseA=Sample 'pause-a'; Start-Sleep -Seconds 3; $null=Capture 'wet-return'; $pauseB=Sample 'pause-b'
    Assert-Paused $wet $pauseA; Assert-Paused $pauseA $pauseB; $result.gates.wet=$wet; $result.gates.pause=@{a=$pauseA;b=$pauseB}
    $null=Send @{cmd='exec';code='0 setOvercast 0; 0 setRain 0'}; Set-Acceleration $Acceleration
    $stopDeadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 500; $dryStart=Sample 'rain-stopping'
        if ($dryStart.weather.rain -le 0.02) { break }
        Require ([DateTime]::UtcNow -lt $stopDeadline) 'Actual rain did not stop.'
    } while ($true)
    $dryDeadline=[DateTime]::UtcNow.AddSeconds($DrySimulationSeconds/$Acceleration+45)
    do {
        Start-Sleep -Milliseconds 500; $drying=Sample 'drying'; Assert-Weather $drying.weather 0 0.02
        if ($drying.time-$dryStart.time -ge $DrySimulationSeconds) { break }
        Require ([DateTime]::UtcNow -lt $dryDeadline) 'Simulation did not complete the bounded natural drying interval.'
    } while ($true)
    Set-Acceleration 0; $dried=Sample 'dried-paused'; Assert-Weather $dried.weather 0 0.02
    Require ($dried.actors.exposed.wetness -lt $wet.actors.exposed.wetness-0.02 -and $dried.actors.exposed.wetness -gt 0) 'Actual dry weather did not gradually reduce stored uniform wetness.'
    Require ($dried.actors.covered.wetness -le 0.001) 'Covered dry control became wet.'
    $null=Capture 'drying'; $result.gates.drying=@{start=$dryStart;end=$dried}
    $trace=@(Select-String -LiteralPath $log -Pattern 'UNIFORM_WET|UNIFORM_CLOTH' | ForEach-Object { $_.Line })
    Require (($trace | Where-Object { $_ -match 'UNIFORM_WET' }).Count -gt 0) 'Installed actual uniform simulation trace missing.'
    Require (($trace | Where-Object { $_ -match '\bUNIFORM_CLOTH ' }).Count -gt 0) 'Installed stock cloth material admission trace missing.'
    $cloth=@($trace | Where-Object { $_ -match '\bUNIFORM_CLOTH ' } | ForEach-Object { Parse-ClothTrace $_ })
    $wetRows=@($cloth | Where-Object { $_.model -ceq 'data3d\mc vojakw2.p3d' -and $_.texture -ceq 'merged\00007mc_vojakw2.paa' -and
        $_.kind -eq 2 -and $_.wet -ge 0.2 -and [Math]::Floor($_.wet*10) -eq [Math]::Floor($wet.actors.exposed.wetness*10) -and
        $_.wet -le $wet.actors.exposed.wetness+0.00015 })
    Require ($wetRows.Count -gt 0) 'No actual stock WB cloth admission in paused wetness band; raw wetness alone does not prove material upload.'
    $result.gates.material=Assert-WbCloth $wetRows[-1] (!$WetOff) $wet.actors.exposed.wetness
    $result.trace=$trace; $result.clothRows=$cloth
    $null=Send @{cmd='exec';code='deleteVehicle uniformOpen; deleteVehicle uniformCovered'}
    Require ((Eval 'isNull uniformOpen') -eq $true -and (Eval 'isNull uniformCovered') -eq $true) 'Actor cleanup failed.'
    Remove-TerrainPuddleStockRoof -SendCommand { param($request) Send $request }; Close-Owned
    $result.status='state-verified-captures-not-visually-accepted'
} catch { $result.status='failed'; $result.error=$_.Exception.Message; throw }
finally {
    if ($p -and !$p.HasExited) {
        try { Close-Owned } catch {}
        if (!$p.HasExited) { $p.Kill(); $null=$p.WaitForExit(5000); $result.forcedOwnedProcessStop=$true }
    }
    if ($client) { $client.Dispose() }
    try {
        $after=Installed-State; $result.after=$after; $changed=$after.deployedFrom -ne $before.deployedFrom
        for ($i=0; $i -lt $before.files.Count; ++$i) { foreach ($field in @('name','path','sha256','bytes','writtenUtc')) { if ($after.files[$i][$field] -ne $before.files[$i][$field]) { $changed=$true } } }
        if ($changed) { $result.status='failed'; $result.error='Installed provenance or binaries changed during campaign.' }
    } catch { $result.status='failed'; $result.error=$_.Exception.Message }
    foreach ($key in $keys) { [Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process') }
    $result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Uniform wetness evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
    Write-Host 'Actual wetting/drying/pause/shelter state verified; clothing appearance needs matched material inspection.'
}
