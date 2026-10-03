# Installed original Everon trench smoke. Runtime/deployment belongs to root.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='original-everon-trenches',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
)
$ErrorActionPreference='Stop';$taskRoot=Split-Path -Parent $PSScriptRoot;$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Num($value){$n=[double]::Parse([string]$value,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n)-and ![double]::IsInfinity($n)) 'Nonfinite fixture value.';return $n}
function Fmt($value){return (Num $value).ToString('R',$culture)}
function Decode([string]$value){$value=$value.Trim();if($value.StartsWith('"')){Require $value.EndsWith('"') 'Incomplete SQF string.';return $value.Substring(1,$value.Length-2).Replace('""','"')};return ConvertFrom-Json $value -NoEnumerate}
function Pair{
 $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw;Require ($stamp-match [regex]::Escape($ExpectedCommit)) 'Installed commit differs.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$f=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $f.FullName).Hash;bytes=$f.Length;writtenUtc=$f.LastWriteTimeUtc.ToString('o')}})}
}
Require ([bool]$env:LOCK_OWNER) 'Invoke through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved.'
$before=Pair;$taskOut=Join-Path $taskRoot ('build/trench-editor/'+$Label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$profile=Join-Path $taskOut 'user';New-Item -ItemType Directory -Force $profile|Out-Null
$settings=@{POSEIDON_USER_DIR=$profile;WGR_GRASS='0';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0';WGR_TEMPORAL='0'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE')
$saved=@{};foreach($key in @($settings.Keys)+$clear){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$log=Join-Path $taskOut 'engine.log';$p=$null;$client=$null;$heldKey=$null;$tankOwned=$false
$result=[ordered]@{passed=$false;installed=$before;expectedCommit=$ExpectedCommit;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;scope='Mission-local infantry and tank trenches on original retail Everon without addon. Actual ramp/floor/support, retained source heights, ordinary W/S traversal and undo. No AI routing, saving, Box3D cavities or complete tank hull contact certification claimed.'}
function Send($command){
 Require (!$p.HasExited) 'Owned game exited unexpectedly.';$wire=$command|ConvertTo-Json -Depth 10 -Compress;$wire|Add-Content -LiteralPath (Join-Path $taskOut 'harness.jsonl');$writer.WriteLine($wire)
 do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected.';$reply=$line|ConvertFrom-Json}while($null-eq $reply.ok)
 $line|Add-Content -LiteralPath (Join-Path $taskOut 'harness.jsonl');Require $reply.ok $line;return $reply
}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function CentreLook{
 $receipt=@{beforeDir=(Num (Eval 'getDir vehicle player'));beforeCursor=(Eval 'triCursorPos')}
 Exec 'triClearView;triCursorMove [0,0]';Start-Sleep -Milliseconds 250
 $null=Send @{cmd='key';sc=93;hold=$true};try{Start-Sleep -Milliseconds 350}finally{$null=Send @{cmd='key_up';sc=93}}
 Start-Sleep -Milliseconds 250;$receipt.afterDir=Num (Eval 'getDir vehicle player');$receipt.afterCursor=Eval 'triCursorPos';return $receipt
}
function HeadingError([double]$actual,[double]$wanted){return [Math]::Abs((($actual-$wanted+540)%360)-180)}
function Point($site,[double]$along,[double]$side,[double]$y){return @{x=($site.x+$site.fx*$along+$site.sx*$side);z=($site.z+$site.fz*$along+$site.sz*$side);y=$y}}
function State($point,$ray){
 $query=@{cmd='dev_cave_editor';action='state'};if($point){foreach($k in @('x','y','z')){$query[$k]=Num $point[$k]}};if($ray){$query.rayFromY=Num $ray[0];$query.rayToY=Num $ray[1]}
 $s=Send $query;Require ($s.worldName.Replace('\','/').ToLowerInvariant()-match '(^|/)eden\.wrp$') 'Not the actual original Everon landscape.';Require ((Num $s.grid)-gt 0) 'Missing grid identity.';return $s
}
function Heights($points){$terms=@($points|ForEach-Object{'triTerrainHeight ['+(Fmt $_.x)+','+(Fmt $_.z)+']'});$values=Eval ('['+($terms-join ',')+']');Require ($values.Count-eq $points.Count) 'Incomplete height readbacks.';return @($values|ForEach-Object{Num $_})}
function AssertUnchanged($baseline,$actual){Require ($actual.heightRevision-eq $baseline.heightRevision-and $actual.worldName-ceq $baseline.worldName-and $actual.grid-eq $baseline.grid) 'Trench changed height identity.';Require ([Math]::Abs((Num $actual.terrainY)-(Num $baseline.terrainY))-lt .0001) 'Raw terrain height changed.'}
function Capture([string]$name,$position,$direction){
 if($position){Exec ('triSetView ['+((@($position)+@($direction)|ForEach-Object{Fmt $_})-join ',')+']')};Start-Sleep -Milliseconds 400
 $path=Join-Path $taskOut ($name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(15)
 while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow-lt $until) 'Screenshot not written.';Start-Sleep -Milliseconds 150}
 $result[$name]=@{path=$path;positionXYZ=$position;directionXYZ=$direction};if($position){Exec 'triClearView'}
}
function Traverse($site,$preset,[bool]$inward,$arm,[double]$startOriginY){
 $isTank=$preset.tank;$key=26;if(!$inward-and $isTank){$key=22}
 $heading=$site.heading;if(!$inward-and !$isTank){$heading+=180}
 if(!$isTank){Exec ('player setDir '+(Fmt $heading)+';player switchCamera "INTERNAL"')}
 $arm.look=CentreLook;Require ((HeadingError (Num (Eval 'getDir vehicle player')) $heading)-lt 5) 'Ordinary LookCenter did not align actor before trench traversal.'
 Require ((Num (Eval 'triGetCameraEffectActive'))-eq 0) 'Camera effect prevents controlled input.'
 $arm.inputContext=Eval 'triGetInputContext';Exec 'setAccTime 1';$null=Send @{cmd='key';sc=$key;hold=$true};$script:heldKey=$key
 $samples=@();$done=$false;$until=[DateTime]::UtcNow.AddSeconds(25)
 try{do{
  Start-Sleep -Milliseconds 180;$position=Eval 'getPosASL vehicle player';Require ($position.Count-eq 3-and (Eval 'alive player')) 'Controlled actor unavailable.'
  if($isTank){Require ((Eval 'typeOf vehicle player')-ceq 'M1Abrams'-and (Eval 'driver opTrenchTank == player')-and (Eval 'alive opTrenchTank')) 'Actual live Abrams driver control lost.'}
  else{Require ((Eval 'vehicle player == player')-and (Eval 'typeOf player')-ceq 'SoldierWB') 'Infantry control changed.'}
  $along=($position[0]-$site.x)*$site.fx+($position[1]-$site.z)*$site.fz;$side=($position[0]-$site.x)*$site.sx+($position[1]-$site.z)*$site.sz
  Require ([Math]::Abs($side)-lt ($preset.width*.5+1)-and $along-gt -14-and $along-lt ($preset.length-1)) 'Controlled actor escaped bounded trench lane.'
  $sample=State @{x=$position[0];z=$position[1];y=($position[2]+.05)} $null
  $originDrop=$startOriginY-(Num $position[2]);$samples+=@{position=$position;along=$along;side=$side;originDrop=$originDrop;speed=(Num (Eval 'speed vehicle player'));dir=(Num (Eval 'getDir vehicle player'));cursor=(Eval 'triCursorPos');state=$sample}
  if($inward){$done=$along-ge ($preset.ramp+2)-and $sample.atHeight-and $originDrop-gt ($preset.depth*.6)-and [Math]::Abs((Num $sample.roadY)-($site.rim-$preset.depth))-lt .12}
  else{$done=$along-lt -1-and !$sample.inFootprint}
 }while(!$done-and [DateTime]::UtcNow-lt $until)}finally{$null=Send @{cmd='key_up';sc=$key};$script:heldKey=$null;Exec 'setAccTime 0';$arm.samples=$samples;$arm.completed=$done}
 Require $done ('Actual '+$preset.name+' '+$(if($inward){'descent'}else{'exit'})+' did not complete within25s.')
}
try{
 foreach($key in $clear){[Environment]::SetEnvironmentVariable($key,$null,'Process')};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $mission=Join-Path $taskRoot 'tests/perf/missions/perf_field.eden';$args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $args -PassThru
 $until=[DateTime]::UtcNow.AddSeconds(120)
 do{Require (!$p.HasExited) 'Startup exited.';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt $until) 'Startup deadline.';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 $until=[DateTime]::UtcNow.AddSeconds(120);while((Eval 'triSceneReady')-cne 'OK'){Require ([DateTime]::UtcNow-lt $until) 'Scene-ready deadline.';Start-Sleep -Milliseconds 300}
 Exec 'setAccTime 0;0 setRain 0;0 setOvercast 0';$initial=State $null $null;Require ($initial.count-eq 0) 'Fresh world already contains editor objects.';$result.initial=$initial
 $terrain=Send @{cmd='dev_terrain_brush';action='state'};$result.terrain=$terrain
 $scan=Eval 'private ["_rows","_x","_z"];_rows=[];for "_z" from 4000 to 8000 step 250 do {for "_x" from 4000 to 8000 step 250 do {_rows set [count _rows,[_x,_z,triTerrainHeight [_x,_z]]]}};_rows'
 Require ($scan.Count-eq 289) 'Incomplete flat-site census.';$result.scan=$scan;$lookup=@{}
 foreach($row in $scan){Require ($row.Count-eq 3) 'Malformed terrain census.';$lookup[([string]$row[0]+':'+[string]$row[1])]=Num $row[2]}
 $candidates=@();foreach($row in $scan){if((Num $row[2])-le (Num $terrain.seaLevel)+.5){continue};$variation=0.;$available=0
  foreach($dir in @(@{dx=0;dz=250},@{dx=250;dz=0},@{dx=0;dz=-250},@{dx=-250;dz=0})){$key=[string]($row[0]+$dir.dx)+':'+[string]($row[1]+$dir.dz);if($lookup.ContainsKey($key)){$variation+=[Math]::Abs($lookup[$key]-(Num $row[2]));++$available}}
  if($available-ge 2){$candidates+=@{x=(Num $row[0]);z=(Num $row[1]);variation=($variation/$available)}}
 }
 $site=$null;$attempts=@();$until=[DateTime]::UtcNow.AddSeconds(120)
 foreach($candidate in @($candidates|Sort-Object variation|Select-Object -First 32)){
  Require ([DateTime]::UtcNow-lt $until) 'Bounded flat-site refinement deadline.'
  foreach($heading in @(0,90)){
   $a=$heading*[Math]::PI/180;$candidate.heading=$heading;$candidate.fx=[Math]::Sin($a);$candidate.fz=[Math]::Cos($a);$candidate.sx=[Math]::Cos($a);$candidate.sz=-[Math]::Sin($a)
   $points=@();for($s=0;$s-le 24;$s+=1){foreach($side in @(-2.9,0,2.9)){$points+=,(Point $candidate $s $side 0)}}
   $heights=Heights $points;$minimum=($heights|Measure-Object -Minimum).Minimum;$maximum=($heights|Measure-Object -Maximum).Maximum;$rim=($minimum+$maximum)*.5
   $clearance=State (Point $candidate 12 0 $rim) $null;$admitted=$maximum-$minimum-le .4-and $rim-gt (Num $terrain.seaLevel)+2.5-and !$clearance.inFootprint
   $attempts+=@{candidate=@{}+$candidate;heights=$heights;min=$minimum;max=$maximum;admitted=$admitted}
   if($admitted){$candidate.rim=$rim;$site=@{}+$candidate;break}
  };if($site){break}
 };$result.refinement=$attempts;Require ([bool]$site) 'No flat dry source admitted in bounded Everon census.';$result.site=$site
 foreach($preset in @(@{name='infantry';tank=$false;width=1.5;depth=1.6;length=12;ramp=4.8},@{name='tank';tank=$true;width=5;depth=2;length=24;ramp=12})){
  $arm=[ordered]@{preset=$preset};$result[$preset.name]=$arm;$floor=$site.rim-$preset.depth;$mid=$preset.ramp+2.5;$point=Point $site $mid 0 ($site.rim+.2)
  $baseline=State $point @(($site.rim+1),($floor+.1));Require (!$baseline.inFootprint-and $baseline.terrainRayHit) 'Original trench column lacks actual terrain intersection.';$arm.baseline=$baseline
  Capture ($preset.name+'-baseline-top') @($point.x,($site.rim+10),$point.z) @(0,-1,.01)
  $created=Send @{cmd='dev_cave_editor';action='trench';x=$site.x;y=$site.rim;z=$site.z;heading=$site.heading;width=$preset.width;depth=$preset.depth;length=$preset.length;tank=$preset.tank};Require ($created.count-eq 1) 'Trench owner count differs.';$arm.created=$created
  $inside=State $point @(($site.rim+1),($floor+.1));AssertUnchanged $baseline $inside
  Require ($inside.inFootprint-and $inside.atHeight-and !$inside.terrainRayHit-and [Math]::Abs((Num $inside.roadY)-$floor)-lt .1) 'Open trench column or actual roadway floor failed.';$arm.inside=$inside
  $ramp=State (Point $site ($preset.ramp*.5) 0 ($site.rim+.2)) $null;Require ([Math]::Abs((Num $ramp.roadY)-($site.rim-$preset.depth*.5))-lt .12) 'Actual ramp roadway differs from intended midpoint.';$arm.ramp=$ramp
  $camera=State (Point $site $mid 0 ($floor+.5)) $null;Require ([Math]::Abs((Num $camera.cameraFloorY)-$floor)-lt .1) 'Trench camera floor differs.';$arm.camera=$camera
  $outside=State (Point $site $mid ($preset.width*.5+.5) ($site.rim+.2)) $null;Require (!$outside.inFootprint) 'Trench widened outside authored footprint.';$arm.sideControl=$outside
  Capture ($preset.name+'-top') @($point.x,($site.rim+10),$point.z) @(0,-1,.01)
  Capture ($preset.name+'-inside') @($point.x,($floor+.8),$point.z) @(-$site.fx,0,-$site.fz)
  $startAlong=-3;if($preset.tank){$startAlong=-9};$start=Point $site $startAlong 0 $site.rim
  Exec ('triClearView;player setPos ['+(Fmt $start.x)+','+(Fmt $start.z)+',0];player setDir '+(Fmt $site.heading)+';player setUnitPos "UP";player switchCamera "INTERNAL"')
  if($preset.tank){
   Exec ('opTrenchTank="M1Abrams" createVehicle ['+(Fmt $start.x)+','+(Fmt $start.z)+',0];opTrenchTank setPos ['+(Fmt $start.x)+','+(Fmt $start.z)+',0];opTrenchTank setDir '+(Fmt $site.heading)+';player moveInDriver opTrenchTank;opTrenchTank setFuel 1;opTrenchTank engineOn true;opTrenchTank switchCamera "EXTERNAL"');$tankOwned=$true
   Require (!(Eval 'isNull opTrenchTank')-and (Eval 'typeOf opTrenchTank')-ceq 'M1Abrams'-and (Eval 'driver opTrenchTank == player')) 'Stock Abrams spawn/driver admission failed.'
  }
  $arm.startLook=CentreLook;Exec 'setAccTime 1';Start-Sleep -Milliseconds 800;Exec 'setAccTime 0';$arm.start=Eval 'getPosASL vehicle player';$arm.startDir=Num (Eval 'getDir vehicle player')
  Require ((HeadingError $arm.startDir $site.heading)-lt 5) 'Actor heading drifted during start settle.'
  $descent=[ordered]@{};$arm.descent=$descent;Traverse $site $preset $true $descent (Num $arm.start[2]);Capture ($preset.name+'-actor-inside') $null $null
  $exit=[ordered]@{};$arm.exit=$exit;Traverse $site $preset $false $exit (Num $arm.start[2])
  if($tankOwned){Exec 'player action ["GETOUT",opTrenchTank]';Exec 'setAccTime 1';Start-Sleep -Seconds 1;Exec 'setAccTime 0';Require (Eval 'vehicle player == player') 'Driver did not leave tank outside trench.';Exec 'deleteVehicle opTrenchTank';Require (Eval 'isNull opTrenchTank') 'Owned tank cleanup failed.';$tankOwned=$false}
  $undo=Send @{cmd='dev_cave_editor';action='undo'};$restored=State $point @(($site.rim+1),($floor+.1));AssertUnchanged $baseline $restored
  Require ($undo.count-eq 0-and !$restored.inFootprint-and $restored.terrainRayHit-and [Math]::Abs((Num $restored.roadY)-(Num $baseline.roadY))-lt .001) 'Trench undo failed actual terrain/support restoration.';$arm.undo=@{owner=$undo;restored=$restored};$arm.passed=$true
  Capture ($preset.name+'-restored-top') @($point.x,($site.rim+10),$point.z) @(0,-1,.01)
 }
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(30000)-and $p.ExitCode-eq 0) 'Normal exit failed.';$after=Pair
 Require (($after|ConvertTo-Json -Depth 6 -Compress)-ceq ($before|ConvertTo-Json -Depth 6 -Compress)) 'Installed pair changed during smoke.'
 Require (!(Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet)) 'Runtime failure logged.';$result.exit=$p.ExitCode;$result.passed=$true;Write-Host "Installed original Everon trench smoke PASS: $taskOut"
}catch{$result.error=$_.Exception.Message;throw}finally{
 if($p-and !$p.HasExited){try{if($client){if($null-ne $heldKey){$null=Send @{cmd='key_up';sc=$heldKey}};if($tankOwned){Exec 'deleteVehicle opTrenchTank'};$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(10000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')};$result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
}
