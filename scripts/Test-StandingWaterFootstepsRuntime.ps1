# Real SDL walking and actual Man::Sound selection; no water seed or forced sound.
[CmdletBinding()]
param([switch]$SelfTest,
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='stock-water-steps',
      [double]$SearchX=2686,[double]$SearchZ=5125,
      [Nullable[double]]$PoolX,[Nullable[double]]$PoolZ,
      [ValidateRange(30,180)][int]$RainTimeoutSeconds=90,
      [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$condition,[string]$message) {if (!$condition) {throw $message}}
function Number([double]$value) {
    Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite fixture value.'
    return $value.ToString('R',$culture)
}
function Decode-Eval([string]$display) {
    $display=$display.Trim()
    if ($display.StartsWith('"')) {return $display.Substring(1,$display.Length-2).Replace('""','"')}
    if ($display.StartsWith('[') -or $display -cmatch '^(true|false|null|-?\d+(\.\d+)?([eE][+-]?\d+)?)$') {return ConvertFrom-Json -InputObject $display -NoEnumerate}
    return $display
}
function Parse-Step([string]$line) {
    $n='[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?'
    $pattern='PUDDLE_STEP model=(.*?) env=(.*?) reason=([a-z-]+) depth=('+ $n+') height=('+ $n+') support=('+ $n+') sole=('+ $n+'),('+ $n+'),('+ $n+') source=(.*?) wave=(true|false) eventMs=(\d+) timeMs=(\d+) readonly=true\s*$'
    Require ($line -cmatch $pattern) 'Missing or unknown actual audio-selection trace.'
    $m=$Matches.Clone();$row=@{line=$line;model=$m[1];env=$m[2];reason=$m[3];source=$m[10];wave=$m[11] -ceq 'true';eventMs=[long]$m[12];timeMs=[long]$m[13]}
    foreach($pair in @(@('depth',4),@('height',5),@('support',6),@('x',7),@('y',8),@('z',9))) {
        $value=[double]::Parse($m[$pair[1]],$culture)
        Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value)) 'Nonfinite actual sole/support trace.';$row[$pair[0]]=$value
    }
    Require ($row.reason -cin @('stale-or-ineligible-sole','authored-override','legacy-water-contact','dry-or-raised-support','sheltered','missing-authored-water','standing-rain-water')) 'Unknown admission policy in installed trace.'
    Require (![string]::IsNullOrWhiteSpace($row.model)) 'Actual soldier model missing.'
    return $row
}
function Variant($row) {
    if ($row.source -match '(?:^|[/\\])People[/\\]water_([LR])(?:\.(?:wss|ogg|wav))?$') {return $Matches[1].ToUpperInvariant()}
    return ''
}
function Assert-Fresh($row) {
    Require ($row.eventMs -gt 0 -and $row.timeMs -ge $row.eventMs -and $row.timeMs-$row.eventMs -le 250) 'Audio row lacks a fresh real animation contact.'
    Require $row.wave 'Existing sound wave was not available to the actual Restart path.'
}
function Assert-WaterSupport($row) {
    # These are rounded telemetry bounds, not weaker production admission.
    Require ($row.depth -ge .00295 -and $row.height-$row.support -ge .0019 -and [Math]::Abs($row.y-$row.support) -le .2511 -and $row.y -le $row.height+.1211) 'Actual event lacks standing-water/support proof.'
}
function Walk-Complete([double]$distance,[double]$minimum,[int]$eventCount) {
    return $distance -ge $minimum -and $eventCount -gt 0
}
function Body-InWindow($position,$window) {
    if (!$window) {return $true}
    $rx=if($window.ContainsKey('radiusX')) {$window.radiusX} else {$window.radius}
    $rz=if($window.ContainsKey('radiusZ')) {$window.radiusZ} else {$window.radius}
    return $position.Count -eq 3 -and [Math]::Abs($position[0]-$window.x) -le $rx -and [Math]::Abs($position[1]-$window.z) -le $rz
}
function Variant-Counts($rows) {
    $counts=@{L=0;R=0;other=0}
    foreach($row in $rows) {$v=Variant $row;if($v -cin @('L','R')) {$counts[$v]++} else {$counts.other++}}
    return $counts
}
function Collect-Variants($rows,[int]$attempts,[double]$elapsedSeconds) {
    $counts=Variant-Counts $rows
    return ($counts.L -eq 0 -or $counts.R -eq 0) -and $attempts -lt 6 -and $elapsedSeconds -lt 45
}
function Assert-Positive($rows,[bool]$requirePair=$true) {
    Require ($rows.Count -ge 4) 'Fewer than four actual exposed standing-water events.'
    $variants=@()
    foreach($row in $rows) {
        Assert-Fresh $row;Assert-WaterSupport $row
        Require ($row.reason -ceq 'standing-rain-water' -and $row.env -ieq 'water') 'Exposed contact did not select standing rainwater.'
        $v=Variant $row;Require ($v -cin @('L','R')) 'Selected wave is not an audited authored People water variant.';$variants+=$v
    }
    if ($requirePair) {Require ($variants -ccontains 'L' -and $variants -ccontains 'R') 'Both authored random water alternatives were not observed; no RNG or playback forcing is permitted.'}
    return @{eventCount=$rows.Count;variants=$variants;sourceCounts=(Variant-Counts $rows);events=$rows;selection='Actual random authored variants, not anatomical sole identifiers'}
}
function Pool-Supported($sample) {
    if (!$sample.valid) {return $false}
    foreach($key in @('depth','height','terrainY')) {if ([double]::IsNaN([double]$sample.$key) -or [double]::IsInfinity([double]$sample.$key)) {return $false}}
    $bed=[double]$sample.height-[double]$sample.depth
    return $sample.depth -ge .0035 -and $sample.depth -le .15 -and $bed -gt .02 -and [Math]::Abs($bed-$sample.terrainY) -le .15 -and $sample.height-$sample.terrainY -ge .0035
}
if ($SelfTest) {
    $line='[info] PUDDLE_STEP model=data3d\mc vojakw2.p3d env=water reason=standing-rain-water depth=0.0120 height=20.012 support=20.000 sole=100.000,20.000,200.000 source=People\water_L wave=true eventMs=1000 timeMs=1016 readonly=true'
    $l=Parse-Step $line;$r=Parse-Step ($line.Replace('water_L','water_R'));$null=Assert-Positive @($l,$r,$l,$r)
    foreach($bad in @($line.Replace('wave=true','wave=false'),$line.Replace('timeMs=1016','timeMs=1251'),$line.Replace('source=People\water_L','source=People\gravel_L'),$line.Replace('support=20.000','support=20.300'),$line.Replace('depth=0.0120','depth=0.0000'),$line.Replace('height=20.012','height=NaN'),$line.Replace('height=20.012','height=1e309'),($line+' unknown=1'))) {
        $failed=$false;try {$b=Parse-Step $bad;$null=Assert-Positive @($b,$r,$l,$r)} catch {$failed=$true};Require $failed 'Malformed/stale/dry/unsupported audio event accepted.'
    }
    $failed=$false;try {$null=Assert-Positive @($l,$l,$l,$l)} catch {$failed=$true};Require $failed 'Missing random alternative accepted as a pair.'
    $single=Assert-Positive @($l,$l,$l,$l) $false
    Require ($single.sourceCounts.L -eq 4 -and $single.sourceCounts.R -eq 0) 'Single-variant actual admission evidence lost before functional controls.'
    Require ((Collect-Variants @($l,$l,$l,$l) 0 0) -and !(Collect-Variants @($l,$r) 0 0) -and
             !(Collect-Variants @($l) 6 0) -and !(Collect-Variants @($l) 0 45)) 'Natural sample collection ignored pair/attempt/time caps.'
    Require (Pool-Supported @{valid=$true;depth=.012;height=20.012;terrainY=20}) 'Supported local pool was refused.'
    foreach($bad in @(@{valid=$false;depth=.012;height=20.012;terrainY=20},@{valid=$true;depth=.001;height=20.001;terrainY=20},@{valid=$true;depth=.012;height=20.012;terrainY=21},@{valid=$true;depth=.2;height=20.2;terrainY=20},@{valid=$true;depth=.012;height=.012;terrainY=0})) {Require (!(Pool-Supported $bad)) 'Dry/deep/raised/sea fixture admitted.'}
    Require ((Decode-Eval 'WEST Alpha:1 (mail)') -ceq 'WEST Alpha:1 (mail)' -and (Decode-Eval '[1,2,3]').Count -eq 3) 'SQF decoder failed.'
    Require (!(Walk-Complete .7 .7 0) -and !(Walk-Complete .6 .7 1) -and (Walk-Complete .7 .7 1)) 'Movement-only or stationary audio accepted as a contact walk.'
    $window=@{x=2550;z=5075;radius=.95}
    Require (Body-InWindow @(2550,5074.25,15) $window) 'Audited initial roof body window refused.'
    Require (!(Body-InWindow @(2551,5075,15) $window) -and !(Body-InWindow @(2550,5076,15) $window)) 'Walk may leave the central roof while awaiting an event.'
    $laneWindow=@{x=2550;z=5075;radiusX=.35;radiusZ=3.3}
    Require ((Body-InWindow @(2550.3,5078,15) $laneWindow) -and !(Body-InWindow @(2550.36,5075,15) $laneWindow) -and
             !(Body-InWindow @(2550,5078.31,15) $laneWindow)) 'Natural sampler escaped the already depth-audited narrow lane.'
    Write-Host 'PASS actual audio trace/support/age/variant and local-pool falsifiers; no game or audio device used.';return
}
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve the running game.'
Require (($null -eq $PoolX) -eq ($null -eq $PoolZ)) 'Specify both pool coordinates or neither; coordinates never seed water.'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')
function Pair {
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim();Require ($stamp.Length -gt 0) 'Missing installed provenance.'
    return @{stamp=$stamp;exe=(Get-FileHash -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash -LiteralPath (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
$before=Pair
$output=Join-Path $root ('build/standing-water-footsteps/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$profile=Join-Path $output 'user';New-Item -ItemType Directory -Force $profile|Out-Null
$log=Join-Path $output 'engine.log'
$result=[ordered]@{status='running';before=$before;stages=@{};captures=@{};
    limitations=@('Wave availability and Restart dispatch do not establish an audible listener mix; no loopback audio is recorded.',
      'People water_L/R are random authored alternatives, not anatomical left/right sole identifiers.',
      'Existing authored sample volume is preserved; no sound gain change.',
      'Fresh dry field and raised real roof are exercised; sea/raised roadway/missing authored source negatives remain CPU/source-only.',
      'A sampled candidate is not accepted until actual fresh sole/support/cover audio rows and measured ordinary SDL movement exist.')}
@{sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;installed=$before;lockOwner=$env:LOCK_OWNER;
  player='SoldierWB';world='noe';search=@($SearchX,$SearchZ);poolOverride=@($PoolX,$PoolZ);input='ordinary SDL W; no AI doMove or forced animation/audio';
  water='real rain only; dev_rain_water state/sample read only';trace='POSEIDON_WATER_STEP_TRACE=1';listening='not recorded'}|
    ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $output 'provenance.json')
$keys=@('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN',
  'POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOWLINE','POSEIDON_WATER_STEP_TRACE',
  'POSEIDON_MUD_TRACE','POSEIDON_SAND_TRACE','POSEIDON_PLAYER_GROUND_TRACE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_RAIN_WATER','WGR_RAIN_WATER_TRACE')
$saved=@{};foreach($key in $keys) {$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$held=$false;$heldLook=$false
function Health {
    Require ($p -and !$p.HasExited) 'Owned game exited unexpectedly.'
    if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator|StartAutoTest could not boot' -Quiet)) {throw 'Installed validation/script failure.'}
}
function Send($command) {
    Health;$line=$command|ConvertTo-Json -Compress;$line|Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$writer.WriteLine($line)
    do {$replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.';$replyLine|Add-Content -LiteralPath (Join-Path $output 'harness.jsonl');$reply=$replyLine|ConvertFrom-Json} while ($null -eq $reply.ok)
    Require ($reply.ok -eq $true) $replyLine;return $reply
}
function Exec([string]$code) {$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code) {return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function Clock {$state=Send @{cmd='query';what='play_state'};Require ($state.has_player -and $state.player_active -and $state.player_local) 'Actual controlled local player missing.';return [long]$state.time_ms}
function Step-Lines {return @(Select-String -LiteralPath $log -SimpleMatch 'PUDDLE_STEP model=' -ErrorAction SilentlyContinue)}
function Boot-Lines {return @(Select-String -LiteralPath $log -SimpleMatch 'PLAYER_GROUND_BOOT time=' -ErrorAction SilentlyContinue)}
function New-Steps([int]$cursor) {
    $lines=@(Step-Lines);$rows=@();Require ($lines.Count -ge $cursor) 'Actual audio log truncated.'
    for($i=$cursor;$i -lt $lines.Count;++$i) {$rows+=,(Parse-Step ([string]$lines[$i].Line))};return $rows
}
function Sample([double]$x,[double]$z) {return Send @{cmd='dev_rain_water';action='sample';x=$x;z=$z}}
function Set-Player([double]$x,[double]$z,[double]$direction) {
    Require ((Eval 'triClearView') -ceq 'OK') 'Render view did not release.'
    # Ordinary held look-around prevents stale external mouse aim from turning
    # the body after setDir; it does not force motion or an animation contact.
    if (!$script:heldLook) {$null=Send @{cmd='key';sc=226;hold=$true};$script:heldLook=$true}
    $y=Get-TerrainPuddleFixtureHeight {param($request) Send $request} $x $z
    Exec ('player setPosASL ['+(Number $x)+','+(Number $z)+','+(Number ($y+.02))+']; player setDir '+(Number $direction)+'; player switchCamera "EXTERNAL"; setAccTime 1')
    Start-Sleep -Seconds 2;Exec 'setAccTime 0';$actual=Eval 'getPosASL player'
    Require ([Math]::Abs($actual[0]-$x) -lt .3 -and [Math]::Abs($actual[1]-$z) -lt .3 -and [Math]::Abs($actual[2]-$y) -lt .3) 'Fixture spawn moved or cannot stand on actual ground.'
    return @{requested=@($x,$z,$y);actual=$actual;heading=$direction;actualHeading=(Eval 'getDir player');terrainHeight=$y;lookAround='ordinary held SDL LAlt226'}
}
function Manual-Walk([string]$stage,[double]$minimum,[double]$acc=1,[double]$maximum=20,[hashtable]$window=$null,[double]$releaseSeconds=.6) {
    Require ((Eval 'triClearView') -ceq 'OK') 'Render-only view remained active during player input.'
    $cursor=@(Step-Lines).Count;$bootCursor=@(Boot-Lines).Count;$start=Eval 'getPosASL player';$startTime=Clock
    $proof=@{start=$start;startTimeMs=$startTime;window=$window;polls=@();events=@();groundBootRows=@();input='ordinary SDL W and held Alt look-around';accTime=$acc;releaseSeconds=$releaseSeconds;
        inputContext=(Eval 'triGetInputContext');cameraEffect=(Eval 'triGetCameraEffectActive');status='started'}
    $result.stages[$stage]=$proof # retain evidence even when the walk fails
    Require ($proof.inputContext -ceq 'Infantry' -and $proof.cameraEffect -eq 0) 'Player input lacks unsuspended infantry context.'
    Require (Body-InWindow $start $window) 'Actor starts outside audited contact window.'
    Exec ('setAccTime '+(Number $acc));$null=Send @{cmd='key';sc=26;hold=$true};$script:held=$true
    $until=[DateTime]::UtcNow.AddSeconds($maximum)
    do {
        Start-Sleep -Milliseconds 80;$position=Eval 'getPosASL player'
        $distance=[Math]::Sqrt([Math]::Pow($position[0]-$start[0],2)+[Math]::Pow($position[1]-$start[1],2))
        $proof.events=@(New-Steps $cursor);$boots=@(Boot-Lines)
        $proof.groundBootRows=@($boots|Select-Object -Skip $bootCursor|ForEach-Object {$_.Line})
        $proof.polls+=@{position=$position;distance=$distance;audioRows=$proof.events.Count;groundBootRows=$proof.groundBootRows.Count;heading=(Eval 'getDir player')}
        Require (Body-InWindow $position $window) 'Actor left audited contact window before a real sound edge; retain fixture-unavailable evidence.'
        Require ([DateTime]::UtcNow -lt $until) ('Ordinary SDL '+$stage+' walk blocked or produced no audio edge before timeout; no forced contacts.')
    } while(!(Walk-Complete $distance $minimum $proof.events.Count))
    $null=Send @{cmd='key_up';sc=26};$script:held=$false;Start-Sleep -Milliseconds ([int](1000*$releaseSeconds/$acc));Exec 'setAccTime 0'
    $end=Eval 'getPosASL player';$endTime=Clock;$rows=@(New-Steps $cursor)
    $proof.end=$end;$proof.endTimeMs=$endTime;$proof.distance=$distance;$proof.events=$rows;$proof.pose=Eval 'getMove player';$proof.status='observed'
    Require (Body-InWindow $end $window) 'Release/deceleration escaped audited contact window.'
    Require ((Eval 'alive player') -eq $true -and $endTime -gt $startTime -and $rows.Count -gt 0) ('Actual '+$stage+' movement produced no living real animation sound contacts.')
    foreach($row in $rows) {
        Require ($row.eventMs -ge $startTime -and $row.timeMs -le $endTime -and [Math]::Abs($row.x-$start[0]) -le $minimum+2 -and [Math]::Abs($row.z-$start[1]) -le $minimum+2) 'Audio row does not belong to the controlled local walk.'
        Assert-Fresh $row
    }
    $proof.status='fresh-contacts';return $rows
}
function Capture([string]$name) {
    Start-Sleep -Milliseconds 500;$time=Clock;$path=Join-Path $output ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(10)
    while (!(Test-Path -LiteralPath $path)) {Health;Require ([DateTime]::UtcNow -lt $until) 'Screenshot missing.';Start-Sleep -Milliseconds 100}
    Require ((Clock) -eq $time) 'Paused simulation advanced during screenshot.'
    $bytes=[IO.File]::ReadAllBytes($path);Require ($bytes.Length -ge 24 -and [BitConverter]::ToString($bytes[0..7]) -eq '89-50-4E-47-0D-0A-1A-0A') 'Screenshot is not PNG.'
    $result.captures[$name]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;timeMs=$time;bytes=$bytes.Length}
}
function Pool-Lane([double]$x,[double]$z) {
    $checks=@();foreach($dz in @(-3.5,-1.75,0,1.75,3.5)) {foreach($dx in @(-.6,0,.6)) {
        $sample=Sample ($x+$dx) ($z+$dz);$checks+=@{x=$x+$dx;z=$z+$dz;sample=$sample}
        if (!(Pool-Supported $sample)) {return @{valid=$false;samples=$checks}}
    }};return @{valid=$true;x=$x;z=$z;samples=$checks}
}
function Close-Owned {
    if ($held) {$null=Send @{cmd='key_up';sc=26};$script:held=$false}
    if ($heldLook) {$null=Send @{cmd='key_up';sc=226};$script:heldLook=$false}
    Exec 'setAccTime 1';$null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)) 'Owned game failed normal quit.'
    Require ($p.ExitCode -eq 0) 'Owned game exit was nonzero.';Require (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) 'Normal shutdown proof missing.';$result.exitCode=$p.ExitCode
}
try {
    foreach($key in $keys) {[Environment]::SetEnvironmentVariable($key,$null,'Process')}
    $env:POSEIDON_USER_DIR=$profile;$env:POSEIDON_WATER_STEP_TRACE='1';$env:POSEIDON_PLAYER_GROUND_TRACE='1';$env:WGR_RAIN_WATER_TRACE='1';$env:POSEIDON_SNOWLINE='off'
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $mission=Join-Path $output 'watersteps.noe';New-Item -ItemType Directory -Force $mission|Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_sand.noe/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','14','--log-file',('"'+$log+'"'))
    $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt');$null=$p.Handle
    $until=[DateTime]::UtcNow.AddSeconds(120)
    do {Health;$client=[Net.Sockets.TcpClient]::new();try {$client.Connect('127.0.0.1',$port)} catch {$client.Dispose();$client=$null};if (!$client) {Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}} while(!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK' -and (Eval 'typeOf player') -ceq 'SoldierWB') 'Fresh actual standard soldier mission missing.'
    Exec 'player allowDamage false; 0 setFog 0; 0 setOvercast 0; 0 setRain 0; setAccTime 0'
    $null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
    $initial=Send @{cmd='dev_rain_water';action='state'};$result.stages.initial=$initial;Require ($initial.volume -eq 0) 'Fresh dry mission has stale rainwater.'
    $result.stages.dryPlacement=Set-Player $SearchX $SearchZ 90
    $dry=@(Manual-Walk 'dry-walk' 4)
    foreach($row in $dry) {Require ($row.reason -ceq 'dry-or-raised-support' -and $row.depth -lt .003 -and $row.env -ine 'water' -and (Variant $row) -eq '') 'Fresh dry contact selected standing/legacy water or lacked dry evidence.'}
    Capture 'dry-real-player'
    Exec '0 setOvercast 1; 0 setRain 1; setAccTime 8';$until=[DateTime]::UtcNow.AddSeconds($RainTimeoutSeconds)
    do {Start-Sleep -Milliseconds 600;$state=Send @{cmd='dev_rain_water';action='state'};Require ([DateTime]::UtcNow -lt $until) 'Actual rain did not create enough field water.'} while($state.width -lt 2 -or $state.rainVolume -lt 500000)
    Exec 'setAccTime 0';$result.stages.wet=Send @{cmd='dev_rain_water';action='state'};$result.stages.weather=Send @{cmd='weather_visibility'}
    Require ($result.stages.wet.volume -gt 0 -and $result.stages.weather.liquidRain -gt 0) 'Actual liquid rain/water missing.'
    $candidates=@()
    if ($null -ne $PoolX) {$candidates+=@{x=[double]$PoolX;z=[double]$PoolZ;sample=(Sample $PoolX $PoolZ)}}
    else {
        $spacing=[double]$state.spacing;$cx=[Math]::Round($SearchX/$spacing)*$spacing;$cz=[Math]::Round($SearchZ/$spacing)*$spacing
        foreach($dz in -5..5) {foreach($dx in -5..5) {$x=$cx+$dx*$spacing;$z=$cz+$dz*$spacing;$candidates+=@{x=$x;z=$z;sample=(Sample $x $z)}}}
    }
    $result.stages.poolCandidates=$candidates;$lane=$null;$attempts=@()
    foreach($candidate in @($candidates|Where-Object {Pool-Supported $_.sample}|Sort-Object {[Math]::Abs($_.sample.depth-.025)}|Select-Object -First 12)) {
        $test=Pool-Lane $candidate.x $candidate.z;$attempts+=,$test;if ($test.valid) {$lane=$test;break}
    }
    $result.stages.laneAttempts=$attempts;Require ($null -ne $lane) 'No supported shallow 7m pool lane from real rain; supply audited PoolX/PoolZ or retain fixture-unavailable evidence.'
    $result.stages.poolLane=$lane;$x=[double]$lane.x;$z=[double]$lane.z
    $result.stages.openPlacement=Set-Player $x ($z-3) 0
    $open=@(Manual-Walk 'exposed-walk' 6)
    # Natural additional walking may observe the other random authored variant.
    if (@($open|ForEach-Object {Variant $_}|Select-Object -Unique).Count -lt 2 -or $open.Count -lt 4) {
        Exec 'player setDir 180';$open+=@(Manual-Walk 'exposed-return-walk' 6)
    }
    # Valid real standing-water dispatch is sufficient to reach shelter controls;
    # independent authored random alternatives remain a strict final gate.
    $result.stages.exposedSelection=Assert-Positive $open $false;Capture 'standing-water-real-player'
    $result.stages.roof=New-TerrainPuddleStockRoof -SendCommand {param($request) Send $request} -X $x -Z $z -RoofLift 3
    $result.stages.coveredPlacement=Set-Player $x ($z-.75) 0
    Require (Pool-Supported (Sample $x ($z-.75))) 'Covered control lost actual water before contact.'
    $covered=@(Manual-Walk 'covered-walk' 1.1 .2 25 @{x=$x;z=$z;radius=.95} .15)
    foreach($row in $covered) {
        Assert-WaterSupport $row
        Require ($row.reason -ceq 'sheltered' -and $row.env -ine 'water' -and (Variant $row) -eq '' -and [Math]::Abs($row.x-$x) -le 1 -and [Math]::Abs($row.z-$z) -le 1) 'Covered actual wet contact did not reject standing-water selection within verified roof.'
    }
    $end=$result.stages['covered-walk'].end
    Require (Body-InWindow $end @{x=$x;z=$z;radius=.95}) 'Real covered walking escaped bounded floor-free roof region.'
    Capture 'covered-water-real-player';Remove-TerrainPuddleStockRoof {param($request) Send $request}
    $result.stages.removedPlacement=Set-Player $x ($z-3) 0
    $removed=@(Manual-Walk 'roof-removed-walk' 6);$result.stages.removedSelection=Assert-Positive $removed $false;Capture 'roof-removed-real-player'
    $combined=@($open)+@($removed);$samplingStart=[DateTime]::UtcNow;$samplingUntil=$samplingStart.AddSeconds(45);$attempt=0
    $sampling=@{maxTraversals=6;maxWallSeconds=45;attempts=@();sourceCounts=(Variant-Counts $combined);status='checking-natural-alternatives';
                policy='Only actual fresh real walking events; no source, RNG, animation or playback setters.'}
    $result.stages.variantSampling=$sampling
    while(Collect-Variants $combined $attempt ([DateTime]::UtcNow-$samplingStart).TotalSeconds) {
        # Keep enough wall time for placement, a real walk and release. Every
        # pass rechecks the previously audited physical lane at current rain state.
        if(($samplingUntil-[DateTime]::UtcNow).TotalSeconds -lt 5) {break}
        $attempt++;$phase=@{attempt=$attempt;sourceCountsBefore=(Variant-Counts $combined);status='validating-real-lane'};$sampling.attempts+=$phase
        $phase.lane=Pool-Lane $x $z;Require $phase.lane.valid 'Actual standing-water lane changed before natural variant sampling.'
        $phase.placement=Set-Player $x ($z-2) 0
        $remaining=($samplingUntil-[DateTime]::UtcNow).TotalSeconds;Require ($remaining -gt 1) 'Natural variant wall-time budget exhausted before real movement.'
        $extra=@(Manual-Walk ('variant-walk-'+$attempt) 4 1 ([Math]::Min(10,$remaining-.5)) @{x=$x;z=$z;radiusX=.35;radiusZ=3.3} .3)
        foreach($row in $extra) {
            Require ([Math]::Abs($row.x-$x) -le .6 -and [Math]::Abs($row.z-$z) -le 3.5) 'Natural extra sole event left the actually sampled wet lane.'
        }
        $combined+=@($extra);$phase.selection=Assert-Positive $combined $false;$phase.sourceCountsAfter=Variant-Counts $combined;$phase.status='actual-fresh-water-contacts'
        $sampling.sourceCounts=Variant-Counts $combined;$sampling.elapsedWallSeconds=([DateTime]::UtcNow-$samplingStart).TotalSeconds
        Require ($sampling.elapsedWallSeconds -le 45) 'Natural variant collection exceeded its wall-time cap.'
    }
    $sampling.elapsedWallSeconds=([DateTime]::UtcNow-$samplingStart).TotalSeconds;$sampling.sourceCounts=Variant-Counts $combined
    $sampling.status=if((Collect-Variants $combined 0 0)) {'bounded-sampling-exhausted'} else {'actual-both-alternatives-observed'}
    $result.stages.combinedWaterSelection=Assert-Positive $combined
    # Allow release/deceleration to settle before the stationary/pause cursor.
    Exec 'setAccTime 1';Start-Sleep -Seconds 2;Exec 'setAccTime 0';Start-Sleep -Milliseconds 300
    $cursor=@(Step-Lines).Count;$clock=Clock;$paused=Send @{cmd='dev_rain_water';action='state'}
    Capture 'paused-water';Start-Sleep -Seconds 5;$again=Send @{cmd='dev_rain_water';action='state'}
    Require ((Clock) -eq $clock -and $again.revision -eq $paused.revision -and $again.generation -eq $paused.generation -and $again.volume -eq $paused.volume -and @(Step-Lines).Count -eq $cursor) 'Paused contact/query/camera advanced water or dispatched another footstep.'
    $result.stages.pause=@{timeMs=$clock;before=$paused;after=$again;audioRows=$cursor}
    Close-Owned;$result.status='actual-water-audio-selection-passed-listening-required'
} catch {
    $result.status='failed';$result.error=$_.Exception.Message
    $result.errorSource=@{line=$_.InvocationInfo.ScriptLineNumber;position=$_.InvocationInfo.PositionMessage;stack=$_.ScriptStackTrace}
    if ($p -and !$p.HasExited -and $client) {
        try {
            if ($held) {$null=Send @{cmd='key_up';sc=26};$script:held=$false}
            Exec 'setAccTime 0'
            $result.failure=@{position=(Eval 'getPosASL player');heading=(Eval 'getDir player');pose=(Eval 'getMove player');timeMs=(Clock);
                inputContext=(Eval 'triGetInputContext');cameraEffect=(Eval 'triGetCameraEffectActive');audioRows=@(Step-Lines).Count;groundBootRows=@(Boot-Lines).Count}
        } catch {$result.failureDiagnosticsError=$_.Exception.Message}
    }
    throw
}
finally {
    if ($p -and !$p.HasExited) {try {Close-Owned} catch {$result.cleanupError=$_.Exception.Message;if (!$p.HasExited) {$p.Kill();$null=$p.WaitForExit(5000);$result.forcedCleanup=$true}}}
    if ($client) {$client.Dispose()}
    try {$result.after=Pair;if ($before.stamp -cne $result.after.stamp -or $before.exe -cne $result.after.exe -or $before.dll -cne $result.after.dll) {$result.status='failed';$result.provenanceError='Installed pair changed during test.'}} catch {$result.status='failed';$result.provenanceError=$_.Exception.Message}
    foreach($key in $keys) {[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    $result|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Actual standing-water audio evidence: $output"
    if ($result.provenanceError) {throw $result.provenanceError}
}
