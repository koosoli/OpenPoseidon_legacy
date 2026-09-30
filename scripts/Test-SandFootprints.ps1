<#
.SYNOPSIS
Locked installed Nogova test of real dry boot-produced sand geometry.
.DESCRIPTION
No direct stamp, deposit or wetness setter. An ordinary leader AI walk must
produce both negative physical terrain height and positive raised rims. Fixed
paused CPU grids prove support/gradient parity and camera-return retention.
PNG captures are descriptive and need root visual review, not pixel isolation.
#>
[CmdletBinding()]
param(
    [ValidateRange(1,8)][int]$SettleSeconds=2,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='nogova-dry-sand',
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$condition,[string]$message) { if (!$condition) { throw $message } }
function Finite-SandNumber($value) {
    Require ($value -is [ValueType] -and $value -isnot [bool]) 'Sand field is not numeric.'
    $number=[double]$value
    Require (![double]::IsNaN($number) -and ![double]::IsInfinity($number)) 'Sand field is nonfinite.'
    return $number
}
function Assert-SandState($sample) {
    Require ($sample.enabled -is [bool] -and $sample.enabled) 'Sand is not enabled.'
    $wet=Finite-SandNumber $sample.wetness
    Require ($wet -ge 0 -and $wet -le 1) 'Sand wetness is outside [0,1].'
    foreach ($field in @('chunks','revision','rejected')) {
        $value=Finite-SandNumber $sample.$field
        Require ($value -ge 0 -and [Math]::Floor($value) -eq $value -and $value -le 9007199254740991) 'Sand counter is invalid.'
    }
}
function Assert-SandSample($sample) {
    Assert-SandState $sample
    foreach ($field in @('x','z','offset','sandDx','sandDz','surfaceY','surfaceDx','surfaceDz')) { $null=Finite-SandNumber $sample.$field }
    Require ($sample.offset -ge -.15001 -and $sample.offset -le .020001) 'Sand offset exceeds the signed -15cm/+2cm sand contract.'
    Require ($sample.sourceEligible -is [bool]) 'Actual source admission is missing.'
    Require ($sample.world -is [string] -and $sample.texture -is [string]) 'Actual terrain source identity is missing.'
}
function Assert-SandGeometry($before,$after) {
    Assert-SandSample $before; Assert-SandSample $after
    Require ($before.x -eq $after.x -and $before.z -eq $after.z) 'Physical samples are not at identical coordinates.'
    Require ($before.world -ceq $after.world -and $before.texture -ceq $after.texture) 'Actual source changed between geometry queries.'
    $depthDelta=[double]$after.offset-[double]$before.offset
    Require ([Math]::Abs(($after.surfaceY-$before.surfaceY)-$depthDelta) -le .00006) 'Physical terrain support height does not include the signed sand offset.'
    foreach ($axis in @('Dx','Dz')) {
        $physical='surface'+$axis; $field='sand'+$axis
        Require ([Math]::Abs(($after.$physical-$before.$physical)-($after.$field-$before.$field)) -le .0001) 'Physical terrain gradient does not include the sand gradient.'
    }
    return @{x=$after.x;z=$after.z;depthDelta=$depthDelta;
        supportDelta=($after.surfaceY-$before.surfaceY); dxDelta=($after.surfaceDx-$before.surfaceDx); dzDelta=($after.surfaceDz-$before.surfaceDz)}
}
function Assert-SandPaused($a,$b) {
    $null=Finite-SandNumber $a.time;$null=Finite-SandNumber $b.time
    Assert-SandState $a.sand;Assert-SandState $b.sand
    Require ($a.time -eq $b.time) 'Simulation advanced during paused sand/camera return.'
    foreach ($field in @('enabled','wetness','chunks','revision','rejected')) { Require ($a.sand.$field -eq $b.sand.$field) 'Sand state changed while paused.' }
    foreach ($field in @('rain','particleDensity','particleSnowflakes','liquidRain')) { Require ($a.weather.$field -eq $b.weather.$field) 'Rain authority changed while paused.' }
    Require ($a.actor.Count -eq 3 -and $b.actor.Count -eq 3) 'Actor ASL is missing.'
    for ($i=0;$i -lt 3;++$i) { Require ($a.actor[$i] -eq $b.actor[$i]) 'Actor moved during paused camera return.' }
    Require ($a.pose -ceq $b.pose -and $a.heading -eq $b.heading) 'Actor pose or heading changed during paused camera return.'
    Require ($a.uniformWetness -eq $b.uniformWetness) 'Uniform wetness changed during paused camera return.'
}
function Parse-SandStep([string]$line) {
    Require ($line -match 'SAND_STEP x=([\d.eE+-]+) z=([\d.eE+-]+) surface=(\S+) files=(\S+) sound=(\S+) texture=(\S+) wet=([\d.eE+-]+) depth=([\d.eE+-]+) rim=([\d.eE+-]+) revision=(\d+) chunks=(\d+)') 'Actual sand boot contact trace grammar is missing.'
    $row=@{x=[double]::Parse($Matches[1],$culture);z=[double]::Parse($Matches[2],$culture);surface=$Matches[3];files=$Matches[4];sound=$Matches[5];texture=$Matches[6];wet=[double]::Parse($Matches[7],$culture);depth=[double]::Parse($Matches[8],$culture);rim=[double]::Parse($Matches[9],$culture);revision=[double]::Parse($Matches[10],$culture);chunks=[double]::Parse($Matches[11],$culture)}
    foreach ($field in @('x','z','wet','depth','rim','revision','chunks')) { $null=Finite-SandNumber $row[$field] }
    Require ($row.wet -ge 0 -and $row.wet -le 1 -and $row.depth -gt 0 -and $row.depth -le .15001 -and $row.rim -gt 0 -and $row.rim -le .020001 -and $row.chunks -gt 0 -and $row.revision -gt 0) 'Contact trace has no bounded actual signed sand geometry.'
    return $row
}
function Assert-SandDry($weather) {
    foreach ($field in @('rain','particleDensity','liquidRain')) {
        $value=Finite-SandNumber $weather.$field
        Require ($value -ge 0 -and $value -le .001) 'Actual sand fixture is not dry.'
    }
    Require ($weather.particleSnowflakes -is [bool] -and !$weather.particleSnowflakes) 'Snow particle override is still active.'
}
function Assert-SandEvidence($geometry) {
    Require ($geometry.Count -gt 0) 'Geometry evidence is empty.'
    foreach ($row in $geometry) { foreach ($field in @('x','z','depthDelta','supportDelta','dxDelta','dzDelta')) { $null=Finite-SandNumber $row.$field } }
    $negative=@($geometry | Where-Object {$_.depthDelta -le -.03})
    $positive=@($geometry | Where-Object {$_.depthDelta -gt .001})
    Require ($negative.Count -gt 0) 'Real dry walking did not lower physical terrain by at least 3cm.'
    Require ($positive.Count -gt 0) 'Real dry walking produced no positive physical rim above 1mm.'
    $negativeGradient=@($negative | Where-Object {[Math]::Abs($_.dxDelta)+[Math]::Abs($_.dzDelta) -gt .001})
    $positiveGradient=@($positive | Where-Object {[Math]::Abs($_.dxDelta)+[Math]::Abs($_.dzDelta) -gt .001})
    Require ($negativeGradient.Count -gt 0 -and $positiveGradient.Count -gt 0) 'Depression and rim must both change physical terrain gradients.'
    return @{negativeCount=$negative.Count;positiveCount=$positive.Count;minimumOffset=($negative | Measure-Object depthDelta -Minimum).Minimum;
        maximumOffset=($positive | Measure-Object depthDelta -Maximum).Maximum;negativeGradientCount=$negativeGradient.Count;positiveGradientCount=$positiveGradient.Count}
}
function Assert-SandWalkPosition($position) {
    Require ($position -is [array] -and $position.Count -eq 3) 'Walking position is missing.'
    foreach ($coordinate in $position) { $null=Finite-SandNumber $coordinate }
    Require ($position[0] -ge 2674 -and $position[0] -le 2679.4 -and $position[1] -ge 5125 -and $position[1] -le 5131.6) 'Actual walking path escaped the audited two-dimensional region.'
}
function Assert-SandPair($before,$after) {
    Require ([bool]$before.deployedFrom -and $before.deployedFrom -ceq $after.deployedFrom -and $before.files.Count -eq 2 -and $after.files.Count -eq 2) 'Installed pair stamp changed.'
    for ($i=0;$i -lt 2;++$i) {
        Require ($before.files[$i].name -ceq @('OpenPoseidon.exe','wgpu_renderer.dll')[$i] -and $before.files[$i].sha256 -match '^[A-Fa-f0-9]{64}$' -and $before.files[$i].bytes -gt 0) 'Installed pair identity or hash is missing.'
        foreach ($field in @('name','path','sha256','bytes','writtenUtc')) { Require ($before.files[$i][$field] -ceq $after.files[$i][$field]) 'Installed EXE/DLL provenance changed.' }
    }
}
function Decode-Eval([string]$display) {
    $value=$display.Trim()
    if ($value.StartsWith('"') -and $value.EndsWith('"')) { return $value.Substring(1,$value.Length-2).Replace('""','"') }
    return ConvertFrom-Json -InputObject $value -NoEnumerate
}
function Installed-State {
    # First installed file read precedes every EXE/DLL access.
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim(); Require ([bool]$stamp) 'Empty installed provenance.'
    $files=@('OpenPoseidon.exe','wgpu_renderer.dll') | ForEach-Object {
        $file=Get-Item -LiteralPath (Join-Path $GameDir $_)
        @{name=$_;path=$file.FullName;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash;bytes=$file.Length;writtenUtc=$file.LastWriteTimeUtc.ToString('o')}
    }
    return @{deployedFrom=$stamp;files=@($files)}
}
$root=Split-Path -Parent $PSScriptRoot
$mission=Join-Path $root 'tests/perf/missions/perf_sand.noe'
Require (Test-Path -LiteralPath (Join-Path $mission 'mission.sqm')) 'Authored Nogova sand mission is missing.'
$before=Installed-State
$output=Join-Path $root ('build/sand-footprints/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$profile=Join-Path $output 'user'; New-Item -ItemType Directory -Force -Path $profile | Out-Null
$log=Join-Path $output 'engine.log'
$result=[ordered]@{status='running';before=$before;gates=@{};samples=@();captures=@{};error=$null;
    limitations='Nogova exact stock SandDark dry walking only. Wet/road/grass/border/snow installed controls remain pending. Signed physical height and gradient gates are independent of images; appearance is descriptive and requires root visual review.'}
$settings=@{POSEIDON_USER_DIR=$profile;POSEIDON_SNOWLINE='off';POSEIDON_SAND_TRACE='1';POSEIDON_UNIFORM_WET_TRACE='1';WGR_GRASS='0';WGR_OBJECT_SNOW='0';
    WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';
    WGR_TERRAIN_JITTER='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_TERRAIN_PUDDLES='0';WGR_INTERIOR_SKY_DEBUG='0'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN',
    'POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOW_SHELTER','POSEIDON_SNOW_TRACE',
    'WGR_SNOW_SURFACE_FIXTURE','WGR_SNOW_POWDER','WGR_TREE_SNOW','WGR_TREE_SNOW_TRACE',
    'WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','POSEIDON_INTERIOR_SKY_PROBE')
$keys=@($settings.Keys)+$clear; $savedEnv=@{}
foreach ($key in $keys) { $savedEnv[$key]=[Environment]::GetEnvironmentVariable($key,'Process') }
@{sourceHead=[string](& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
    missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash;
    helperSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Test-SandFootprintHelpers.ps1')).Hash;
    installed=$before;mission=$mission;lockOwner=$env:LOCK_OWNER;settings=$settings;clearedEnvironment=$clear;inheritedEnvironment=$savedEnv;
    sandCandidate=@(2675,5125);target=@(2675,5131);fullSupportGuard=.6;sourceUVInterior=@(.12,.88);
    grid=@{x=@(2673.53125,2680.03125);z=@(5124.03125,5132.03125);samples=3445};
    actualPathBounds=@{x=@(2674,2679.4);z=@(5125,5131.6)};
    cellSize=.125;width=1280;height=720;msaa=4;minimumWalkDistance=4;
    weatherAuthority='Actual weather rain zero, visible particle mode off/density0, snow disabled and snowline off. No direct sand stamp or wetness setter.';
    controls=@('fresh empty baseline','actual dry ordinary walking','physical negative depression and positive rim','paused camera return');limitations=$result.limitations
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
$p=$null;$client=$null;$reader=$null;$writer=$null
function Assert-RunHealth {
    if ($p -and $p.HasExited) { throw "Owned game exited early: $($p.ExitCode)" }
    if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|StartAutoTest could not boot|Unbekannter Operator' -Quiet)) { throw 'Installed runtime/mission failure.' }
}
function Send($command) {
    Assert-RunHealth; $request=$command | ConvertTo-Json -Compress
    $request | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$writer.WriteLine($request)
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        Require ([DateTime]::UtcNow -lt $deadline) 'Harness response deadline exceeded.'
        $line=$reader.ReadLine();Require ($null -ne $line) 'Harness closed.'
        $line | Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$reply=$line | ConvertFrom-Json
    } while ($null -eq $reply.ok)
    if (!$reply.ok) { throw $line };return $reply
}
function Eval([string]$code) { $reply=Send @{cmd='eval';code=$code};Require ($null -ne $reply.result) 'Missing evaluator result.';return Decode-Eval ([string]$reply.result) }
function Set-Acceleration([double]$value) { $null=Send @{cmd='exec';code=('setAccTime '+$value.ToString('R',$culture))} }
function State([string]$stage) {
    $sand=Send @{cmd='dev_sand';action='state'};Assert-SandState $sand
    $sample=@{stage=$stage;time=(Finite-SandNumber (Eval 'time'));sand=$sand;weather=(Send @{cmd='weather_visibility'});
        actor=(Eval 'getPosASL sandWalker');heading=(Eval 'getDir sandWalker');pose=(Eval 'getMove sandWalker');
        uniformWetness=(Finite-SandNumber (Eval 'triUniformWetness sandWalker'))}
    Require ($sample.actor -is [array] -and $sample.actor.Count -eq 3) 'Actual actor ASL is missing.'
    foreach ($coordinate in $sample.actor) { $null=Finite-SandNumber $coordinate }
    $null=Finite-SandNumber $sample.heading
    Require ($sample.pose -is [string] -and [bool]$sample.pose) 'Actual actor pose is missing.'
    Require ($sample.uniformWetness -ge 0 -and $sample.uniformWetness -le 1) 'Actual uniform wetness is invalid.'
    Require ((Eval 'alive sandWalker') -eq $true) 'Actual walking actor died.'
    $support=Send @{cmd='dev_sand';action='sample';x=$sample.actor[0];z=$sample.actor[1]};Assert-SandSample $support
    Assert-SandDry $sample.weather
    Require ($sand.wetness -le .001) 'Sand became wet in a dry fixture.'
    $sample.groundSupport=$support;$sample.supportGap=$sample.actor[2]-$support.surfaceY
    Require ([Math]::Abs($sample.supportGap) -le .03) 'Actual actor left physical ground support.'
    $result.samples+=,$sample;return $sample
}
function Grid([string]$stage) {
    # Fixed before/after coordinates, deliberately offset from cell centres so
    # nonzero gradients are tested as well as deepest physical support height.
    $rows=@()
    for ($j=0;$j -le 64;++$j) { for ($i=-12;$i -le 40;++$i) {
        $sample=Send @{cmd='dev_sand';action='sample';x=(2675+$i*.125+.03125);z=(5124+$j*.125+.03125)}
        Assert-SandSample $sample;Require ($sample.sourceEligible) 'Sampled walking lane left the admitted sand source.';$rows+=,$sample
    } }
    $rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output ($stage+'-grid.json'))
    return ,$rows
}
function Walk([string]$stage,[double]$targetZ,[double]$minimumDistance) {
    $start=State ($stage+'-before');$null=Send @{cmd='exec';code=('sandWalker setUnitPos "UP"; sandWalker setBehaviour "CARELESS"; group sandWalker setSpeedMode "LIMITED"; sandWalker doMove [2675,'+$targetZ.ToString('R',$culture)+',0]')}
    Assert-SandWalkPosition $start.actor;$observed=@()
    Set-Acceleration 1;$deadline=[DateTime]::UtcNow.AddSeconds(25)
    do {
        Start-Sleep -Milliseconds 300;$position=Eval 'getPosASL sandWalker'
        Assert-SandWalkPosition $position
        # Admission is read-only: validate the whole .6m boot/rim support
        # rectangle, not merely the centre of the ordinary AI's diagonal path.
        foreach ($dx in @(-.6,.6)) { foreach ($dz in @(-.6,.6)) {
            $support=Send @{cmd='dev_sand';action='sample';x=($position[0]+$dx);z=($position[1]+$dz)}
            Assert-SandSample $support;Require ($support.sourceEligible) 'Actual path boot kernel left admitted sand.'
        } }
        $observed+=,@($position)
        $distance=[Math]::Sqrt([Math]::Pow($position[0]-$start.actor[0],2)+[Math]::Pow($position[1]-$start.actor[1],2))
        if ($distance -ge $minimumDistance) { break }
        Require ([DateTime]::UtcNow -lt $deadline) 'Actor did not actually walk the required distance.'
    } while ($true)
    Set-Acceleration 0;$end=State ($stage+'-after')
    Assert-SandWalkPosition $end.actor
    $result.gates[$stage]=@{start=$start;end=$end;actualDistance=$distance;observedPath=$observed};return $end
}
function Capture([string]$stage,[string]$camera) {
    Require ((Eval ('triFreeFlyPose "'+$camera+'"')) -ceq 'OK') 'Camera refused.';Start-Sleep -Seconds $SettleSeconds
    $path=Join-Path $output ($stage+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) { Assert-RunHealth;Require ([DateTime]::UtcNow -lt $until) 'PNG missing.';Start-Sleep -Milliseconds 100 }
    $bytes=[IO.File]::ReadAllBytes($path)
    Require ($bytes.Length -ge 24 -and [BitConverter]::ToString($bytes[0..7]) -eq '89-50-4E-47-0D-0A-1A-0A') 'Invalid PNG.'
    $w=([int]$bytes[16]*16777216)+([int]$bytes[17]*65536)+([int]$bytes[18]*256)+[int]$bytes[19]
    $h=([int]$bytes[20]*16777216)+([int]$bytes[21]*65536)+([int]$bytes[22]*256)+[int]$bytes[23]
    Require ($w -eq 1280 -and $h -eq 720) 'PNG dimensions differ from the fixture.'
    $result.captures[$stage]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=$bytes.Length;width=$w;height=$h;camera=$camera;state=(State ($stage+'-capture'));utc=[DateTime]::UtcNow.ToString('o')}
}
function Close-Owned {
    if ($p -and !$p.HasExited) { $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game failed normal exit.' }
    Require ($p -and $p.ExitCode -eq 0 -and (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet)) 'Normal shutdown proof missing.'
    $result.normalExit=@{pid=$p.Id;exitCode=$p.ExitCode;shutdownComplete=$true}
}
try {
    foreach ($key in $clear) { Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue }
    foreach ($key in $settings.Keys) { [Environment]::SetEnvironmentVariable($key,$settings[$key],'Process') }
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--test-world-freefly','2675','5125','28','0','-70','--log-file',('"'+$log+'"'))
    $result.arguments=$arguments
    $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $arguments -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt')
    $null=$p.Handle;$result.pid=$p.Id;$until=[DateTime]::UtcNow.AddSeconds(120)
    do {
        Assert-RunHealth;$client=[Net.Sockets.TcpClient]::new()
        try { $client.Connect('127.0.0.1',$port) } catch { $client.Dispose();$client=$null }
        if (!$client) { Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250 }
    } while (!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Nogova mission not ready.'
    $null=Send @{cmd='exec';code='player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setDate [1985,6,21,16,0]; setAccTime 0; player setPos [2686,5125,0]'}
    Require ((Eval 'triSetSimTime 100') -ceq 'OK:100000') 'Fixture simulation time refused.'
    Require ((Eval 'triSetBrightness 1') -ceq 'OK:1.000') 'Brightness refused.'
    $null=Send @{cmd='dev_snow';action='disable'}
    $snow=Send @{cmd='dev_snow';action='state'}
    Require ($snow.enabled -is [bool] -and !$snow.enabled) 'Snow remains enabled.'
    $result.gates.snowDisabled=$snow
    $null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
    # No existing subordinate is reused: fresh independent group makes the
    # actor leader, avoiding formation movement and unsupported enableAI.
    $null=Send @{cmd='exec';code='sandWalkerGroup=createGroup west; "SoldierWB" createUnit [[2675,5125,0],sandWalkerGroup,"sandWalker=this; removeAllWeapons this; this allowDamage false"]; sandWalker setPos [2675,5125,0]; sandWalker setDir 0; sandWalker switchMove "civil"; doStop sandWalker'}
    Require ((Eval 'typeOf sandWalker') -ceq 'SoldierWB') 'Actual walking actor is not stock SoldierWB.'
    Require ((Eval '(leader sandWalkerGroup) == sandWalker') -eq $true) 'Walker is not the leader of its fresh group.'
    Set-Acceleration 1;Start-Sleep -Seconds 2;Set-Acceleration 0
    $initial=State 'initial';Require ($initial.sand.chunks -eq 0 -and $initial.sand.wetness -le .001) 'Fresh mission sand is not dry/empty.'
    Require ([Math]::Abs($initial.actor[0]-2675) -lt .2 -and [Math]::Abs($initial.actor[1]-5125) -lt .2) 'Walker placement differs from the authored source candidate.'
    $centre=Send @{cmd='dev_sand';action='sample';x=2675;z=5125};Assert-SandSample $centre
    Require ($centre.sourceEligible -and $centre.world.Replace('\','/').TrimStart('/').ToLowerInvariant() -ceq 'noe/noe.wrp' -and $centre.texture.Replace('\','/').TrimStart('/').ToLowerInvariant() -ceq 'o/pt.paa' -and $centre.surfaceClass -ceq 'SandDark' -and $centre.files -ceq 'pt??????' -and $centre.sound -ceq 'sand' -and $centre.character -ceq '') 'Actual Nogova sand source differs from the audited candidate.'
    $baseline=Grid 'baseline';Require (@($baseline | Where-Object {$_.offset -ne 0}).Count -eq 0) 'Baseline already contains signed sand edits.'
    $ground=$centre.surfaceY
    $low='2676 5124 '+($ground+1).ToString('R',$culture)+' 35 -25'
    $top='2676.5 5127 '+($ground+4).ToString('R',$culture)+' 0 -86'
    $away='2680 5131 '+($ground+8).ToString('R',$culture)+' 220 -65'
    Capture 'before-low' $low;Capture 'before-top' $top
    $walk=Walk 'dry-walking' 5131 4
    Require ($walk.sand.chunks -gt 0 -and $walk.sand.revision -gt $initial.sand.revision) 'Real dry walking produced no stored sand contacts.'
    Require ($walk.uniformWetness -le .001) 'Actual dry walking actor became wet.'
    $after=Grid 'walking-after';Require ($after.Count -eq $baseline.Count) 'Geometry grid size changed.'
    $geometry=@();for ($i=0;$i -lt $baseline.Count;++$i) { $geometry+=,(Assert-SandGeometry $baseline[$i] $after[$i]) }
    $evidence=Assert-SandEvidence $geometry;$evidence.samples=$geometry;$result.gates.geometry=$evidence
    $trace=@(Select-String -LiteralPath $log -Pattern 'SAND_STEP' | ForEach-Object {Parse-SandStep $_.Line})
    $lane=@($trace | Where-Object {$_.x -ge 2673.53125 -and $_.x -le 2680.03125 -and $_.z -ge 5124.03125 -and $_.z -le 5132.03125 -and $_.surface -ceq 'SandDark' -and $_.files -ceq 'pt??????' -and $_.sound -ceq 'sand' -and $_.wet -le .001 -and $_.texture.Replace('\','/').TrimStart('/').ToLowerInvariant() -ceq 'o/pt.paa'})
    Require ($lane.Count -gt 0) 'Actual stock-sand boot producer trace missing from the walked lane.';$result.gates.bootTrace=$lane
    # The measured walk and physical grid above are the proof. This paused
    # fixture relocation only clears the worker from subsequent appearance views.
    $centreX=($initial.actor[0]+$walk.actor[0])/2;$centreZ=($initial.actor[1]+$walk.actor[1])/2
    $lowX=$centreX-.75
    $azimuth=[Math]::Atan2(.75,$centreZ-5124)*180/[Math]::PI
    $result.gates.cameraFraming=@{actualPathMidpoint=@($centreX,$centreZ);beforeLow=$low;beforeTop=$top;adaptiveAfter=$true;pixelIsolated=$false}
    $low=$lowX.ToString('R',$culture)+' 5124 '+($ground+1).ToString('R',$culture)+' '+$azimuth.ToString('R',$culture)+' -25'
    $top=$centreX.ToString('R',$culture)+' '+$centreZ.ToString('R',$culture)+' '+($ground+4).ToString('R',$culture)+' 0 -86'
    $null=Send @{cmd='exec';code='sandWalker setPos [2686,5125,0]; doStop sandWalker'}
    $result.gates.workerRelocation=@{fixtureOnly=$true;excludedFromWalkingProof=$true;state=(State 'worker-away')}
    $pauseA=State 'pause-a'
    Capture 'after-low' $low;Capture 'after-top' $top;Capture 'after-away' $away;Capture 'after-return-low' $low
    Start-Sleep -Seconds 2;$pauseB=State 'pause-b';Assert-SandPaused $pauseA $pauseB
    $returned=Grid 'camera-return';for ($i=0;$i -lt $after.Count;++$i) { foreach ($field in @('offset','sandDx','sandDz','surfaceY','surfaceDx','surfaceDz','revision','chunks','wetness')) { Require ($after[$i].$field -eq $returned[$i].$field) 'Camera return changed paused signed sand data.' } }
    $result.gates.pause=@{a=$pauseA;b=$pauseB;exactGridReturn=$true}
    $null=Send @{cmd='exec';code='deleteVehicle sandWalker'};Close-Owned
    $result.status='physical-contact-state-verified-appearance-pending'
} catch { $result.status='failed';$result.error=$_.Exception.Message;throw }
finally {
    if ($p -and !$p.HasExited) {
        try { Close-Owned } catch {}
        if (!$p.HasExited) { $p.Kill();$null=$p.WaitForExit(5000);$result.status='failed';$result.forcedOwnedProcessStop=$true;$result.error='Owned process required forced cleanup.' }
    }
    if ($client) { $client.Dispose() }
    try { $afterPair=Installed-State;Assert-SandPair $before $afterPair;$result.after=$afterPair } catch { $result.status='failed';$result.error=$_.Exception.Message }
    foreach ($key in $keys) {
        if ($null -eq $savedEnv[$key]) { Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process') }
    }
    $result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Sand footprint evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
}
