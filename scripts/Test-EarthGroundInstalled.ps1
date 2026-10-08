<# Installed runtime owner invokes through scripts/with-game-lock.sh after a
   matched deployment. Tests the actual stock controlled SoldierWB and ordinary
   SDL input. The original retail Everon mission supplies real model/config data;
   Earth rendering and resident ground replace its terrain queries. No empty WRP
   or NoShape actor is accepted. Screenshots still require visual inspection. #>
[CmdletBinding()]
param(
    [ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
    [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [string]$CacheDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
    [ValidateRange(60,240)][int]$PatchTimeoutSeconds=150,
    [switch]$SelfTest
)
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Num($value){$n=[double]::Parse([string]$value,[Globalization.NumberStyles]::Float,$culture);Require ([double]::IsFinite($n)) 'Nonfinite runtime value';return $n}
function Fmt($value){return (Num $value).ToString('R',$culture)}
function Decode([string]$value){
    $value=$value.Trim()
    if($value.StartsWith('"')){Require $value.EndsWith('"') 'Incomplete SQF string';return $value.Substring(1,$value.Length-2).Replace('""','"')}
    return ConvertFrom-Json -InputObject $value -NoEnumerate
}
function GroundData([string]$value){
    Require (!$value.StartsWith('FAIL:')) ('Ground diagnostic refused: '+$value)
    $ground=$value|ConvertFrom-Json
    Require ($ground.covered -is [bool] -and (Num $ground.revision)-ge1) 'Malformed ground diagnostic'
    if($ground.covered){
        Require ($ground.bounds.Count-eq4 -and (Num $ground.grid)-ge1 -and (Num $ground.generation)-ge1) 'Missing resident bounds'
        foreach($field in @('height','dX','dZ')){$null=Num $ground.$field}
        foreach($edge in $ground.bounds){$null=Num $edge}
    }
    return $ground
}
function HorizontalDistance($a,$b){return [Math]::Sqrt([Math]::Pow((Num $a[0])-(Num $b[0]),2)+[Math]::Pow((Num $a[1])-(Num $b[1]),2))}
function AssertStanding($state){
    Require ($state.alive -and $state.onFoot -and $state.ground.covered) 'Controlled player lost live resident ground'
    Require ($state.ground.playerContactPoints-gt0 -and $state.contactClearance-gt-9000) 'Stock model has no real land-contact shape (NoShape is not acceptance)'
    Require ($state.clearance-ge-.3 -and $state.clearance-le1.5) ('Player root buried/floating: '+$state.clearance)
    Require ($state.terrainClearance-ge-.3 -and $state.terrainClearance-le1.5) 'Legacy terrain clearance differs from resident support'
    Require ($state.contactClearance-ge-.75 -and $state.contactClearance-le1.5) 'Contact bounding box is far below/above resident ground'
    Require ([Math]::Abs((Num $state.position[2])-$state.ground.height-$state.clearance)-lt.08) 'ASL root and ground-query clearance disagree'
}
if($SelfTest){
    $ground=GroundData '{"covered":true,"revision":3,"generation":7,"bounds":[3200,3200,9600,9600],"grid":25,"height":300,"dX":0,"dZ":0,"playerContactPoints":2,"playerDown":false}'
    $state=@{alive=$true;onFoot=$true;ground=$ground;clearance=.6;terrainClearance=.6;contactClearance=.6;position=@(6181.81,6985.71,300.6)}
    AssertStanding $state
    Require ((HorizontalDistance @(0,0,100) @(3,4,-100))-eq5) 'ASL distance used altitude as north'
    Require ((Decode '"{""covered"":false}"')-ceq'{"covered":false}') 'SQF JSON string decoding failed'
    foreach($bad in @(-9999,-1,3)){
        $copy=$state.Clone();$copy.contactClearance=$bad;$rejected=$false
        try{AssertStanding $copy}catch{$rejected=$true};Require $rejected 'Invalid model/contact clearance accepted'
    }
    $rejected=$false;try{$null=Num 'NaN'}catch{$rejected=$true};Require $rejected 'Nonfinite coordinate accepted'
    Write-Host 'Earth installed acceptance helpers PASS; no game, profile or GPU accessed'
    return
}
Require ([bool]$ExpectedCommit) 'Supply -ExpectedCommit'
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved'
$repo=Split-Path -Parent $PSScriptRoot
function Pair{
    $stamp=(Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw).Trim()
    Require ($stamp-match '^\S+\s+([a-fA-F0-9]{8,40})(?:\s|$)') 'Malformed installed source stamp'
    $installedCommit=$Matches[1]
    Require ($installedCommit.StartsWith($ExpectedCommit,[StringComparison]::OrdinalIgnoreCase) -or $ExpectedCommit.StartsWith($installedCommit,[StringComparison]::OrdinalIgnoreCase)) 'Installed commit differs'
    return @{stamp=$stamp;exe=(Get-FileHash -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash -LiteralPath (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
$before=Pair
$mission=Join-Path $repo 'tests/perf/missions/perf_field.eden'
Require (Test-Path -LiteralPath (Join-Path $mission 'mission.sqm')) 'Proven stock Everon player fixture missing'
$imagery=Join-Path $CacheDirectory 'blue-marble-june-5400.jpg'
Require (Test-Path -LiteralPath $imagery) 'Cached NASA overview required; this acceptance does not download overview imagery'
$out=Join-Path $repo ('build/earth-ground-installed/'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$profile=Join-Path $out 'user';$null=New-Item -ItemType Directory -Force -Path $profile
$log=Join-Path $out 'engine.log';$stderr=Join-Path $out 'stderr.log'
$settings=@{
    POSEIDON_WHOLE_WORLD='1';POSEIDON_EARTH_LATITUDE='49.61';POSEIDON_EARTH_LONGITUDE='6.13';
    POSEIDON_EARTH_CACHE=$CacheDirectory;POSEIDON_EARTH_IMAGERY=$imagery;
    POSEIDON_EARTH_BUILDINGS='0';POSEIDON_EARTH_SURFACE='0';POSEIDON_USER_DIR=$profile;
    POSEIDON_PLAYER_GROUND_TRACE='1';WGR_WET_SOIL_DIAGNOSTIC='1';WGR_GRASS='0';
    POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0';WGR_TEMPORAL='0'
}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES')
$saved=@{};foreach($key in @($settings.Keys)+$clear){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$result=[ordered]@{passed=$false;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;fixture=$mission;samples=@();captures=@{};scope='Actual live stock player, near-resident terrain only, ordinary W and Z input. Native buildings/water/navigation, globe/far collision and appearance are not established. Parent inspects captures.'}
$p=$null;$client=$null;$writer=$null;$reader=$null;$heldKey=$null
$totalDeadline=[DateTime]::UtcNow.AddSeconds(240+3*$PatchTimeoutSeconds)
function Health{
    Require (!$p.HasExited) 'Owned game exited unexpectedly'
    Require ([DateTime]::UtcNow-lt$totalDeadline) 'Overall bounded acceptance deadline'
    if(Test-Path -LiteralPath $log){Require (!(Select-String -LiteralPath $log -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|Critical error|Unbekannter Operator' -Quiet)) 'Runtime/script failure; inspect logs'}
}
function Send($command){
    Health;$wire=$command|ConvertTo-Json -Depth 10 -Compress;$wire|Add-Content -LiteralPath (Join-Path $out 'harness.jsonl');$writer.WriteLine($wire)
    do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected';$line|Add-Content -LiteralPath (Join-Path $out 'harness.jsonl');$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok)
    Require $reply.ok $line;return $reply
}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Ground([double]$x,[double]$z){return GroundData (Eval ('triEarthGround ['+(Fmt $x)+','+(Fmt $z)+']'))}
function Location{
    $value=Eval 'triEarthLocation []'
    Require ($value-match'^OK:latitude=([-+0-9.eE]+) longitude=([-+0-9.eE]+) altitude=([-+0-9.eE]+) revision=([0-9]+)$') 'Malformed geographic origin query'
    return @{latitude=(Num $Matches[1]);longitude=(Num $Matches[2]);altitude=(Num $Matches[3]);revision=[uint64]$Matches[4];raw=$value}
}
function State([string]$label){
    $v=Eval '[getPosASL player,triPlayerGroundClearance,triPlayerTerrainClearance,triPlayerContactGroundClearance,getDir player,getMove player,alive player,vehicle player == player,time]'
    Require ($v.Count-eq9 -and $v[0].Count-eq3) 'Incomplete actual player state'
    $state=@{label=$label;position=$v[0];clearance=(Num $v[1]);terrainClearance=(Num $v[2]);contactClearance=(Num $v[3]);direction=(Num $v[4]);pose=$v[5];alive=$v[6];onFoot=$v[7];time=(Num $v[8]);ground=(Ground (Num $v[0][0]) (Num $v[0][1]))}
    AssertStanding $state;$result.samples+=,$state;return $state
}
function WaitGround([uint64]$revision){
    $until=[DateTime]::UtcNow.AddSeconds($PatchTimeoutSeconds)
    do{
        $ground=Ground 6181.81 6985.71
        Require ($ground.revision-eq$revision) 'Ground diagnostic retained a stale geographic revision'
        if($ground.covered){return $ground}
        Health;Require ([DateTime]::UtcNow-lt$until) 'Resident elevation deadline; no ground acceptance established'
        Start-Sleep -Milliseconds 250
    }while($true)
}
function Capture([string]$label){
    $path=Join-Path $out ($label+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(15)
    while(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length-eq0){Health;Require ([DateTime]::UtcNow-lt$until) 'Screenshot missing';Start-Sleep -Milliseconds 100}
    $result.captures[$label]=$path
}
function CameraClearance{
    $camera=(Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera.position
    Require ($camera.Count-eq3) 'Missing actual rendered camera'
    $ground=Ground (Num $camera[0]) (Num $camera[2])
    Require ($ground.covered) 'Player camera escaped resident near ground'
    $clearance=(Num $camera[1])-$ground.height
    Require ($clearance-gt.05 -and $clearance-lt4) 'Actual first-person camera is buried or floating far above ground'
    return @{positionEngineXYZ=$camera;ground=$ground;clearance=$clearance}
}
function Tap([int]$sc){$null=Send @{cmd='key';sc=$sc;hold=$true};$script:heldKey=$sc;try{Start-Sleep -Milliseconds 180}finally{$null=Send @{cmd='key_up';sc=$sc};$script:heldKey=$null}}
function CentreLook{Exec 'triClearView;triCursorMove [0,0]';Tap 93;Start-Sleep -Milliseconds 250}
function Walk([string]$label,[double]$minimum,[bool]$prone){
    Require ((Num (Eval 'triGetCameraEffectActive'))-eq0) 'Freefly/camera effect owns input'
    $start=State ($label+'-before');Exec 'setAccTime 1';$null=Send @{cmd='key';sc=26;hold=$true};$script:heldKey=26
    $until=[DateTime]::UtcNow.AddSeconds(20);$samples=@()
    try{do{
        Start-Sleep -Milliseconds 200;$sample=State $label;$samples+=,$sample
        Require ($sample.ground.playerDown-eq$prone) 'Actual player posture differs from traversal arm'
        $distance=HorizontalDistance $start.position $sample.position
        Require ($distance-lt25) 'Player escaped bounded traversal'
        Require ([DateTime]::UtcNow-lt$until) 'Ordinary W input did not move actual controlled player'
    }while($distance-lt$minimum)}finally{$null=Send @{cmd='key_up';sc=26};$script:heldKey=$null}
    Start-Sleep -Milliseconds 400;Exec 'setAccTime 0';$end=State ($label+'-after')
    Require ($end.time-gt$start.time+.1) 'Movement arm did not advance real simulation time'
    return @{start=$start;end=$end;distance=$distance;samples=$samples;input='ordinary SDL W; no doMove/switchMove'}
}
try{
    foreach($key in $clear){[Environment]::SetEnvironmentVariable($key,$null,'Process')}
    foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
    $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','4000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','12','--log-file',('"'+$log+'"'))
    $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $argv -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError $stderr -PassThru
    $until=[DateTime]::UtcNow.AddSeconds(150)
    do{
        Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null}
        if(!$client){Require ([DateTime]::UtcNow-lt$until) 'Harness startup deadline';Start-Sleep -Milliseconds 250}
    }while(!$client)
    $stream=$client.GetStream();$stream.ReadTimeout=30000;$stream.WriteTimeout=30000
    $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    while((Eval 'triSceneReady')-cne'OK'){Health;Require ([DateTime]::UtcNow-lt$until) 'Scene-ready deadline';Start-Sleep -Milliseconds 250}
    Exec 'setAccTime 0;player allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0;triDevPanelShow 0;player switchCamera "INTERNAL"'
    Require ((Eval 'typeOf player')-ceq'SoldierWB' -and (Eval 'vehicle player == player')) 'Actual stock on-foot player missing'
    $initialLocation=Location;$result.initialGround=WaitGround $initialLocation.revision
    Require ($result.initialGround.playerContactPoints-gt0) 'Stock player is NoShape or lacks actual land-contact vertices'
    # Exact owner reproduction: third SQF coordinate is AGL, not absolute Y.
    Exec 'player setPos [6181.81,6985.71,0.60];player setDir 283.2;player setUnitPos "UP"'
    $teleport=State 'reported-teleport-paused';$result.teleport=$teleport
    Require ((HorizontalDistance $teleport.position @(6181.81,6985.71,0))-lt.1 -and [Math]::Abs($teleport.clearance-.6)-lt.1) 'Reported teleport did not add actual resident height'
    Require ($teleport.ground.height-gt100 -and $teleport.position[2]-gt100) 'Reported buried-player reproduction used zero/fixture ground'
    Exec 'setAccTime 1';CentreLook;Start-Sleep -Milliseconds 800
    $standing=State 'standing-settled';Require (!$standing.ground.playerDown) 'Initial player is prone'
    $result.walk=Walk 'walk' 4 $false;$result.standingCamera=CameraClearance;Capture 'standing-first'
    Exec 'player switchCamera "EXTERNAL"';Start-Sleep -Milliseconds 350;Capture 'standing-third'
    Exec 'player switchCamera "INTERNAL";setAccTime 1';Tap 29
    $until=[DateTime]::UtcNow.AddSeconds(8)
    do{Start-Sleep -Milliseconds 200;$prone=State 'prone-transition';Require ([DateTime]::UtcNow-lt$until) 'Ordinary Z did not enter prone posture'}while(!$prone.ground.playerDown)
    $result.crawl=Walk 'crawl' 2 $true;$result.proneCamera=CameraClearance;Capture 'prone-first'
    Require ($result.proneCamera.clearance-lt$result.standingCamera.clearance-.25) 'Prone camera did not lower with actual posture'
    Exec 'player switchCamera "EXTERNAL"';Start-Sleep -Milliseconds 350;Capture 'prone-third'
    Exec 'player switchCamera "INTERNAL";setAccTime 1';Tap 29
    $until=[DateTime]::UtcNow.AddSeconds(8)
    do{Start-Sleep -Milliseconds 200;$up=State 'stand-transition';Require ([DateTime]::UtcNow-lt$until) 'Ordinary Z did not leave prone posture'}while($up.ground.playerDown)
    Exec 'setAccTime 0'
    # Pin only the rendered camera at the patch centre. Input remains attached
    # to the real player, preventing camera-driven recenter from moving the edge.
    $bounds=$up.ground.bounds;$midX=($bounds[0]+$bounds[2])/2;$midZ=($bounds[1]+$bounds[3])/2
    $centre=Ground $midX $midZ
    Exec ('triSetView ['+(Fmt $midX)+','+(Fmt ($centre.height+12))+','+(Fmt $midZ)+',1,0,0];player setDir 90;triCursorMove [0,0]')
    Tap 93;Start-Sleep -Milliseconds 350
    $edgeGround=Ground $midX $midZ;$bounds=$edgeGround.bounds;$edgeX=$bounds[2]-8
    Exec ('player setPos ['+(Fmt $edgeX)+','+(Fmt $midZ)+',0];player setDir 90')
    $edgeStart=State 'edge-before';$edgeSamples=@();Exec 'setAccTime 1'
    $null=Send @{cmd='key';sc=26;hold=$true};$heldKey=26
    try{for($i=0;$i-lt20;$i++){
        Start-Sleep -Milliseconds 200;$sample=State 'resident-edge';$edgeSamples+=,$sample
        Require ($sample.ground.generation-eq$edgeGround.generation) 'Resident patch moved during fixed-camera boundary test'
        Require ($sample.position[0]-le$bounds[2]-1.7 -and $sample.position[0]-ge$edgeX-.5) 'Player crossed resident safety edge or moved backwards'
    }}finally{$null=Send @{cmd='key_up';sc=26};$heldKey=$null}
    Exec 'setAccTime 0'
    Require (($edgeSamples[-1].position[0]-$edgeStart.position[0])-gt2) 'Edge arm never exercised actual movement'
    Require ((HorizontalDistance $edgeSamples[-1].position $edgeSamples[-5].position)-lt.4) 'Player did not stop at resident edge'
    $outsideX=$bounds[2]+100;$outside=Ground $outsideX $midZ;Require (!$outside.covered) 'Outside point falsely reports resident coverage'
    $beforeRejected=Eval 'getPosASL player';Exec ('player setPos ['+(Fmt $outsideX)+','+(Fmt $midZ)+',0]');$afterRejected=Eval 'getPosASL player'
    Require ((HorizontalDistance $beforeRejected $afterRejected)-lt.01 -and [Math]::Abs($beforeRejected[2]-$afterRejected[2])-lt.01) 'Uncovered setPos moved player'
    $result.edge=@{start=$edgeStart;samples=$edgeSamples;outside=$outside;beforeRejected=$beforeRejected;afterRejected=$afterRejected;camera='fixed triSetView; gameplay input still real player'}
    Exec 'player setPos [6181.81,6985.71,0];triClearView'
    # In one eval transaction, Go invalidates the ground before another render
    # can publish. Orbit requests no near mesh; stale worker completion must not
    # recreate support for the old geography.
    $old=Location;$playerBeforeGo=Eval 'getPosASL player'
    $invalidated=GroundData (Eval 'triEarthLocation [46.6,8.1,18000000];triEarthGround [6181.81,6985.71]')
    Require (!$invalidated.covered -and $invalidated.generation-eq0 -and $invalidated.revision-eq$old.revision+1) 'Geographic Go did not immediately invalidate old ground'
    Exec 'player setPos [6400,6400,0]';$playerAfterGo=Eval 'getPosASL player'
    Require ((HorizontalDistance $playerBeforeGo $playerAfterGo)-lt.01) 'Geographic Go or unsupported teleport relocated player'
    Start-Sleep -Seconds 2;$orbitGround=Ground 6181.81 6985.71;Require (!$orbitGround.covered -and $orbitGround.generation-eq0) 'Stale near patch reappeared in orbital revision'
    $rapid=@();foreach($code in @('triEarthLocation [36.1,-112.1,1000]','triEarthLocation [46.6,8.1,1000]','triEarthLocation [49.61,6.13,1000]')){
        Require ((Eval $code).StartsWith('OK:')) 'Rapid geographic Go refused';$rapid+=,(Location)
    }
    $final=Location;$finalGround=WaitGround $final.revision
    Require ($final.revision-eq$old.revision+4 -and $finalGround.generation-gt0) 'Rapid relocation revision count/publication differs'
    $rebases=@(Select-String -LiteralPath $log -Pattern ('Earth relocated: .*revision '+$final.revision+'$'))
    Require ($rebases.Count-gt0) 'Final renderer rebase receipt missing'
    $patches=@(Select-String -LiteralPath $log -Pattern 'Earth terrain patch ')
    foreach($row in $patches|Where-Object{$_.LineNumber-gt$rebases[-1].LineNumber}){Require ($row.Line-match ('revision '+$final.revision+'$')) 'Superseded geographic patch published after final rebase'}
    $result.relocation=@{before=$old;invalidated=$invalidated;orbit=$orbitGround;rapid=$rapid;final=$final;ground=$finalGround;playerBefore=$playerBeforeGo;playerAfter=$playerAfterGo}
    Capture 'final-geographic-preview'
    Require (!(Select-String -LiteralPath $stderr -Pattern 'Validation Error|panicked at|DeviceLost|wgr_create failed' -Quiet)) 'Renderer stderr failure'
    $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)) 'Owned game did not quit normally'
    Require ($p.ExitCode-eq0) 'Owned game exit was nonzero';$result.exitCode=$p.ExitCode
    $result.passed=$true;Write-Host ('Earth resident ground installed acceptance PASS; inspect captures: '+$out)
}catch{$result.error=$_.Exception.Message;throw}finally{
    if($p-and!$p.HasExited){
        if($writer){try{if($null-ne$heldKey){$writer.WriteLine((@{cmd='key_up';sc=$heldKey}|ConvertTo-Json -Compress))};$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}}
        if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000);$result.forcedCleanup=$true}
    }
    if($client){$client.Dispose()}
    foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    try{$after=Pair;$result.after=$after;Require ($before.stamp-ceq$after.stamp -and $before.exe-ceq$after.exe -and $before.dll-ceq$after.dll) 'Installed pair changed during acceptance'}
    catch{$result.passed=$false;$result.pairError=$_.Exception.Message;throw}
    finally{$result|ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $out 'result.json')}
}
