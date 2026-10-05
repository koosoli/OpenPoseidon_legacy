<#
.SYNOPSIS
Locked installed Nogova test of real boot-produced signed mud geometry.
.DESCRIPTION
No direct stamp, deposit or wetness setter. Dry, covered wet and exposed wet
controls use ordinary AI walking, as Test-SnowCrawl does. A paused, identical
CPU sample grid proves physical support-height and gradient changes. Captures
are descriptive geometry evidence, not pixel-isolated appearance acceptance.
#>
[CmdletBinding()]
param(
    [ValidateRange(1,4)][double]$Acceleration=4,
    [ValidateRange(30,120)][int]$WetDeadlineSeconds=60,
    [ValidateRange(1,8)][int]$SettleSeconds=2,
    [switch]$BootRelief,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='nogova-boots',
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Game already running; refusing shared ownership.' }
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$condition,[string]$message) { if (!$condition) { throw $message } }
function Finite-MudNumber($value) {
    Require ($value -is [ValueType] -and $value -isnot [bool]) 'Mud field is not numeric.'
    $number=[double]$value
    Require (![double]::IsNaN($number) -and ![double]::IsInfinity($number)) 'Mud field is nonfinite.'
    return $number
}
function Assert-MudState($sample) {
    Require ($sample.enabled -is [bool] -and $sample.enabled) 'Mud is not enabled.'
    $wet=Finite-MudNumber $sample.wetness
    Require ($wet -ge 0 -and $wet -le 1) 'Mud wetness is outside [0,1].'
    foreach ($field in @('chunks','revision','rejected')) {
        $value=Finite-MudNumber $sample.$field
        Require ($value -ge 0 -and [Math]::Floor($value) -eq $value -and $value -le 9007199254740991) 'Mud counter is invalid.'
    }
}
function Assert-MudSample($sample) {
    Assert-MudState $sample
    foreach ($field in @('x','z','offset','mudDx','mudDz','surfaceY','surfaceDx','surfaceDz')) { $null=Finite-MudNumber $sample.$field }
    Require ($sample.offset -ge -.35001 -and $sample.offset -le .000001) 'Mud offset exceeds the signed 35-centimetre contract.'
    Require ($sample.sourceEligible -is [bool]) 'Actual source admission is missing.'
    Require ($sample.world -is [string] -and $sample.texture -is [string]) 'Actual terrain source identity is missing.'
}
function Assert-MudGeometry($before,$after) {
    Assert-MudSample $before; Assert-MudSample $after
    Require ($before.x -eq $after.x -and $before.z -eq $after.z) 'Physical samples are not at identical coordinates.'
    Require ($before.world -ceq $after.world -and $before.texture -ceq $after.texture) 'Actual source changed between geometry queries.'
    $depthDelta=[double]$after.offset-[double]$before.offset
    Require ([Math]::Abs(($after.surfaceY-$before.surfaceY)-$depthDelta) -le .00006) 'Physical terrain support height does not include the signed mud offset.'
    foreach ($axis in @('Dx','Dz')) {
        $physical='surface'+$axis; $field='mud'+$axis
        Require ([Math]::Abs(($after.$physical-$before.$physical)-($after.$field-$before.$field)) -le .0001) 'Physical terrain gradient does not include the mud gradient.'
    }
    return @{x=$after.x;z=$after.z;depthDelta=$depthDelta;
        supportDelta=($after.surfaceY-$before.surfaceY); dxDelta=($after.surfaceDx-$before.surfaceDx); dzDelta=($after.surfaceDz-$before.surfaceDz)}
}
function Assert-MudPaused($a,$b) {
    $null=Finite-MudNumber $a.time;$null=Finite-MudNumber $b.time
    Assert-MudState $a.mud;Assert-MudState $b.mud
    Require ($a.time -eq $b.time) 'Simulation advanced during paused mud/camera return.'
    foreach ($field in @('enabled','wetness','chunks','revision','rejected')) { Require ($a.mud.$field -eq $b.mud.$field) 'Mud state changed while paused.' }
    foreach ($field in @('rain','particleDensity','particleSnowflakes','liquidRain')) { Require ($a.weather.$field -eq $b.weather.$field) 'Rain authority changed while paused.' }
    Require ($a.actor.Count -eq 3 -and $b.actor.Count -eq 3) 'Actor ASL is missing.'
    for ($i=0;$i -lt 3;++$i) { Require ($a.actor[$i] -eq $b.actor[$i]) 'Actor moved during paused camera return.' }
    Require ($a.pose -ceq $b.pose -and $a.heading -eq $b.heading) 'Actor pose or heading changed during paused camera return.'
    Require ($a.uniformWetness -eq $b.uniformWetness) 'Uniform wetness changed during paused camera return.'
}
function Parse-MudStep([string]$line) {
    Require ($line -match 'MUD_STEP x=([\d.eE+-]+) z=([\d.eE+-]+) surface=(\S+) files=(\S+) sound=(\S+) texture=(\S+) wet=([\d.eE+-]+) depth=([\d.eE+-]+) revision=(\d+) chunks=(\d+)') 'Actual boot contact trace grammar is missing.'
    $row=@{x=[double]::Parse($Matches[1],$culture);z=[double]::Parse($Matches[2],$culture);surface=$Matches[3];files=$Matches[4];sound=$Matches[5];texture=$Matches[6];wet=[double]::Parse($Matches[7],$culture);depth=[double]::Parse($Matches[8],$culture);revision=[double]::Parse($Matches[9],$culture);chunks=[double]::Parse($Matches[10],$culture)}
    foreach ($field in @('x','z','wet','depth','revision','chunks')) { $null=Finite-MudNumber $row[$field] }
    Require ($row.wet -gt .15 -and $row.wet -le 1 -and $row.depth -gt 0 -and $row.depth -le .35001 -and $row.chunks -gt 0) 'Contact trace has no bounded actual wet depression.'
    return $row
}
function Assert-MudPair($before,$after) {
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
$mission=Join-Path $root 'tests/perf/missions/perf_mud.noe'
Require (Test-Path -LiteralPath (Join-Path $mission 'mission.sqm')) 'Authored Nogova mud mission is missing.'
$before=Installed-State
$output=Join-Path $root ('build/mud-footprints/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$profile=Join-Path $output 'user'; New-Item -ItemType Directory -Force -Path $profile | Out-Null
$log=Join-Path $output 'engine.log'
$result=[ordered]@{status='running';before=$before;gates=@{};samples=@();captures=@{};error=$null;
    limitations='Nogova exact cultivated soil only. Hard road/grass/border and snow installed negatives remain untested; source/CPU exclusion tests are separate. Images are descriptive, not isolated pixel acceptance.'}
$settings=@{POSEIDON_USER_DIR=$profile;POSEIDON_SNOWLINE='off';POSEIDON_MUD_TRACE='1';POSEIDON_UNIFORM_WET_TRACE='1';WGR_GRASS='0';WGR_OBJECT_SNOW='0';
    WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';
    WGR_TERRAIN_JITTER='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_TERRAIN_PUDDLES='1';WGR_INTERIOR_SKY_DEBUG='0'}
if($BootRelief){$settings.POSEIDON_BOOT_RELIEF_PROTOTYPE='1';$result.bootReliefPrototype=$true}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN',
    'POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','POSEIDON_INTERIOR_SKY_PROBE')
$keys=@($settings.Keys)+$clear; $savedEnv=@{}
foreach ($key in $keys) { $savedEnv[$key]=[Environment]::GetEnvironmentVariable($key,'Process') }
@{sourceHead=[string](& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;
    missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash;
    roofHelperSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')).Hash;
    installed=$before;mission=$mission;lockOwner=$env:LOCK_OWNER;settings=$settings;clearedEnvironment=$clear;inheritedEnvironment=$savedEnv;
    soilCandidate=@(4975,4675);cellSize=.125;width=1280;height=720;msaa=4;acceleration=$Acceleration;
    wetAuthority='weather_particles particle density 1 liquid; actual GLandscape rain stays dry. Tests the visible-liquid override production pathway.';
    controls=@('dry actual walking','covered wet actual walking','removed-roof wet actual walking');limitations=$result.limitations
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'provenance.json')
. "$PSScriptRoot/TerrainPuddleStockRoof.ps1"
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
    $mud=Send @{cmd='dev_mud';action='state'};Assert-MudState $mud
    $sample=@{stage=$stage;time=(Finite-MudNumber (Eval 'time'));mud=$mud;weather=(Send @{cmd='weather_visibility'});
        actor=(Eval 'getPosASL mudWalker');heading=(Eval 'getDir mudWalker');pose=(Eval 'getMove mudWalker');
        uniformWetness=(Finite-MudNumber (Eval 'triUniformWetness mudWalker'))}
    Require ($sample.actor -is [array] -and $sample.actor.Count -eq 3) 'Actual actor ASL is missing.'
    foreach ($coordinate in $sample.actor) { $null=Finite-MudNumber $coordinate }
    $null=Finite-MudNumber $sample.heading
    Require ($sample.pose -is [string] -and [bool]$sample.pose) 'Actual actor pose is missing.'
    Require ($sample.uniformWetness -ge 0 -and $sample.uniformWetness -le 1) 'Actual uniform wetness is invalid.'
    Require ((Eval 'alive mudWalker') -eq $true) 'Actual walking actor died.'
    $support=Send @{cmd='dev_mud';action='sample';x=$sample.actor[0];z=$sample.actor[1]};Assert-MudSample $support
    $sample.groundSupport=$support;$sample.supportGap=$sample.actor[2]-$support.surfaceY
    Require ([Math]::Abs($sample.supportGap) -le .03) 'Actual actor left physical ground support.'
    $result.samples+=,$sample;return $sample
}
function Grid([string]$stage) {
    # Fixed before/after coordinates, deliberately offset from cell centres so
    # nonzero gradients are tested as well as deepest physical support height.
    $rows=@()
    for ($j=0;$j -le 52;++$j) { for ($i=-4;$i -le 4;++$i) {
        $sample=Send @{cmd='dev_mud';action='sample';x=(4975+$i*.125+.03125);z=(4674.125+$j*.125+.03125)}
        Assert-MudSample $sample;$rows+=,$sample
    } }
    $rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output ($stage+'-grid.json'))
    return ,$rows
}
function Walk([string]$stage,[double]$targetZ,[double]$minimumDistance,[double]$walkAcceleration=1) {
    $start=State ($stage+'-before');$null=Send @{cmd='exec';code=('mudWalker setUnitPos "UP"; mudWalker setBehaviour "CARELESS"; group mudWalker setSpeedMode "LIMITED"; mudWalker doMove [4975,'+$targetZ.ToString('R',$culture)+',0]')}
    Set-Acceleration $walkAcceleration;$deadline=[DateTime]::UtcNow.AddSeconds(25)
    do {
        Start-Sleep -Milliseconds 300;$position=Eval 'getPosASL mudWalker'
        $distance=[Math]::Sqrt([Math]::Pow($position[0]-$start.actor[0],2)+[Math]::Pow($position[1]-$start.actor[1],2))
        if ($distance -ge $minimumDistance) { break }
        Require ([DateTime]::UtcNow -lt $deadline) 'Actor did not actually walk the required distance.'
    } while ($true)
    Set-Acceleration 0;$end=State ($stage+'-after')
    Require ([Math]::Abs($end.actor[0]-4975) -le .6) 'Actual walking path escaped the sampled lane.'
    $result.gates[$stage]=@{start=$start;end=$end;actualDistance=$distance};return $end
}
function Reset-Walker {
    # Placement between controls is a fixture operation, never footprint proof.
    # This engine implements disableAI but has no enableAI operator. A fresh
    # fixture actor keeps each control independent without an unsupported call.
    $null=Send @{cmd='exec';code='deleteVehicle mudWalker; mudWalkerGroup=createGroup west; "SoldierWB" createUnit [[4975,4674.25,0],mudWalkerGroup,"mudWalker=this; removeAllWeapons this; this allowDamage false"]; mudWalker setPos [4975,4674.25,0]; mudWalker setDir 0; mudWalker switchMove "civil"; doStop mudWalker'}
    Set-Acceleration 1;Start-Sleep -Seconds 2;Set-Acceleration 0
    $position=Eval 'getPosASL mudWalker';Require ([Math]::Abs($position[0]-4975) -lt .2 -and [Math]::Abs($position[1]-4674.25) -lt .2) 'Walker placement failed.'
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
    $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--test-world-freefly','4975','4675','28','0','-70','--log-file',('"'+$log+'"'))
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
    $null=Send @{cmd='exec';code='player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setDate [1985,6,21,16,0]; setAccTime 0; "SoldierWB" createUnit [[4975,4674.25,0],group player,"mudWalker=this; removeAllWeapons this; this allowDamage false"]; player setPos [4970,4675,0]'}
    Require ((Eval 'typeOf mudWalker') -ceq 'SoldierWB') 'Actual walking actor is not stock SoldierWB.'
    Require ((Eval 'triSetBrightness 1') -ceq 'OK:1.000') 'Brightness refused.'
    $null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false};Reset-Walker
    $initial=State 'initial';Require ($initial.mud.chunks -eq 0 -and $initial.mud.wetness -le .001) 'Fresh mission mud is not dry/empty.'
    $centre=Send @{cmd='dev_mud';action='sample';x=4975;z=4675};Assert-MudSample $centre
    Require ($centre.sourceEligible -and $centre.world.Replace('\','/').TrimStart('/').ToLowerInvariant() -ceq 'noe/noe.wrp' -and $centre.texture.Replace('\','/').TrimStart('/').ToLowerInvariant() -ceq 'o/pole2.paa') 'Actual Nogova cultivated source differs from the audited candidate.'
    $baseline=Grid 'baseline';Require (@($baseline | Where-Object {$_.offset -ne 0}).Count -eq 0) 'Baseline already contains depressions.'
    $ground=$centre.surfaceY;$camera='4975 4673 '+($ground+2.2).ToString('R',$culture)+' 0 -55';$away='4980 4680 '+($ground+8).ToString('R',$culture)+' 220 -65'
    Capture 'dry-before' $camera;$dry=Walk 'dry-walking' 4680.25 4
    Require ($dry.mud.chunks -eq 0 -and $dry.mud.revision -eq $initial.mud.revision) 'Actual dry walking deformed the mud field.'
    Require ($dry.uniformWetness -le .001) 'Actual dry walking actor started wet.'
    # Exercise the shared source's important negative before liquid wetting:
    # visible snowflakes with dry actual weather must wet neither cloth nor soil.
    $null=Send @{cmd='weather_particles';action='set';mode='particle';density=1;snowflakes=$true}
    $snowStart=State 'snow-particle-start';Set-Acceleration $Acceleration;$deadline=[DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 500;$snow=State 'snow-particle'
        Require ($snow.weather.rain -le .02 -and $snow.weather.particleDensity -ge .99 -and $snow.weather.particleSnowflakes -and $snow.weather.liquidRain -le .001) 'Actual snow-particle negative authority differs.'
        if ($snow.time-$snowStart.time -ge 20) { break }
        Require ([DateTime]::UtcNow -lt $deadline) 'Snow-particle negative did not complete real simulation time.'
    } while ($true)
    Set-Acceleration 0;$snow=State 'snow-particle-paused'
    Require ($snow.mud.wetness -le .001 -and $snow.mud.chunks -eq 0 -and $snow.uniformWetness -le .001) 'Visible snowflakes incorrectly wet soil or stock uniform.'
    $result.gates.snowParticles=@{start=$snowStart;end=$snow;minimumSimSeconds=20}
    $null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
    $roof=New-TerrainPuddleStockRoof -SendCommand {param($request) Send $request} -X 4975 -Z 4675 -ControlX 4987 -ControlZ 4675 -RoofLift 3;$result.roof=$roof
    Reset-Walker
    $null=Send @{cmd='weather_particles';action='set';mode='particle';density=1;snowflakes=$false}
    $wetStart=State 'wetting-start';Set-Acceleration $Acceleration;$deadline=[DateTime]::UtcNow.AddSeconds($WetDeadlineSeconds)
    do {
        Start-Sleep -Milliseconds 500;$wet=State 'wetting'
        Require ($wet.weather.rain -le .02 -and $wet.weather.particleDensity -ge .99 -and !$wet.weather.particleSnowflakes -and $wet.weather.liquidRain -ge .99) 'Visible-liquid diagnostic did not supply actual rain authority.'
        if ($wet.mud.wetness -ge .90 -and $wet.time-$wetStart.time -ge 20) { break }
        Require ([DateTime]::UtcNow -lt $deadline) 'Actual liquid rain did not wet the soil.'
    } while ($true)
    # A destination only 1.6m away is inside the AI arrival tolerance. Request
    # the ordinary full walking lane, then pause after a measured .7m under
    # the roof. Slow only this narrow control so RPC/poll latency cannot carry
    # the actor beyond the independently bounded covered endpoint.
    Set-Acceleration 0;$coveredBefore=State 'covered-before';$covered=Walk 'covered-walking' 4680.25 .7 .2
    Require ($covered.actor[1] -le 4676 -and $covered.actor[1] -ge 4674.25) 'Covered walking escaped the central floor-free roof region.'
    Require ($covered.mud.chunks -eq 0 -and $covered.mud.revision -eq $coveredBefore.mud.revision) 'Actual roof-covered walking produced mud depressions.'
    Require ($covered.uniformWetness -le .001) 'Actual roof-covered uniform became wet from liquid particles.'
    Capture 'covered-walking' $camera
    Remove-TerrainPuddleStockRoof -SendCommand {param($request) Send $request};Reset-Walker
    $openStart=State 'open-before';Require ($openStart.mud.chunks -eq 0) 'Covered/reset controls contaminated exposed baseline.'
    $open=Walk 'open-walking' 4680.25 4
    Require ($open.mud.chunks -gt 0 -and $open.mud.revision -gt $openStart.mud.revision) 'Real exposed wet walking produced no stored contacts.'
    Require ($open.uniformWetness -gt $covered.uniformWetness+.02) 'Actual exposed stock uniform did not wet from visible liquid particles with dry landscape weather.'
    $after=Grid 'open-after';Require ($after.Count -eq $baseline.Count) 'Geometry grid size changed.'
    $geometry=@();for ($i=0;$i -lt $baseline.Count;++$i) { $geometry+=,(Assert-MudGeometry $baseline[$i] $after[$i]) }
    $negative=@($geometry | Where-Object {$_.depthDelta -lt -.001})
    Require ($negative.Count -gt 0) 'No meaningful negative physical terrain support height in the walked lane.'
    Require (($negative | Measure-Object depthDelta -Minimum).Minimum -lt -.10) 'Wet mud did not create an actual decimetre-scale depression.'
    Require (@($negative | Where-Object {[Math]::Abs($_.dxDelta)+[Math]::Abs($_.dzDelta) -gt .001}).Count -gt 0) 'Mud changed no physical terrain gradient.'
    $result.gates.geometry=@{samples=$geometry;negativeCount=$negative.Count;minimumOffset=($negative | Measure-Object depthDelta -Minimum).Minimum}
    $trace=@(Select-String -LiteralPath $log -Pattern 'MUD_STEP' | ForEach-Object {Parse-MudStep $_.Line})
    $lane=@($trace | Where-Object {$_.x -ge 4974.4 -and $_.x -le 4975.6 -and $_.z -ge 4674 -and $_.z -le 4681 -and $_.texture.Replace('\','/').TrimStart('/').ToLowerInvariant() -ceq 'o/pole2.paa'})
    Require ($lane.Count -gt 0) 'Actual stock-soil boot producer trace missing from the walked lane.';$result.gates.bootTrace=$lane
    $pauseA=State 'pause-a';Capture 'wet-close' $camera;Capture 'wet-away' $away;Capture 'wet-return' $camera;Start-Sleep -Seconds 2;$pauseB=State 'pause-b';Assert-MudPaused $pauseA $pauseB
    if($BootRelief){
        Require (Select-String -LiteralPath $log -Pattern 'BOOT_RELIEF_PROTOTYPE admitted=authored-footstep-mark' -Quiet) 'Actual authored boot material route absent.'
        $result.gates.bootReliefMaterialAdmission=$true
    }
    $returned=Grid 'camera-return';for ($i=0;$i -lt $after.Count;++$i) { foreach ($field in @('offset','mudDx','mudDz','surfaceY','surfaceDx','surfaceDz','revision','chunks','wetness')) { Require ($after[$i].$field -eq $returned[$i].$field) 'Camera return changed paused physical mud data.' } }
    $result.gates.pause=@{a=$pauseA;b=$pauseB;exactGridReturn=$true}
    if($BootRelief){
        $null=Send @{cmd='exec';code='0 setFog .78;setAccTime 1'};Start-Sleep -Seconds 1
        $null=Send @{cmd='exec';code='setAccTime 0'};Capture 'wet-close-fog' $camera
    }
    $null=Send @{cmd='exec';code='deleteVehicle mudWalker'};Close-Owned
    $result.status='physical-contact-state-verified-appearance-pending'
} catch { $result.status='failed';$result.error=$_.Exception.Message;throw }
finally {
    if ($p -and !$p.HasExited) {
        try { Close-Owned } catch {}
        if (!$p.HasExited) { $p.Kill();$null=$p.WaitForExit(5000);$result.status='failed';$result.forcedOwnedProcessStop=$true;$result.error='Owned process required forced cleanup.' }
    }
    if ($client) { $client.Dispose() }
    try { $afterPair=Installed-State;Assert-MudPair $before $afterPair;$result.after=$afterPair } catch { $result.status='failed';$result.error=$_.Exception.Message }
    foreach ($key in $keys) {
        if ($null -eq $savedEnv[$key]) { Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($key,$savedEnv[$key],'Process') }
    }
    $result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Mud footprint evidence: $output"
    if ($result.status -eq 'failed') { throw $result.error }
}
