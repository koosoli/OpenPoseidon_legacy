# Installed original Everon automatic damage deaths. No corpse-articulate command.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='original-everon-automatic-ragdoll',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [double]$SiteX=5500,[double]$SiteZ=10000,
 [ValidateSet('UP','MIDDLE','DOWN')][string]$Stance='UP',
 [switch]$RifleDeath,[switch]$SaveLoad,[switch]$RuntimeToggle,[switch]$InheritedBlastTrajectory,[switch]$BlastDeath,[switch]$FrozenProjectile,[switch]$DefaultStartup
)
$ErrorActionPreference='Stop';$taskRoot=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Num($v){$n=[double]::Parse([string]$v,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n)-and ![double]::IsInfinity($n)) 'Nonfinite value.';return $n}
function Fmt($v){return (Num $v).ToString('R',$culture)}
function Decode([string]$v){$v=$v.Trim();if($v.StartsWith('"')){Require $v.EndsWith('"') 'Incomplete SQF result.';return $v.Substring(1,$v.Length-2).Replace('""','"')};return ConvertFrom-Json $v -NoEnumerate}
function Pair{
 $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw;Require ($stamp-match [regex]::Escape($ExpectedCommit)) 'Installed source differs.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$f=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $f.FullName).Hash;bytes=$f.Length;writtenUtc=$f.LastWriteTimeUtc.ToString('o')}})}
}
Require ([bool]$env:LOCK_OWNER) 'Invoke through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved.'
$before=Pair;$taskOut=Join-Path $taskRoot ('build/automatic-ragdoll/'+$Label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$profile=Join-Path $taskOut 'user';New-Item -ItemType Directory -Force $profile|Out-Null
$settings=@{POSEIDON_USER_DIR=$profile;POSEIDON_AUTOMATIC_RAGDOLL='1';WGR_GRASS='0';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0';WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1'}
if($RuntimeToggle){$settings.POSEIDON_AUTOMATIC_RAGDOLL='0'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')
if($DefaultStartup){$settings.Remove('POSEIDON_AUTOMATIC_RAGDOLL');$clear+='POSEIDON_AUTOMATIC_RAGDOLL'}
$saved=@{};foreach($key in @($settings.Keys)+$clear){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$log=Join-Path $taskOut 'engine.log';$p=$null;$client=$null
$result=[ordered]@{passed=$false;expectedCommit=$ExpectedCommit;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;scope='Original Everon ordinary damage deaths (default startup when requested), actual consumed poses, four independent active owners/budget, persistent freeze, cleanup; rifle, save-load, inherited trajectory and actual grenade only when requested. Default promotion explicitly authorised by owner; this runner does not establish broad model or appearance acceptance.';arms=@();gates=@{}}
function Send($command,[bool]$allowError=$false){
 Require (!$p.HasExited) 'Owned game exited unexpectedly.'
 $wire=$command|ConvertTo-Json -Depth 12 -Compress;$wire|Add-Content -LiteralPath (Join-Path $taskOut 'harness.jsonl');$writer.WriteLine($wire)
 do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected.';$reply=$line|ConvertFrom-Json}while($null-eq $reply.ok)
 $line|Add-Content -LiteralPath (Join-Path $taskOut 'harness.jsonl');Require ($allowError-or $reply.ok) $line;return $reply
}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Query([string]$command){return Eval ('triPhysicsShowcase "'+$command+'"')}
function AutoCounts([bool]$enabled=$true){
 $status=Query 'corpse-auto-status';Require ($status-match '^OK corpse-auto-status enabled=(0|1) active=(\d+) frozen=(\d+) pending=(\d+) activeBudget=4 retainedBudget=64$') ('Automatic status unavailable: '+$status)
 Require ([int]$Matches[1]-eq [int]$enabled) 'Automatic admission switch differs from requested mode.'
 return @{active=[int]$Matches[2];frozen=[int]$Matches[3];pending=[int]$Matches[4];raw=$status}
}
function PhysicsCounts{
 $status=Query 'corpse-physics-status';Require ($status-match 'articulatedBodies=(\d+) articulatedJoints=(\d+)$') 'Actual Box3D counts unavailable.'
 return @{bodies=[int]$Matches[1];joints=[int]$Matches[2];raw=$status}
}
function HitStatus{
 $raw=Query 'corpse-auto-hit-status';Require ($raw-match '^OK corpse-auto-hit-status received=(\d+) wakes=(\d+) transfers=(\d+) lastPart=(-?\d+) recipe=(\d)$') ('Actual hit status unavailable: '+$raw)
 return @{raw=$raw;received=[int]$Matches[1];wakes=[int]$Matches[2];transfers=[int]$Matches[3];lastPart=[int]$Matches[4];recipe=[int]$Matches[5]}
}
function Pose([bool]$frozen){
 $actual=Send @{cmd='corpse_pose_state'}
 Require ($actual.readonly-and $actual.frozen-eq $frozen-and $actual.solver-eq !$frozen-and $actual.retainedBounds-and $actual.bones.Count-eq 33-and $actual.levels.Count-ge 6-and $actual.proxies.Count-gt 0) 'Full consumed stock pose unavailable.'
 foreach($level in $actual.levels){Require ($level.palette.Count-eq 33-and $level.palettePoints.Count-eq $level.points-and $level.consumerPoints.Count-eq $level.compared-and $level.consumerPointIndices.Count-eq $level.compared) 'Incomplete actual point/palette arrays.'}
 return $actual
}
function VisiblePose($actual){
 $levels=@($actual.levels|ForEach-Object{[ordered]@{level=$_.level;palette=$_.palette;palettePoints=$_.palettePoints;consumerPoints=$_.consumerPoints;consumerPointIndices=$_.consumerPointIndices}})
 return [ordered]@{model=$actual.model;bones=$actual.bones;objectTransform=$actual.objectTransform;minimum=$actual.retainedMinimum;maximum=$actual.retainedMaximum;radius=$actual.retainedRadius;levels=$levels;proxies=$actual.proxies}
}
function ExactPose($a,$b){Require (((VisiblePose $a)|ConvertTo-Json -Depth 20 -Compress)-ceq ((VisiblePose $b)|ConvertTo-Json -Depth 20 -Compress)) 'Frozen actual pose, proxies, root or bounds changed.'}
function CompareNumeric($a,$b,[double]$tolerance){
 if($null-eq $a-or $null-eq $b){Require ($null-eq $a-and $null-eq $b) 'Missing saved pose value.';return}
 if($a-is [ValueType]-and $a-isnot [bool]){Require ([Math]::Abs((Num $a)-(Num $b))-le $tolerance) 'Frozen save changed numeric pose.';return}
 if($a-is [System.Collections.IDictionary]){Require ($b-is [System.Collections.IDictionary]-and $a.Count-eq $b.Count) 'Saved pose structure changed.';foreach($k in $a.Keys){CompareNumeric $a[$k] $b[$k] $tolerance};return}
 if($a-is [array]){Require ($b-is [array]-and $a.Count-eq $b.Count) 'Saved pose array changed.';for($i=0;$i-lt $a.Count;++$i){CompareNumeric $a[$i] $b[$i] $tolerance};return}
 if($a-is [pscustomobject]){CompareNumeric @{} @{} $tolerance;foreach($property in $a.PSObject.Properties){CompareNumeric $property.Value $b.($property.Name) $tolerance};return}
 Require ([string]$a-ceq [string]$b) 'Saved pose identity changed.'
}
function Capture([string]$name,[string]$unit){
 $pos=Eval ('getPosASL '+$unit);Exec ('triSetView ['+(Fmt ($pos[0]+4))+','+(Fmt ($pos[2]+2.5))+','+(Fmt ($pos[1]-4))+',-.65,-.35,.65]')
 Start-Sleep -Milliseconds 400;$path=Join-Path $taskOut ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
 $until=[DateTime]::UtcNow.AddSeconds(15);while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow-lt $until) 'Missing screenshot.';Start-Sleep -Milliseconds 150}
 $result[$name]=@{path=$path;actorASL=$pos};Exec 'triClearView'
}
function Spawn([string]$name,[double]$offset,[string]$stance){
 $x=Fmt ($SiteX+$offset);$z=Fmt $SiteZ
 Exec ('"SoldierWB" createUnit [['+$x+','+$z+',0],ragGroup,"'+$name+'=this;this setCombatMode ""BLUE"";this setBehaviour ""CARELESS"";this disableAI ""MOVE"""];'+$name+' setDir 0;'+$name+' setUnitPos "'+$stance+'";doStop '+$name)
 Exec 'setAccTime 1';Start-Sleep -Milliseconds 1600;Exec 'setAccTime 0'
 Require ((Eval ('alive '+$name))-eq $true-and (Eval ('typeOf '+$name))-ceq 'SoldierWB') 'Actual stock live fixture failed.'
 return Eval ('getPosASL '+$name)
}
function AirborneArm([string]$unit,[string]$label,[bool]$requireTwelve){
 $samples=@();$pastTwelve=$false;$saveChecked=$false;$until=[DateTime]::UtcNow.AddSeconds(35)
 Exec 'setAccTime 1'
 do{
  $owners=AutoCounts;Require ($owners.active+$owners.pending+$owners.frozen-eq 5) 'Airborne death lost independent ownership.'
  $raw=Query 'corpse-auto-motion';Require ($raw-match '^OK corpse-auto-motion frozen=(\d) supported=(\d) time=([^ ]+) travel=([^ ]+) root=\(([^,]+),([^,]+),([^\)]+)\) speed=\(([^,]+),([^,]+),([^\)]+)\)$') ('Actual automatic motion witness unavailable: '+$raw)
  $sample=@{raw=$raw;frozen=([int]$Matches[1]-ne 0);supported=([int]$Matches[2]-ne 0);time=(Num $Matches[3]);travel=(Num $Matches[4]);root=@((Num $Matches[5]),(Num $Matches[6]),(Num $Matches[7]));speed=@((Num $Matches[8]),(Num $Matches[9]),(Num $Matches[10]))}
  $sample.terrainY=Num (Eval ('triTerrainHeight ['+(Fmt $sample.root[0])+','+(Fmt $sample.root[2])+']'));$samples+=,$sample;$result.gates[$label]=@{samples=$samples;pastTwelve=$pastTwelve;saveChecked=$saveChecked}
  if($sample.root[1]-gt $sample.terrainY+1){Require (!$sample.frozen) 'Automatic corpse froze above its native floor.'}
  if($sample.travel-gt 12-and $sample.root[1]-gt $sample.terrainY+1){$pastTwelve=$true}
  if($requireTwelve-and !$saveChecked-and $sample.root[1]-gt $sample.terrainY+2-and !$sample.frozen){
   Exec 'setAccTime 0';$held=Pose $false
   Require ((Query 'corpse-freeze')-ceq 'REFUSED corpse-freeze automatic-supported-low-motion-required') 'Explicit freeze accepted an airborne automatic body.'
   Require ((Eval 'triSaveGame "airborne-corpse"')-ceq 'OK') 'Actual airborne save failed.'
   Require ((PhysicsCounts).bodies-eq 11-and (PhysicsCounts).joints-eq 10) 'Airborne save retired moving solver bodies.';ExactPose $held (Pose $false);$saveChecked=$true;Exec 'setAccTime 1'
  }
  Require ([DateTime]::UtcNow-lt $until) 'Automatic airborne recovery exceeded35s; inspect actual samples.';if(!$sample.frozen){Start-Sleep -Milliseconds 75}
 }while(!$sample.frozen)
 Exec 'setAccTime 0';Require ($sample.root[1]-le $sample.terrainY+1-and $sample.root[1]-ge $sample.terrainY-2) 'Frozen body did not return to native ground.'
 if($requireTwelve){Require ($pastTwelve-and $saveChecked) 'Actual inherited trajectory did not cross old12m cutoff or exercise airborne save/freeze control.'}
 $result.gates[$label]=@{passed=$true;samples=$samples;pastTwelve=$pastTwelve;saveChecked=$saveChecked;finalPose=(Pose $true)};Capture $label $unit
 Exec ('deleteVehicle '+$unit);Require ((AutoCounts).frozen-eq 4-and (PhysicsCounts).bodies-eq 0-and (PhysicsCounts).joints-eq 0) 'Airborne arm cleanup failed.'
}
function WaitOwners([int]$active,[int]$frozen,[int]$seconds){
 $until=[DateTime]::UtcNow.AddSeconds($seconds)
 do{$count=AutoCounts;if($count.active-eq $active-and $count.frozen-eq $frozen-and $count.pending-eq 0){return $count};Require ([DateTime]::UtcNow-lt $until) ('Automatic owner deadline: '+$count.raw);Start-Sleep -Milliseconds 100}while($true)
}
try{
 foreach($key in $clear){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $mission=Join-Path $taskRoot 'tests/perf/missions/perf_field.eden'
 $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $args -PassThru
 $until=[DateTime]::UtcNow.AddSeconds(120)
 do{Require (!$p.HasExited) 'Startup exited.';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt $until) 'Startup deadline.';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 $until=[DateTime]::UtcNow.AddSeconds(120);while((Eval 'triSceneReady')-cne 'OK'){Require ([DateTime]::UtcNow-lt $until) 'Scene-ready deadline.';Start-Sleep -Milliseconds 300}
 Exec ('setAccTime 0;player allowDamage false;player setPos ['+(Fmt ($SiteX-60))+','+(Fmt $SiteZ)+',0];0 setRain 0;0 setFog 0;0 setOvercast 0;ragGroup=createGroup west')
 $world=Send @{cmd='dev_cave_editor';action='state';x=$SiteX;y=0;z=$SiteZ};Require ($world.worldName.Replace('\','/').ToLowerInvariant()-match '(^|/)eden\.wrp$'-and $world.count-eq 0) 'Original retail Everon or empty excavation fixture unavailable.';$result.world=$world
 $near=Send @{cmd='diag_near';pos=@($SiteX,$SiteZ);r=40};Require ($near.count-eq @($near.objects).Count) 'Obstacle census truncated.'
 $blockers=@($near.objects|Where-Object{($_.static-or (([int]$_.type-band 3)-ne 0))-and $_.shape-and [Math]::Abs((Num $_.pos[0])-$SiteX)-le (35+(Num $_.radius))-and [Math]::Abs((Num $_.pos[1])-$SiteZ)-le (5+(Num $_.radius))})
 $result.obstacles=@{receipt=$near;blockers=$blockers};Require ($blockers.Count-eq 0) 'Retail obstacles overlap the candidate fixture; select another bounded site.'
 if($DefaultStartup){Require ($null-eq [Environment]::GetEnvironmentVariable('POSEIDON_AUTOMATIC_RAGDOLL','Process')) 'Default startup override must be absent.';$result.gates.defaultOn=AutoCounts;if($RuntimeToggle){$null=Query 'corpse-auto-disable'}}
 if($RuntimeToggle){$result.gates.defaultOff=AutoCounts $false;Require ((Query 'corpse-auto-enable')-ceq 'OK corpse-auto-toggle existing-owners-retained') 'Runtime new-death admission did not enable.'}
 $result.initial=AutoCounts;Require ($result.initial.active-eq 0-and $result.initial.frozen-eq 0-and $result.initial.pending-eq 0) 'Unexpected pre-existing automatic corpse.'
 $prepared=@(Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: mission terrain preparation ready=true|AUTORAGDOLL: mission terrain preparation ready=1'|ForEach-Object{$_.Line})
 $registrationsBefore=@(Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: registered shared actual terrain'|ForEach-Object{$_.Line})
 Require ($prepared.Count-gt 0-and $registrationsBefore.Count-eq 1) 'Shared real terrain was not prepared before fixture damage.'
 $result.gates.terrainPreparation=@{beforeDamage=$true;preparationLines=$prepared;registrationLines=$registrationsBefore;scope='Setup cost moved to enable/startup, not removed.'}
 $placements=@();for($i=0;$i-lt 5;++$i){$placements+=,@{name=('ragVictim'+$i);stance=$Stance;position=(Spawn ('ragVictim'+$i) ($i*7) $Stance)}}
 $result.placements=$placements
 Exec 'setAccTime 0;ragVictim0 setDammage 1;ragVictim1 setDammage 1;ragVictim2 setDammage 1;ragVictim3 setDammage 1;ragVictim4 setDammage 1'
 $queued=AutoCounts;Require ($queued.pending-eq 4-and $queued.active-eq 0) 'Four actual queued deaths or budget fallback failed.';$result.gates.queued=$queued
 $admissionTimer=[Diagnostics.Stopwatch]::StartNew();Exec 'setAccTime .05';$active=WaitOwners 4 0 20;$admissionTimer.Stop();Exec 'setAccTime 0';$physics=PhysicsCounts;Require ($physics.bodies-eq 44-and $physics.joints-eq 40) 'Four independent Box3D corpses were not created.';$result.gates.active=@{owners=$active;backend=$physics;wallMilliseconds=$admissionTimer.Elapsed.TotalMilliseconds;firstTerrainRegistrationLines=@(Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: registered shared actual terrain'|ForEach-Object{$_.Line})}
 Require ($result.gates.active.firstTerrainRegistrationLines.Count-eq $registrationsBefore.Count) 'First damage rebuilt the prepared terrain.'
 $initial=@();for($i=0;$i-lt 4;++$i){Require ((Query ('corpse-auto-select:'+$i))-ceq 'OK corpse-auto-select selected') 'Owner selection failed.';$initial+=,(Pose $false);Capture ('initial-'+$i) ('ragVictim'+$i)}
 if($RuntimeToggle){$null=Query 'corpse-auto-disable';$off=AutoCounts $false;Require ($off.active-eq 4-and (PhysicsCounts).bodies-eq 44-and (PhysicsCounts).joints-eq 40) 'Turning off removed an active owner.';for($i=0;$i-lt 4;++$i){$null=Query ('corpse-auto-select:'+$i);ExactPose $initial[$i] (Pose $false)};$result.gates.activeToggle=$off;$null=Query 'corpse-auto-enable'}
 Exec 'setAccTime 4';$frozen=WaitOwners 0 4 30;Exec 'setAccTime 0';$physics=PhysicsCounts;Require ($physics.bodies-eq 0-and $physics.joints-eq 0) 'Automatic retirement leaked handles.'
 $settled=@();for($i=0;$i-lt 4;++$i){$null=Query ('corpse-auto-select:'+$i);$settled+=,(Pose $true);Require ($initial[$i].entity-ceq $settled[$i].entity) 'Independent owner changed.';Capture ('frozen-'+$i) ('ragVictim'+$i)}
 if($Stance-eq 'UP'){Require ([Math]::Abs((Num $initial[0].objectTransform[7])-(Num $settled[0].objectTransform[7]))-gt .1) 'Standing soldier did not physically fall/rebase.'}
 Require (((VisiblePose $initial[0])|ConvertTo-Json -Depth 20 -Compress)-cne ((VisiblePose $settled[0])|ConvertTo-Json -Depth 20 -Compress)) 'Actual body simulation did not change the consumed pose.'
 $result.gates.retirement=@{owners=$frozen;backend=$physics;initial=$initial;frozen=$settled}
 $start=Num (Eval 'time');Exec 'setAccTime 4';$until=[DateTime]::UtcNow.AddSeconds(20);while((Num (Eval 'time'))-$start-lt 31){Require ([DateTime]::UtcNow-lt $until) 'Persistence clock did not advance.';Start-Sleep -Milliseconds 200};Exec 'setAccTime 0'
 for($i=0;$i-lt 4;++$i){$null=Query ('corpse-auto-select:'+$i);ExactPose $settled[$i] (Pose $true)};$result.gates.persistence=@{passed=$true;elapsed=((Num (Eval 'time'))-$start)}
 if($RuntimeToggle){
  $null=Query 'corpse-auto-disable';$off=AutoCounts $false;Require ($off.frozen-eq 4) 'Turning off removed a frozen owner.'
  $null=Spawn 'ragDisabledVictim' -21 'UP';Exec 'ragDisabledVictim setDammage 1;setAccTime 1';Start-Sleep -Milliseconds 350;Exec 'setAccTime 0'
  $off=AutoCounts $false;Require (!(Eval 'alive ragDisabledVictim')-and $off.frozen-eq 4-and $off.active-eq 0-and $off.pending-eq 0) 'Disabled new death unexpectedly gained ownership.'
  Exec 'deleteVehicle ragDisabledVictim';for($i=0;$i-lt 4;++$i){$null=Query ('corpse-auto-select:'+$i);ExactPose $settled[$i] (Pose $true)}
  $result.gates.frozenToggle=$off;$null=Query 'corpse-auto-enable'
 }
 if($FrozenProjectile){
  $null=Query 'corpse-auto-select:0';$beforeHit=Pose $true;$hitBefore=HitStatus;Require ($hitBefore.recipe-eq 1) 'Frozen runtime recipe unavailable.'
  $hulls=@($beforeHit.hulls|Where-Object{$_.bone-match 'zebra|hrudnik' });Require ($hulls.Count-gt 0) 'Actual frozen chest fire hull missing.'
  $hull=$hulls[0];$target=@();for($axis=0;$axis-lt 3;++$axis){$target+=,(((Num $hull.worldMin[$axis])+(Num $hull.worldMax[$axis]))*.5)}
  # A native stock rifle projectile crosses the actual frozen Fire geometry.
  # Do not script damage or inject an impulse; receipt/wake must come from hit.
  $route=$null;foreach($direction in @(@(1,0),@(-1,0),@(0,1),@(0,-1))){
   $clearLine=$true;for($sample=1;$sample-le 5;++$sample){$distance=1.25*$sample/5;$x=$target[0]-$direction[0]*$distance;$z=$target[2]-$direction[1]*$distance;$native=Num (Eval ('triTerrainHeight ['+(Fmt $x)+','+(Fmt $z)+']'));if($target[1]-le $native+.01){$clearLine=$false}}
   if($clearLine){$route=$direction;break}
  };Require ($null-ne $route) 'No bounded clear projectile segment above native floor.'
  $start=@(($target[0]-$route[0]*1.25),$target[1],($target[2]-$route[1]*1.25));$speed=@(($route[0]*800),0,($route[1]*800))
  Exec ('ragShotStartASL=['+(Fmt $start[0])+','+(Fmt $start[2])+','+(Fmt $start[1])+'];ragCorpseProjectile="BulletSingleW" createVehicle ['+(Fmt $start[0])+','+(Fmt $start[2])+',0];ragCorpseProjectile setPosASL ragShotStartASL;ragCorpseProjectile setVelocity ['+(Fmt $speed[0])+','+(Fmt $speed[2])+','+(Fmt $speed[1])+']')
  Require ((Eval '!isNull ragCorpseProjectile')-eq $true-and (Eval 'typeOf ragCorpseProjectile')-ceq 'BulletSingleW') 'Actual stock rifle projectile creation failed.'
  # createVehicle uses AGL. Assert differences INSIDE the engine before its
  # six-significant-digit SQF formatter rounds large world coordinates.
  $issuedError=Eval '[abs(((getPosASL ragCorpseProjectile) select 0)-(ragShotStartASL select 0)),abs(((getPosASL ragCorpseProjectile) select 1)-(ragShotStartASL select 1)),abs(((getPosASL ragCorpseProjectile) select 2)-(ragShotStartASL select 2))]'
  Require ($issuedError.Count-eq 3) 'Actual projectile position witness unavailable.';foreach($positionError in $issuedError){Require ((Num $positionError)-le .002) 'Actual projectile ASL differs from measured chest segment.'}
  $issuedVelocity=Eval 'velocity ragCorpseProjectile';Require ($issuedVelocity.Count-eq 3-and (Num $issuedVelocity[0])-eq $speed[0]-and (Num $issuedVelocity[1])-eq $speed[2]-and (Num $issuedVelocity[2])-eq $speed[1]) 'Actual projectile velocity differs.'
  $result.gates.frozenProjectile=@{passed=$false;scope='Actual stock rifle ammo collision; not player Fired/input proof';beforePose=$beforeHit;beforeHit=$hitBefore;fireHull=$hull;target=$target;start=$start;issuedPositionASL=(Eval 'getPosASL ragCorpseProjectile');issuedPositionError=$issuedError;issuedVelocity=$issuedVelocity}
  ExactPose $beforeHit (Pose $true);Exec 'setAccTime .2';$until=[DateTime]::UtcNow.AddSeconds(12);$hits=@()
  do{$hit=HitStatus;$hits+=,$hit;$result.gates.frozenProjectile.hitSamples=$hits;Require ([DateTime]::UtcNow-lt $until) 'Issued projectile did not confirm native corpse hit, wake and local transfer.';if($hit.wakes-le $hitBefore.wakes-or $hit.transfers-le $hitBefore.transfers){Start-Sleep -Milliseconds 50}}while($hit.wakes-le $hitBefore.wakes-or $hit.transfers-le $hitBefore.transfers)
  Exec 'setAccTime 0';Require ($hit.received-gt $hitBefore.received-and $hit.lastPart-ge 0-and (AutoCounts).active-eq 1-and (PhysicsCounts).bodies-eq 11) 'Actual frozen native hit or independent solver wake missing.'
  $afterHit=Pose $false;$result.gates.frozenProjectile.activePose=$afterHit;Capture 'frozen-projectile-reaction' 'ragVictim0'
  Exec 'setAccTime 1';$null=WaitOwners 0 4 30;Exec 'setAccTime 0';$settled[0]=Pose $true
  Require (((VisiblePose $beforeHit)|ConvertTo-Json -Depth 20 -Compress)-cne ((VisiblePose $settled[0])|ConvertTo-Json -Depth 20 -Compress)) 'Actual projectile wake produced no changed consumed pose.'
  Require ((PhysicsCounts).bodies-eq 0-and (PhysicsCounts).joints-eq 0) 'Projectile resettlement leaked solver handles.'
  $result.gates.frozenProjectile.finalPose=$settled[0];$result.gates.frozenProjectile.passed=$true;Capture 'frozen-projectile-resettled' 'ragVictim0';Exec 'deleteVehicle ragCorpseProjectile'
 }
 if($SaveLoad){
  $stream.ReadTimeout=120000;try{Require ((Eval 'triSaveGame "automatic-corpse"')-ceq 'OK') 'Actual mission save failed.'}finally{$stream.ReadTimeout=30000}
  Exec 'deleteVehicle ragVictim0;deleteVehicle ragVictim1;deleteVehicle ragVictim2;deleteVehicle ragVictim3';Require ((AutoCounts).frozen-eq 0) 'Save mutation control failed.'
  $stream.ReadTimeout=120000;try{Require ((Eval 'triLoadGame "automatic-corpse"')-ceq 'OK') 'Actual mission load failed.'}finally{$stream.ReadTimeout=30000};Start-Sleep -Milliseconds 700;Exec 'setAccTime 0'
  Require ((AutoCounts).frozen-eq 4-and (PhysicsCounts).bodies-eq 0-and (PhysicsCounts).joints-eq 0) 'Frozen save restoration or handle-free ownership failed.'
  $loaded=@();for($i=0;$i-lt 4;++$i){$null=Query ('corpse-auto-select:'+$i);$loaded+=,(Pose $true)}
  for($i=0;$i-lt 4;++$i){$nearest=@($loaded|Sort-Object {[Math]::Abs((Num $_.objectTransform[3])-(Num $settled[$i].objectTransform[3]))}|Select-Object -First 1);CompareNumeric (VisiblePose $settled[$i]) (VisiblePose $nearest[0]) .002}
  $result.gates.saveLoad=@{passed=$true;loaded=$loaded};Capture 'loaded-first' 'ragVictim0'
 }
 if($InheritedBlastTrajectory){
  $null=Spawn 'ragTravelVictim' -14 'UP';Exec 'ragTravelVictim setVelocity [8,0,10];ragTravelVictim setDammage 1;setAccTime .05';$null=WaitOwners 1 4 20;Exec 'setAccTime 0';$null=Query 'corpse-auto-select:4';$result.gates.inheritedTrajectoryInitial=Pose $false;AirborneArm 'ragTravelVictim' 'inherited-blast-trajectory' $true
 }
 if($BlastDeath){
  $position=Spawn 'ragBlastVictim' -14 'UP';Exec ('ragGrenadeStartASL=['+(Fmt ($position[0]-3))+','+(Fmt $position[1])+','+(Fmt ($position[2]+.15))+'];ragLiveGrenade="GrenadeHand" createVehicle ['+(Fmt ($position[0]-3))+','+(Fmt $position[1])+',0];ragLiveGrenade setPosASL ragGrenadeStartASL;ragLiveGrenade setVelocity [0,0,0]')
  Require ((Eval '!isNull ragLiveGrenade')-eq $true-and (Eval 'typeOf ragLiveGrenade')-ceq 'GrenadeHand') 'Actual GrenadeHand ammo fixture missing.'
  $grenadeError=Eval '[abs(((getPosASL ragLiveGrenade) select 0)-(ragGrenadeStartASL select 0)),abs(((getPosASL ragLiveGrenade) select 1)-(ragGrenadeStartASL select 1)),abs(((getPosASL ragLiveGrenade) select 2)-(ragGrenadeStartASL select 2))]'
  Require ($grenadeError.Count-eq 3) 'Actual grenade position witness unavailable.';foreach($positionError in $grenadeError){Require ((Num $positionError)-le .002) 'Actual grenade ASL differs from requested target-relative placement.'}
  $result.gates.liveGrenade=@{type=(Eval 'typeOf ragLiveGrenade');position=(Eval 'getPosASL ragLiveGrenade');issuedPositionError=$grenadeError;targetPosition=$position};Exec 'setAccTime 1';$until=[DateTime]::UtcNow.AddSeconds(18);$grenadeSamples=@()
  while(Eval 'alive ragBlastVictim'){$grenadeSamples+=,(Eval '[time,getDammage ragBlastVictim,getPosASL ragBlastVictim,isNull ragLiveGrenade]');$result.gates.grenadeDamageWitnesses=$grenadeSamples;Require ([DateTime]::UtcNow-lt $until) 'Actual grenade did not kill stock target within18s.';Start-Sleep -Milliseconds 100}
  Exec 'setAccTime 0';$owners=AutoCounts;Require ($owners.active+$owners.pending+$owners.frozen-eq 5) 'Actual grenade death did not enter admitted automatic ownership; inspect authored-fallback reason.'
  if($owners.pending){Exec 'setAccTime .05';$null=WaitOwners 1 4 20;Exec 'setAccTime 0'};$null=Query 'corpse-auto-select:4';AirborneArm 'ragBlastVictim' 'actual-grenade-death' $false;Exec 'deleteVehicle ragLiveGrenade'
 }
 if($RifleDeath){
  $live=Spawn 'ragRifleVictim' -14 'UP'
  # perf_field contains only a WEST centre. GroupCreate refuses absent sides;
  # initialise and prove the actual binding BEFORE sampling typed unit values.
  Exec ('createCenter east;east setFriend [west,0];west setFriend [east,0];ragEnemyGroup=createGroup east;ragShooter=objNull;ragFired=0;ragLastWeapon="";"SoldierEB" createUnit [['+(Fmt ($SiteX-14))+','+(Fmt ($SiteZ-12))+',0],ragEnemyGroup,"ragShooter=this;this disableAI ""MOVE"";this setBehaviour ""COMBAT"";this setCombatMode ""RED"""]')
  Require ((Eval '!isNull ragShooter')-eq $true) 'AK74 fixture shooter binding missing: original mission requires createCenter east before createGroup east.'
  Require ((Eval 'alive ragShooter')-eq $true-and (Eval 'typeOf ragShooter')-ceq 'SoldierEB') 'AK74 fixture did not create the actual live stock Eastern soldier.'
  $shooterPosition=Eval 'getPosASL ragShooter';Require ($shooterPosition.Count-eq 3-and [Math]::Abs((Num $shooterPosition[0])-($SiteX-14))-lt 1-and [Math]::Abs((Num $shooterPosition[1])-($SiteZ-12))-lt 1) 'Actual AK74 shooter placement differs.'
  Exec 'ragShooter setDir 0;ragShooter setUnitPos "UP";ragShooter setSkill 1;removeAllWeapons ragShooter;ragShooter addMagazine "AK74";ragShooter addMagazine "AK74";ragShooter addWeapon "AK74";ragShooter selectWeapon "AK74";ragShooter addEventHandler ["Fired",{ragFired=ragFired+1;ragLastWeapon=_this select 1}];ragShooter reveal ragRifleVictim;ragShooter doTarget ragRifleVictim;setAccTime 1'
  Require ((Eval '"AK74" in weapons ragShooter')-eq $true-and (Num (Eval 'ragShooter ammo "AK74"'))-gt 0) 'Actual AK74 weapon or loaded ammunition missing.'
  $result.gates.rifleFixture=@{shooterType=(Eval 'typeOf ragShooter');shooterPosition=$shooterPosition;targetPosition=$live;weapon='AK74';ammo=(Eval 'ragShooter ammo "AK74"');center='Explicit EAST centre before group/unit creation; hostile fixture sides'}
  Start-Sleep -Milliseconds 1800;$until=[DateTime]::UtcNow.AddSeconds(35);$rifleSamples=@()
  while(Eval 'alive ragRifleVictim'){
   $sample=Eval '[time,ragFired,ragLastWeapon,typeOf ragShooter,getPosASL ragShooter,getDir ragShooter,weapons ragShooter,magazines ragShooter,ragShooter ammo "AK74",canFire ragShooter,ragShooter knowsAbout ragRifleVictim,getPosASL ragRifleVictim,getDammage ragRifleVictim,alive ragRifleVictim]'
   $rifleSamples+=,$sample;$result.gates.rifleDiagnostics=$rifleSamples;Require ([DateTime]::UtcNow-lt $until) 'Actual AK74 fixture did not kill within35s; see rifleDiagnostics for fired/ammo/knowledge/position/damage witnesses.'
   Exec 'ragShooter doTarget ragRifleVictim;ragShooter doFire ragRifleVictim;ragShooter fire "AK74"';Start-Sleep -Milliseconds 250
  };Exec 'setAccTime 0'
  Require ((Num (Eval 'ragFired'))-gt 0-and (Eval 'ragLastWeapon')-ceq 'AK74') 'No actual AK74 Fired event.';$rifleOwners=AutoCounts;Require ($rifleOwners.active+$rifleOwners.pending+$rifleOwners.frozen-eq 5-and $rifleOwners.frozen-ge 4) 'Actual rifle death did not enter independent automatic ownership.'
  Exec 'setAccTime 4';$null=WaitOwners 0 5 30;Exec 'setAccTime 0';$result.gates.rifle=@{passed=$true;shots=(Eval 'ragFired');damage=(Eval 'getDammage ragRifleVictim');alive=(Eval 'alive ragRifleVictim')};Capture 'actual-rifle-death' 'ragRifleVictim';Exec 'deleteVehicle ragRifleVictim;deleteVehicle ragShooter'
 }
 Exec 'deleteVehicle ragVictim0;deleteVehicle ragVictim1;deleteVehicle ragVictim2;deleteVehicle ragVictim3;deleteVehicle ragVictim4'
 $cleanup=AutoCounts;$backend=PhysicsCounts;Require ($cleanup.active-eq 0-and $cleanup.frozen-eq 0-and $cleanup.pending-eq 0-and $backend.bodies-eq 0-and $backend.joints-eq 0) 'Delete cleanup left retained owners or solver handles.';$result.gates.cleanup=@{owners=$cleanup;backend=$backend}
 Require ((Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: actual zero-motion consumers exact=(true|1)\b').Count-ge 4) 'Actual zero-motion consumer proofs missing.'
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(30000)-and $p.ExitCode-eq 0) 'Normal game exit failed.'
 Require (!(Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet)) 'Runtime failure logged.'
 Require ((Pair|ConvertTo-Json -Depth 6 -Compress)-ceq ($before|ConvertTo-Json -Depth 6 -Compress)) 'Installed pair changed.'
 $result.exit=$p.ExitCode;$result.passed=$true;Write-Host "Installed automatic ragdoll smoke PASS: $taskOut"
}catch{$result.error=$_.Exception.Message;throw}finally{
 if($p-and !$p.HasExited){try{if($client){$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(10000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){if($null-eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
 $result|ConvertTo-Json -Depth 24|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
}
