# Installed original Everon, actual mission-local cave owner and controlled stock soldier.
# Root runtime owner invokes through with-game-lock.sh after deploying a matched pair.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='original-everon-cave',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [switch]$SelectionChecks
)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Num($value){$n=[double]::Parse([string]$value,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n) -and ![double]::IsInfinity($n)) 'Nonfinite fixture value.';return $n}
function Fmt($value){return (Num $value).ToString('R',$culture)}
function Decode([string]$value){$value=$value.Trim();if($value.StartsWith('"')){Require $value.EndsWith('"') 'Incomplete SQF string.';return $value.Substring(1,$value.Length-2).Replace('""','"')};return ConvertFrom-Json $value -NoEnumerate}
function Pair{
 $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw
 Require ($stamp -match [regex]::Escape($ExpectedCommit)) 'Installed commit differs.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{ $file=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash;bytes=$file.Length;writtenUtc=$file.LastWriteTimeUtc.ToString('o')}})}
}
Require ([bool]$env:LOCK_OWNER) 'Invoke through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved.'
$before=Pair
$taskOut=Join-Path $taskRoot ('build/cave-editor/'+$Label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$profile=Join-Path $taskOut 'user';New-Item -ItemType Directory -Force $profile|Out-Null
$settings=@{POSEIDON_USER_DIR=$profile;WGR_GRASS='0';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0';WGR_TEMPORAL='0'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE')
$saved=@{};foreach($key in @($settings.Keys)+$clear){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$log=Join-Path $taskOut 'engine.log';$p=$null;$client=$null;$held=$false
$result=[ordered]@{passed=$false;expectedCommit=$ExpectedCommit;installed=$before;lookPreparations=@();scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;scope='Original retail Everon without addon fixture. Actual bounded cave queries and terrain-only legacy rays, stock W-key traversal, roof controls and sub-grid aperture. Screenshots require runtime owner inspection; no Box3D cavity, AI navigation, persistence or general terrain ray equivalence claimed.'}
function Send($command,[bool]$allowError=$false){
 Require (!$p.HasExited) 'Owned game exited unexpectedly.'
 $wire=$command|ConvertTo-Json -Depth 12 -Compress;$wire|Add-Content -LiteralPath (Join-Path $taskOut 'harness.jsonl');$writer.WriteLine($wire)
 do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected.';$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
 $line|Add-Content -LiteralPath (Join-Path $taskOut 'harness.jsonl');Require ($allowError -or $reply.ok) $line;return $reply
}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function CentreLook{
 # KP5 is the fresh profile's ordinary LookCenter action. Unlike setting the
 # screen cursor alone, it resets the retained virtual-camera/UI aim direction.
 $receipt=@{beforeDir=(Num (Eval 'getDir player'));beforeCursor=(Eval 'triCursorPos')}
 Exec 'triClearView;triCursorMove [0,0]';Start-Sleep -Milliseconds 250
 $null=Send @{cmd='key';sc=93;hold=$true};try{Start-Sleep -Milliseconds 350}finally{$null=Send @{cmd='key_up';sc=93}}
 Start-Sleep -Milliseconds 250;$receipt.afterDir=Num (Eval 'getDir player');$receipt.afterCursor=Eval 'triCursorPos';return $receipt
}
function HeadingError([double]$actual,[double]$wanted){return [Math]::Abs((($actual-$wanted+540)%360)-180)}
function Point($site,[double]$along,[double]$side,[double]$y){return @{x=($site.x+$site.fx*$along+$site.sx*$side);z=($site.z+$site.fz*$along+$site.sz*$side);y=$y}}
function State($point,$ray){
 $query=@{cmd='dev_cave_editor';action='state'};if($point){foreach($k in @('x','y','z')){$query[$k]=Num $point[$k]}}
 if($ray){$query.rayFromY=Num $ray[0];$query.rayToY=Num $ray[1]}
 $state=Send $query;Require ($state.worldName.Replace('\','/').ToLowerInvariant() -match '(^|/)eden\.wrp$') 'Not the actual original Everon landscape.'
 Require ((Num $state.grid) -gt 0) 'Missing terrain grid.';return $state
}
function Heights($points){
 $terms=@($points|ForEach-Object{'triTerrainHeight ['+(Fmt $_.x)+','+(Fmt $_.z)+']'})
 $values=Eval ('['+($terms -join ',')+']');Require ($values.Count -eq $points.Count) 'Incomplete actual height readbacks.'
 return @($values|ForEach-Object{Num $_})
}
function PlaceOutside($site){
 $point=Point $site -2 0 $site.floor
 Exec ('triClearView;setAccTime 0;player setPos ['+(Fmt $point.x)+','+(Fmt $point.z)+',0];player setDir '+(Fmt $site.heading)+';player setUnitPos "UP";player switchCamera "INTERNAL"')
 $look=CentreLook
 Exec 'setAccTime 1';Start-Sleep -Milliseconds 800;Exec 'setAccTime 0'
 $look.settledDir=Num (Eval 'getDir player');$look.requestedDir=$site.heading;$result.lookPreparations+=,$look
 Require ((HeadingError $look.settledDir $site.heading) -lt 5) 'Ordinary LookCenter did not align the player before cave traversal.'
 Require ((Eval 'typeOf player') -ceq 'SoldierWB' -and (Eval 'alive player')) 'Stock live US soldier unavailable.'
 Require ((Num (Eval 'triGetCameraEffectActive')) -eq 0) 'Camera effect suspends gameplay input.'
 return Eval 'getPosASL player'
}
function Capture([string]$name,$position,$direction){
 if($position){Exec ('triSetView ['+((@($position)+@($direction)|ForEach-Object{Fmt $_}) -join ',')+']')}
 Start-Sleep -Milliseconds 400;$path=Join-Path $taskOut ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
 $until=[DateTime]::UtcNow.AddSeconds(15);while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow -lt $until) 'Screenshot not written.';Start-Sleep -Milliseconds 150}
$result[$name]=@{path=$path;positionXYZ=$position;directionXYZ=$direction}
 if($position){Exec 'triClearView'}
}
function AssertUnchanged($baseline,$actual){
 Require ($actual.heightRevision -eq $baseline.heightRevision -and $actual.grid -eq $baseline.grid -and $actual.worldName -ceq $baseline.worldName) 'Cave modified height source identity.'
 Require ([Math]::Abs((Num $actual.terrainY)-(Num $baseline.terrainY)) -lt .0001) 'Raw terrain roof changed.'
}
try{
 foreach($key in $clear){[Environment]::SetEnvironmentVariable($key,$null,'Process')};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $mission=Join-Path $taskRoot 'tests/perf/missions/perf_field.eden'
 $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $args -PassThru
 $until=[DateTime]::UtcNow.AddSeconds(120)
 do{Require (!$p.HasExited) 'Startup exited.';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Startup deadline.';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 $until=[DateTime]::UtcNow.AddSeconds(120);while((Eval 'triSceneReady') -cne 'OK'){Require ([DateTime]::UtcNow -lt $until) 'Scene-ready deadline.';Start-Sleep -Milliseconds 300}
 Exec 'setAccTime 0;0 setRain 0;0 setOvercast 0'
 $initial=State $null $null;Require ($initial.count -eq 0) 'Fresh mission already has editor caves.';$result.initial=$initial
 # One bounded read-only census; refine at most 32 actual rising source slopes.
 $scan=Eval 'private ["_rows","_x","_z"];_rows=[];for "_z" from 4000 to 8000 step 250 do {for "_x" from 4000 to 8000 step 250 do {_rows set [count _rows,[_x,_z,triTerrainHeight [_x,_z]]]}};_rows'
 Require ($scan.Count -eq 289) 'Incomplete Everon hillside census.';$result.scan=$scan
 $lookup=@{};foreach($row in $scan){Require ($row.Count -eq 3) 'Malformed height census.';$lookup[([string]$row[0]+':'+[string]$row[1])]=Num $row[2]}
 $candidates=@();foreach($row in $scan){
  if((Num $row[2]) -lt 20){continue}
  foreach($dir in @(@{dx=0;dz=250;heading=0},@{dx=250;dz=0;heading=90},@{dx=0;dz=-250;heading=180},@{dx=-250;dz=0;heading=270})){
   $key=[string]($row[0]+$dir.dx)+':'+[string]($row[1]+$dir.dz)
   if($lookup.ContainsKey($key)){$gain=$lookup[$key]-(Num $row[2]);if($gain -gt 25){$candidates+=@{x=(Num $row[0]);z=(Num $row[1]);heading=$dir.heading;gain=$gain}}}
  }
 }
 $attempts=@();$site=$null;$until=[DateTime]::UtcNow.AddSeconds(120)
 foreach($candidate in @($candidates|Sort-Object gain -Descending|Select-Object -First 32)){
  Require ([DateTime]::UtcNow -lt $until) 'Bounded hillside refinement deadline.'
  $a=$candidate.heading*[Math]::PI/180;$candidate.fx=[Math]::Sin($a);$candidate.fz=[Math]::Cos($a);$candidate.sx=[Math]::Cos($a);$candidate.sz=-[Math]::Sin($a)
  $path=@();for($s=0;$s -le 32;$s+=2){$path+=,(Point $candidate $s 0 0)}
  $heights=Heights $path;$floor=$heights[0];$covered=$heights[-1] -ge $floor+2.9 -and $heights[8] -ge $floor+2.9;$supported=@($heights|Where-Object{$_ -lt $floor-.2}).Count -eq 0
  $attempts+=@{candidate=$candidate;heights=$heights;coveredBack=$covered;supportedFloor=$supported}
  if($covered -and $supported){$candidate.floor=$floor;$site=$candidate;break}
 }
 $result.refinement=$attempts;Require ([bool]$site) 'No actual source hill admitted in bounded census.';$result.site=$site
 $centre=Point $site 16 0 ($site.floor+1.2);$baseline=State $centre $null;$result.baseline=$baseline
 Require (!$baseline.inFootprint) 'Chosen site overlaps an existing authored hole.'
 $roofRay=@(($baseline.terrainY+1),($baseline.terrainY-.25));$roofBefore=State $centre $roofRay
 Require $roofBefore.terrainRayHit 'Original roof column does not hit actual legacy terrain.'
 Capture 'baseline-above' @($centre.x,($baseline.terrainY+6),$centre.z) @(0,-1,.01)
 $normal=Send @{cmd='dev_cave_editor';action='create';x=$site.x;y=$site.floor;z=$site.z;heading=$site.heading;width=3;height=2.5;length=32}
 Require ($normal.count -eq 1) 'Normal cave count differs.'
 if($SelectionChecks){
  $normalId=Num $normal.selectedId;Require ($normalId -gt 0 -and @($normal.records).Count -eq 1) 'Missing stable cave selection receipt.'
  $selected=Send @{cmd='dev_cave_editor';action='select';id=$normalId};Require ($selected.selectedId -eq $normalId) 'Cave selection differs.'
  $rotated=Send @{cmd='dev_cave_editor';action='rotate';id=$normalId;heading=($site.heading+.1)}
  Require ($rotated.count -eq 1 -and $rotated.records[0].id -eq $normalId -and $rotated.records[0].objectId -eq $normal.records[0].objectId -and $rotated.records[0].sourceName -ceq $normal.records[0].sourceName -and [Math]::Abs($rotated.records[0].heading-($site.heading+.1)) -lt .001) 'Cave rotation changed identity or failed to apply.'
  $back=Send @{cmd='dev_cave_editor';action='rotate';heading=$site.heading};Require ([Math]::Abs($back.records[0].heading-$site.heading) -lt .001) 'Selected cave did not rotate back.'
  $result.selection=@{selected=$selected;rotated=$rotated;returned=$back}
 }
 $inside=State $centre @(($site.floor+2.1),($site.floor+.2));AssertUnchanged $baseline $inside
 Require ($inside.inFootprint -and $inside.atHeight -and $inside.boundedHeader -and !$inside.terrainRayHit) 'Normal aperture/height/header/legacy intersection proof failed.'
 Require ([Math]::Abs((Num $inside.roadY)-$site.floor) -lt .1 -and [Math]::Abs((Num $inside.cameraFloorY)-$site.floor) -lt .1) 'Inside gameplay/camera floor differs from cave floor.'
 $roof=State @{x=$centre.x;z=$centre.z;y=($baseline.terrainY+.2)} $roofRay;AssertUnchanged $baseline $roof
 Require (!$roof.atHeight -and $roof.terrainRayHit -and [Math]::Abs((Num $roof.roadY)-(Num $baseline.terrainY)) -lt .1 -and [Math]::Abs((Num $roof.terrainRayY)-(Num $roofBefore.terrainRayY)) -lt .001) 'Above-cave roof collision/support changed.'
 $result.normal=@{created=$normal;inside=$inside;roofBefore=$roofBefore;roof=$roof}
 $entranceView=Point $site -6 0 ($site.floor+1.5);Capture 'normal-entrance' @($entranceView.x,$entranceView.y,$entranceView.z) @($site.fx,0,$site.fz)
 Capture 'normal-inside' @($centre.x,($site.floor+1.5),$centre.z) @($site.fx,0,$site.fz)
 Capture 'normal-above' @($centre.x,($baseline.terrainY+6),$centre.z) @(0,-1,.01)
 $start=PlaceOutside $site;$samples=@();$input=Eval 'triGetInputContext';Exec 'setAccTime 1';$null=Send @{cmd='key';sc=26;hold=$true};$held=$true
 $until=[DateTime]::UtcNow.AddSeconds(15);$entered=$false
 do{
  Start-Sleep -Milliseconds 180;$position=Eval 'getPosASL player';Require ($position.Count -eq 3 -and (Eval 'alive player')) 'Player disappeared or died during cave traversal.'
  $along=($position[0]-$site.x)*$site.fx+($position[1]-$site.z)*$site.fz;$side=($position[0]-$site.x)*$site.sx+($position[1]-$site.z)*$site.sz
  Require ([Math]::Abs($side) -lt 4 -and $along -gt -8 -and $along -lt 24) 'Controlled player escaped bounded lane.'
  $probe=State @{x=$position[0];z=$position[1];y=($position[2]+.2)} $null;$samples+=@{position=$position;along=$along;side=$side;dir=(Num (Eval 'getDir player'));cursor=(Eval 'triCursorPos');state=$probe}
  $entered=$along -ge 6 -and $probe.atHeight -and $position[2]+1 -lt $probe.terrainY
 }while(!$entered -and [DateTime]::UtcNow -lt $until)
 $null=Send @{cmd='key_up';sc=26};$held=$false;Exec 'setAccTime 0'
 $result.traversal=@{start=$start;inputContext=$input;samples=$samples;entered=$entered};Require $entered 'W-key traversal did not enter below retained roof.'
 if($SelectionChecks){
  $beforeEdit=State $null $null
  $refusedDelete=Send @{cmd='dev_cave_editor';action='delete';id=$normalId} $true
  $refusedRotate=Send @{cmd='dev_cave_editor';action='rotate';id=$normalId;heading=($site.heading+.1)} $true
  Require (!$refusedDelete.ok -and !$refusedRotate.ok) 'Occupied cave accepted delete or rotation.'
  $afterEdit=State $null $null;Require (($beforeEdit.records|ConvertTo-Json -Depth 6 -Compress) -ceq ($afterEdit.records|ConvertTo-Json -Depth 6 -Compress)) 'Refused occupied edits changed cave records.'
  $result.occupiedEditGuard=@{delete=$refusedDelete;rotate=$refusedRotate;unchanged=$true}
 }
 Capture 'player-inside-first' $null $null
 # Real L/Headlights input enables the soldier's torch even in a daytime cave.
 Exec 'setAccTime 1';$null=Send @{cmd='key';sc=15;hold=$true}
 try{Start-Sleep -Milliseconds 350}finally{$null=Send @{cmd='key_up';sc=15}}
 Start-Sleep -Milliseconds 350;Exec 'setAccTime 0'
 $keyLightLogged=[bool](Select-String -LiteralPath $log -Pattern 'Flashlight on:.*player=1' -Quiet)
 if(!$keyLightLogged){
  # Also exercise the normal Light on action if injected keyboard input did
  # not activate. The registered action name contains a space.
  Exec 'player action ["LIGHT ON",player];setAccTime 1';Start-Sleep -Milliseconds 700;Exec 'setAccTime 0'
 }
 Capture 'player-inside-flashlight' $null $null
 Require ([bool](Select-String -LiteralPath $log -Pattern 'Flashlight on:.*player=1' -Quiet)) 'Neither L nor the ordinary Light on action activated the player flashlight inside the cave.'
 $result.flashlight=@{input='L / Headlights, ordinary Light on action if keyboard did not activate';daytime=$true;keyActivationLogged=$keyLightLogged;activationLogged=$true}
 Exec 'player switchCamera "EXTERNAL"';Capture 'player-inside-third' $null $null
 # Return through the entrance using ordinary gameplay input before closing it.
 Exec ('player switchCamera "INTERNAL";player setDir '+(Fmt ($site.heading+180))+';setAccTime 0')
 $result.exitLook=CentreLook;Require ((HeadingError (Num (Eval 'getDir player')) ($site.heading+180)) -lt 5) 'Exit heading was not aligned before W input.';Exec 'setAccTime 1'
 $null=Send @{cmd='key';sc=26;hold=$true};$held=$true;$returnSamples=@();$left=$false;$until=[DateTime]::UtcNow.AddSeconds(15)
 do{
  Start-Sleep -Milliseconds 180;$position=Eval 'getPosASL player';Require ((Eval 'alive player') -and $position.Count -eq 3) 'Player unavailable during cave exit.'
  $along=($position[0]-$site.x)*$site.fx+($position[1]-$site.z)*$site.fz;$side=($position[0]-$site.x)*$site.sx+($position[1]-$site.z)*$site.sz
  Require ([Math]::Abs($side) -lt 4 -and $along -gt -8 -and $along -lt 24) 'Exit escaped bounded cave lane.'
  # Clear the complete armed model and shell margin, not only its root point.
  $probe=State @{x=$position[0];z=$position[1];y=($position[2]+.2)} $null;$returnSamples+=@{position=$position;along=$along;dir=(Num (Eval 'getDir player'));cursor=(Eval 'triCursorPos');state=$probe};$left=$along -lt -4 -and !$probe.inFootprint
 }while(!$left -and [DateTime]::UtcNow -lt $until)
 $null=Send @{cmd='key_up';sc=26};$held=$false;Exec 'setAccTime 0';$result.exitTraversal=@{left=$left;samples=$returnSamples};Require $left 'W-key traversal did not leave cave before undo.'
 $undone=Send @{cmd='dev_cave_editor';action='undo'};Require ($undone.count -eq 0) 'Normal cave undo did not release owner.'
 $restored=State $centre $roofRay;AssertUnchanged $baseline $restored
 Require (!$restored.inFootprint -and !$restored.atHeight -and $restored.terrainRayHit -and [Math]::Abs((Num $restored.roadY)-(Num $baseline.roadY)) -lt .001) 'Normal undo did not restore legacy terrain/support.'
 $result.normalUndo=@{undo=$undone;restored=$restored}
 # Tiny opening uses actual 0.1m parameters; neither terrain vertices nor brush radius are expanded.
 $tiny=Send @{cmd='dev_cave_editor';action='create';x=$site.x;y=$site.floor;z=$site.z;heading=$site.heading;width=.1;height=.1;length=32}
 $tinyCentre=Point $site 16 0 ($site.floor+.05);$tinyInside=State $tinyCentre @(($site.floor+.08),($site.floor+.02));AssertUnchanged $baseline $tinyInside
 Require ($tiny.count -eq 1 -and .1 -lt $tinyInside.grid -and $tinyInside.inFootprint -and $tinyInside.atHeight -and $tinyInside.boundedHeader -and !$tinyInside.terrainRayHit) 'Sub-grid opening failed exact footprint/vertical controls.'
 $outside=@();foreach($side in @(-.075,.075)){$q=State (Point $site 16 $side ($site.floor+.05)) $null;Require (!$q.inFootprint -and !$q.atHeight) '0.1m opening expanded past its authored footprint.';$outside+=,$q}
 $aboveWindow=State (Point $site 16 0 ($site.floor+.2)) $null;Require (!$aboveWindow.atHeight) '0.1m vertical opening expanded past ceiling.'
 $tinyRoof=State @{x=$centre.x;z=$centre.z;y=($baseline.terrainY+.2)} $roofRay;Require ($tinyRoof.terrainRayHit -and !$tinyRoof.atHeight) 'Tiny cave cut the terrain roof.'
 $result.tiny=@{created=$tiny;inside=$tinyInside;sideControls=$outside;aboveWindow=$aboveWindow;roof=$tinyRoof}
 $tinyView=Point $site -2 0 ($site.floor+.05);Capture 'tiny-entrance' @($tinyView.x,$tinyView.y,$tinyView.z) @($site.fx,0,$site.fz)
 Capture 'tiny-above' @($centre.x,($baseline.terrainY+3),$centre.z) @(0,-1,.01)
 $tinyUndo=Send @{cmd='dev_cave_editor';action='undo'};$tinyRestored=State $centre $roofRay;AssertUnchanged $baseline $tinyRestored
 Require ($tinyUndo.count -eq 0 -and !$tinyRestored.inFootprint -and $tinyRestored.terrainRayHit) 'Tiny cave undo failed.';$result.tinyUndo=@{undo=$tinyUndo;restored=$tinyRestored}
 if($SelectionChecks){
  $first=Send @{cmd='dev_cave_editor';action='create';x=$site.x;y=$site.floor;z=$site.z;heading=$site.heading;width=.1;height=.1;length=32}
  $companion=Point $site 0 3 0;$companionFloor=(Heights @($companion))[0]
  $second=Send @{cmd='dev_cave_editor';action='create';x=$companion.x;y=$companionFloor;z=$companion.z;heading=$site.heading;width=.1;height=.1;length=32}
  Require ($second.count -eq 2 -and $first.selectedId -ne $second.selectedId) 'Distinct editor records were not created.'
  $deleted=Send @{cmd='dev_cave_editor';action='delete';id=$first.selectedId}
  Require ($deleted.count -eq 1 -and $deleted.records[0].id -eq $second.selectedId) 'Deleting a non-last cave removed the wrong record.'
  $deletedSurface=State $centre $roofRay;Require (!$deletedSurface.inFootprint -and $deletedSurface.terrainRayHit) 'Deleted cave left a stale terrain cut.'
  $otherCentre=@{x=($companion.x+$site.fx*16);z=($companion.z+$site.fz*16);y=($companionFloor+.05)}
  $retained=State $otherCentre $null;Require ($retained.inFootprint -and $retained.atHeight) 'Deleting one cave closed its neighbour.'
  $deletedLast=Send @{cmd='dev_cave_editor';action='delete';id=$second.selectedId};Require ($deletedLast.count -eq 0) 'Selected final cave deletion failed.'
  $result.selectiveDelete=@{first=$first;second=$second;deleted=$deleted;originalRestored=$deletedSurface;otherRetained=$retained;finalDelete=$deletedLast}
 }
 Capture 'restored-above' @($centre.x,($baseline.terrainY+6),$centre.z) @(0,-1,.01)
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(30000) -and $p.ExitCode -eq 0) 'Normal game exit failed.'
 $after=Pair;Require (($after|ConvertTo-Json -Depth 6 -Compress) -ceq ($before|ConvertTo-Json -Depth 6 -Compress)) 'Installed pair changed during smoke.'
 Require (!(Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet)) 'Runtime failure logged.'
 $result.exit=$p.ExitCode;$result.passed=$true;Write-Host "Installed original Everon cave smoke PASS: $taskOut"
}catch{$result.error=$_.Exception.Message;throw}finally{
 if($p -and !$p.HasExited){try{if($client){if($held){$null=Send @{cmd='key_up';sc=26}};$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(10000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
 $result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
}
