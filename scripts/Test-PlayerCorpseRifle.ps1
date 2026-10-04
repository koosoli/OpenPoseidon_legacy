# Installed original Everon editor-style mission: normal player input against four stock corpses.
# No createVehicle bullet, fire command, aim setter, freefly or photographic camera during firing.
# Invoke via with-game-lock.sh; the parent/deployer alone runs this script.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [ValidateSet('SoldierWB','SoldierEB','SoldierGB','Civilian')][string[]]$Classes=@('SoldierWB','SoldierEB','SoldierGB','Civilian'),
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='default-editor-corpse-rifle',
 [ValidateRange(10,60)][int]$SettleDeadlineSeconds=35,
 [switch]$SelfTest
)
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Fmt([double]$v){Require (![double]::IsNaN($v)-and ![double]::IsInfinity($v)) 'Nonfinite numeric value.';return $v.ToString('R',$culture)}
function Decode([string]$v){$v=$v.Trim();if($v.StartsWith('"')){Require $v.EndsWith('"') 'Incomplete SQF string.';return $v.Substring(1,$v.Length-2).Replace('""','"')};return ConvertFrom-Json $v -NoEnumerate}
function Clamp([double]$v,[double]$lo,[double]$hi){return [Math]::Max($lo,[Math]::Min($hi,$v))}
function Unit($v){$n=0.;foreach($x in $v){$null=Fmt $x;$n+=$x*$x};Require ($v.Count-eq 3-and $n-gt .000001) 'Invalid raw XYZ vector.';$n=[Math]::Sqrt($n);return ,@(($v[0]/$n),($v[1]/$n),($v[2]/$n))}
function AimError($state,$point){
 Require ($state.readonly-and $state.manual-and !$state.playerSuspended-and !$state.cameraEffect) 'Normal controlled-player input/camera state unavailable.'
 $actual=Unit @([double]$state.directionX,[double]$state.directionY,[double]$state.directionZ)
 $delta=@(($point[0]-$state.muzzleX),($point[1]-$state.muzzleY),($point[2]-$state.muzzleZ));$desired=Unit $delta
 $dot=$actual[0]*$desired[0]+$actual[1]*$desired[1]+$actual[2]*$desired[2]
 $range=[Math]::Sqrt($delta[0]*$delta[0]+$delta[1]*$delta[1]+$delta[2]*$delta[2])
 return @{actual=$actual;desired=$desired;range=$range;angle=[Math]::Acos((Clamp $dot -1 1));
  yaw=[Math]::IEEERemainder(([Math]::Atan2($desired[0],$desired[2])-[Math]::Atan2($actual[0],$actual[2])),(2*[Math]::PI));
  pitch=([Math]::Asin($desired[1])-[Math]::Asin($actual[1]));perpendicularError=($range*[Math]::Sqrt([Math]::Max(0.0,(1.0-($dot*$dot)))))}
}
function AimStep($error){
 # Gun motion lags SDL input. Fixed conservative feedback avoids interpreting
 # delayed motion from the preceding input as a new sensitivity measurement.
 $radiansPerPixel=.006;$damping=.4;$limit=30
 return @{dx=[int](Clamp ([Math]::Round($damping*$error.yaw/$radiansPerPixel)) (-$limit) $limit);
  dy=[int](Clamp ([Math]::Round(-$damping*$error.pitch/$radiansPerPixel)) (-$limit) $limit)}
}
function ChestTarget($pose){
 $chest=@($pose.hulls|Where-Object {$_.bone-match 'zebra|hrudnik'});Require ($chest.Count-gt 0) 'Actual posed chest Fire hull absent.'
 $h=$chest[0];Require ($h.worldMin.Count-eq 3-and $h.worldMax.Count-eq 3) 'Actual chest bounds incomplete.'
 $point=[double[]]::new(3);for($i=0;$i-lt 3;++$i){Require ($h.worldMin[$i]-le $h.worldMax[$i]) 'Reversed chest bounds.';$point[$i]=.5*([double]$h.worldMin[$i]+[double]$h.worldMax[$i]);$null=Fmt $point[$i]}
 return @{point=$point;hull=$h;frozen=[bool]$pose.frozen;entity=$pose.entity;model=$pose.model;timeMs=$pose.timeMs}
}
function MissionText{
 $groups=@('class Item0 { side="WEST"; class Vehicles { items=1; class Item0 { position[]={5500,0,9994}; azimut=0; id=0; side="WEST"; vehicle="SoldierWB"; player="PLAYER COMMANDER"; leader=1; skill=1; }; }; };')
 $sides=@{SoldierWB='WEST';SoldierEB='EAST';SoldierGB='GUER';Civilian='CIV'}
 for($i=0;$i-lt $Classes.Count;++$i){$id=$i+1;$x=5500+$i*16;$c=$Classes[$i];$side=$sides[$c]
  $groups+=('class Item'+$id+' { side="'+$side+'"; class Vehicles { items=1; class Item0 { position[]={'+$x+',0,10000}; azimut=0; id='+$id+'; side="'+$side+'"; vehicle="'+$c+'"; leader=1; skill=0.5; init="rifleVictim'+$i+'=this;this allowDamage false;this disableAI ""MOVE"";this disableAI ""TARGET"";this disableAI ""AUTOTARGET"";this setCombatMode ""BLUE"";this setBehaviour ""CARELESS"";this setUnitPos ""UP"";doStop this"; }; }; };')
 }
 return 'version=11; class Mission { randomSeed=1234; class Intel { year=1985;month=6;day=21;hour=16;minute=0;weather=0;fog=0; }; class Groups { items='+($Classes.Count+1)+';'+($groups-join "`n")+' }; }; class Intro {randomSeed=1;class Intel {};}; class OutroWin {randomSeed=2;class Intel {};}; class OutroLoose {randomSeed=3;class Intel {};};'
}
if($SelfTest){
 $s=@{readonly=$true;manual=$true;playerSuspended=$false;cameraEffect=$false;muzzleX=0.;muzzleY=1.;muzzleZ=0.;directionX=0.;directionY=-.1;directionZ=[Math]::Sqrt(.99)}
 $p=@(0.,.5,(5*[Math]::Sqrt(.99)));$e=AimError $s $p;Require ($e.angle-lt .000001-and $e.perpendicularError-lt .000001) 'Raw XYZ aiming geometry failed.'
 $off=AimError $s @(0.,1.,5.);Require ([Math]::Abs($off.perpendicularError-.5)-lt .000001-and $off.angle-gt .1) 'Nonzero muzzle-ray miss was rounded away.'
 $step=AimStep @{yaw=.6;pitch=-.6};Require ($step.dx-eq 30-and $step.dy-eq 30) 'Conservative mouse-step bounds failed.'
 $step=AimStep @{yaw=.03;pitch=.03};Require ($step.dx-eq 2-and $step.dy-eq -2) 'Damped mouse-step signs or gain failed.'
 $target=ChestTarget @{hulls=@(@{bone='hrudnik';worldMin=@(2,3,4);worldMax=@(4,5,6)})};Require ($target.point-is [double[]]-and $target.point.Count-eq 3-and $target.point[0]-eq 3-and $target.point[1]-eq 4-and $target.point[2]-eq 5) 'Scalar chest midpoint failed.'
 $s.cameraEffect=$true;$refused=$false;try{$null=AimError $s $p}catch{$refused=$true};Require $refused 'Camera-effect input accepted.'
 $m=MissionText;foreach($c in $Classes){Require ($m.Contains('vehicle="'+$c+'"')) 'Editor mission target omitted.'}
 $tokens=$null;$errors=$null;$null=[Management.Automation.Language.Parser]::ParseFile($PSCommandPath,[ref]$tokens,[ref]$errors);Require ($errors.Count-eq 0) 'Runner parse failed.'
 Write-Host 'PASS: aligned/nonzero raw XYZ aim, damped bounded mouse steps, scalar actual-hull midpoint, camera guard, four editor mission classes and parse.';return
}
Require ([bool]$env:LOCK_OWNER) 'Invoke via scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved.'
Require ($Classes.Count-ge 1-and @($Classes|Select-Object -Unique).Count-eq $Classes.Count) 'Duplicate or empty target list.'
function Pair{
 $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw;Require ($stamp-match [regex]::Escape($ExpectedCommit)) 'Installed source commit differs.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$f=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;bytes=$f.Length;sha256=(Get-FileHash -LiteralPath $f.FullName).Hash;writtenUtc=$f.LastWriteTimeUtc.ToString('o')}})}
}
$before=Pair;$out=Join-Path $root ('build/player-corpse-rifle/'+$Label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$profile=Join-Path $out 'user';$mission=Join-Path $out 'corpse_rifle.eden';New-Item -ItemType Directory -Force $profile,$mission|Out-Null
[IO.File]::WriteAllText((Join-Path $mission 'mission.sqm'),(MissionText))
[IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"version=3;`nqualityPreset=3;`nbrightness=1;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$settings=@{POSEIDON_USER_DIR=$profile;POSEIDON_SNOW_BULLET_TRACE='1';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0';WGR_GRASS='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1';WGR_TEMPORAL='0'}
$clear=@('POSEIDON_AUTOMATIC_RAGDOLL','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')
$saved=@{};foreach($key in @($settings.Keys)+$clear){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$log=Join-Path $out 'engine.log';$p=$null;$client=$null;$held=$false
$result=[ordered]@{passed=$false;expectedCommit=$ExpectedCommit;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;mission=$mission;missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash;classes=$Classes;scope='Default stock editor-loaded classes, ordinary damage death and real player M16 SDL fire. Actual five-field Fired, selected magazine decrement, normal muzzle ray and native terminal/hit diagnostics. Fresh and frozen trials reported separately; no artificial projectiles or aim setter.';cases=@()}
function Send($command,[bool]$allowError=$false){
 Require (!$p.HasExited) 'Owned game exited.';$wire=$command|ConvertTo-Json -Depth 16 -Compress;$wire|Add-Content -LiteralPath (Join-Path $out 'harness.jsonl');$writer.WriteLine($wire)
 do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness closed.';$line|Add-Content -LiteralPath (Join-Path $out 'harness.jsonl');$reply=$line|ConvertFrom-Json}while($null-eq $reply.ok)
 Require ($allowError-or $reply.ok) $line;return $reply
}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Query([string]$cmd){return Eval ('triPhysicsShowcase "'+$cmd+'"')}
function Clock{return [long](Send @{cmd='query';what='play_state'}).time_ms}
function Diagnostics{return @{owners=(Query 'corpse-auto-status');backend=(Query 'corpse-physics-status');hit=(Query 'corpse-auto-hit-status');motion=(Query 'corpse-auto-motion');bodyMotion=(Query 'corpse-auto-bodymotion')}}
function HitCount([string]$raw,[string]$field){if($raw-match ('\b'+$field+'=(\d+)\b')){return [int]$Matches[1]};return -1}
function ConsumedPose($pose){return @{root=$pose.objectTransform;levels=@($pose.levels|ForEach-Object {@{palette=$_.palette;points=$_.consumerPoints}});proxies=$pose.proxies}|ConvertTo-Json -Depth 30 -Compress}
function MotionSample{
 $raw=Query 'corpse-auto-motion';$s=@{raw=$raw;timeMs=(Clock)}
 if($raw-match 'root=\(([^,]+),([^,]+),([^\)]+)\)'){
  $x=[double]::Parse($Matches[1],$culture);$y=[double]::Parse($Matches[2],$culture);$z=[double]::Parse($Matches[3],$culture)
  foreach($v in @($x,$y,$z)){$null=Fmt $v};$ground=[double](Eval ('triTerrainHeight ['+(Fmt $x)+','+(Fmt $z)+']'))
  $s.root=@($x,$y,$z);$s.terrainY=$ground;$s.rootHeightAboveGround=$y-$ground
 };return $s
}
function LogLines{return @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue)}
function TerminalLines{return @(Select-String -LiteralPath $log -SimpleMatch 'SNOW_BULLET_TERMINAL kind=' -ErrorAction SilentlyContinue|ForEach-Object {$_.Line})}
function Ledger{return @{fired=[int](Eval 'rifleShots');ammo=[double](Eval 'player ammo "M16"')}}
function Release{$null=Send @{cmd='mouse_button';button=1;down=$false};$script:held=$false;Start-Sleep -Milliseconds 80}
function Capture([string]$name){$path=Join-Path $caseOut ($name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(10);while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow-lt $until) 'Screenshot missing.';Start-Sleep -Milliseconds 80};return @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}}
function Aim([string]$stage){
 Release;Exec 'triClearView;player switchCamera "INTERNAL"'
 if($stage-ceq 'frozen'){Exec 'setAccTime 1'}else{Exec 'setAccTime .2'}
 $idle=Ledger;$history=@();$stable=0;$until=[DateTime]::UtcNow.AddSeconds(35)
 Start-Sleep -Milliseconds 500
 for($i=0;$i-lt 60;++$i){
  Require ([DateTime]::UtcNow-lt $until) 'Bounded normal mouse aiming deadline.'
  $pose=Send @{cmd='corpse_pose_state'} -allowError $true
  if($pose.ok){$target=ChestTarget $pose}else{
   # Keep actual input diagnostics even when model admission itself failed.
   $pos=Eval 'getPosASL rifleVictim';$target=@{point=@([double]$pos[0],([double]$pos[2]+.25),[double]$pos[1]);poseError=$pose;frozen=$null;scope='Authored death root targeting estimate; no actual posed hull or hit claim.'}
  }
  $state=Send @{cmd='player_weapon_state'};$error=AimError $state $target.point
  Require ($error.range-ge 1.5-and $error.range-le 12) 'Actual muzzle/corpse range outside fixture.'
  $step=AimStep $error
  $history+=,@{iteration=$i;actualState=$state;target=$target;error=$error;step=$step;radiansPerPixel=.006;damping=.4;inputSettleMilliseconds=500}
  $case[$stage].aim=$history
  $now=Ledger;Require ($now.fired-eq $idle.fired-and $now.ammo-eq $idle.ammo) 'Unexpected rifle activity while aiming.'
  if($error.angle-le .008-and $error.perpendicularError-le .04){$stable++}else{$stable=0}
  if($stable-ge 3){Exec 'setAccTime 0';$case[$stage].ready=Send @{cmd='player_weapon_state'};$case[$stage].target=$target;$case[$stage].capture=Capture ($stage+'-before');return}
  if($stable-eq 0-and ($step.dx-ne 0-or $step.dy-ne 0)){$null=Send @{cmd='mouse_motion';dx=$step.dx;dy=$step.dy}}
  # Read the actual muzzle only after bounded live simulation has responded.
  Start-Sleep -Milliseconds 500
 }
 throw 'Normal SDL mouse could not establish the measured chest ray.'
}
function Shoot([string]$stage){
 $case[$stage]=[ordered]@{passed=$false};Aim $stage
 $trial=$case[$stage];$trial.before=Diagnostics;$trial.deathAgeSimSeconds=((Clock)-$case.deathTimeMs)/1000.;$beforeShot=Ledger;$terminalBefore=@(TerminalLines).Count
 $trial.actualPoseBefore=Send @{cmd='corpse_pose_state'} -allowError $true
 $trial.admissionProven=($trial.before.owners-match 'active=1 frozen=0 pending=0'-and $trial.before.backend-match 'articulatedBodies=11 articulatedJoints=10$')
 Require ($beforeShot.ammo-gt 0-and [double](Eval 'triPlayerCurrentMagazineAmmo')-eq $beforeShot.ammo) 'Selected loaded M16 magazine unavailable.'
 Exec 'setAccTime .05';$null=Send @{cmd='mouse_button';button=1;down=$true};$script:held=$true;$until=[DateTime]::UtcNow.AddSeconds(15)
 do{Start-Sleep -Milliseconds 30;$now=Ledger;Require ([DateTime]::UtcNow-lt $until) 'SDL click produced no actual Fired event.'}while($now.fired-eq $beforeShot.fired)
 Release;$trial.firedSource=Eval 'rifleShotSource';$trial.firedTime=Eval 'rifleShotTime';$trial.afterShot=Ledger;$trial.beforeShot=$beforeShot
 Require ($trial.afterShot.fired-eq $beforeShot.fired+1-and $trial.afterShot.ammo-eq $beforeShot.ammo-1-and $trial.firedSource.Count-eq 4-and $trial.firedSource[0]-ceq 'M16'-and $trial.firedSource[3]-ceq 'BulletSingleW') 'Single real M16 Fired/ammo source proof differs.'
 $trial.inputProven=$true;$releaseMs=Clock;Exec 'setAccTime 1';$until=[DateTime]::UtcNow.AddSeconds(8)
 $trial.postFireMotion=@()
 while((Clock)-$releaseMs-lt 2000){Require ([DateTime]::UtcNow-lt $until) 'Post-fire real simulation deadline.';$trial.postFireMotion+=,(MotionSample);Start-Sleep -Milliseconds 70}
 Exec 'setAccTime 0';$trial.after=Diagnostics;$trial.terminal=@(TerminalLines|Select-Object -Skip $terminalBefore);$trial.afterCapture=Capture ($stage+'-after')
 $trial.actualPoseAfter=Send @{cmd='corpse_pose_state'} -allowError $true
 $trial.consumedPoseChanged=($trial.actualPoseBefore.ok-and $trial.actualPoseAfter.ok-and (ConsumedPose $trial.actualPoseBefore)-cne (ConsumedPose $trial.actualPoseAfter))
 $trial.receivedDelta=(HitCount $trial.after.hit 'received')-(HitCount $trial.before.hit 'received');$trial.wakeDelta=(HitCount $trial.after.hit 'wakes')-(HitCount $trial.before.hit 'wakes');$trial.transferDelta=(HitCount $trial.after.hit 'transfers')-(HitCount $trial.before.hit 'transfers')
 Require ($trial.receivedDelta-ge 1) 'Real Fired projectile did not reach the automatic corpse hit path; inspect native terminal and ray receipts.'
 if($stage-ceq 'frozen'){Require ($trial.wakeDelta-ge 1-and $trial.consumedPoseChanged) 'Real rifle hit did not wake and change the actual frozen consumed corpse pose.'}else{Require ($trial.target.frozen-eq $false-and $trial.admissionProven) 'Fresh trial lacks actual active Box3D11/10 at the time of fire; inspect admission/aim age.'}
 $trial.passed=$true
}
function Settle{
 Exec 'setAccTime 4';$until=[DateTime]::UtcNow.AddSeconds($SettleDeadlineSeconds);$samples=@()
 do{$s=Diagnostics;$samples+=,$s;$case.settlement=$samples;if($s.owners-match 'active=0 frozen=1 pending=0'){break};Require ([DateTime]::UtcNow-lt $until) 'Actual supported frozen owner deadline.';Start-Sleep -Milliseconds 100}while($true)
 Exec 'setAccTime 0';Require ((Query 'corpse-physics-status')-match 'articulatedBodies=0 articulatedJoints=0$') 'Frozen solver handles retained.'
}
try{
 foreach($key in $clear){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $args -PassThru;$null=$p.Handle
 $until=[DateTime]::UtcNow.AddSeconds(120)
 do{Require (!$p.HasExited) 'Startup exited.';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt $until) 'Harness startup deadline.';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 $until=[DateTime]::UtcNow.AddSeconds(120);while((Eval 'triSceneReady')-cne 'OK'){Require ([DateTime]::UtcNow-lt $until) 'Mission ready deadline.';Start-Sleep -Milliseconds 300}
 Exec 'setAccTime 0;player allowDamage false;player selectWeapon "M16";player setUnitPos "UP";player switchCamera "INTERNAL";0 setRain 0;0 setFog 0;0 setOvercast 0;rifleShots=0;rifleShotTime=-1;rifleShotSource=[];player addEventHandler ["Fired",{rifleShots=rifleShots+1;rifleShotTime=time;rifleShotSource=[_this select 1,_this select 2,_this select 3,_this select 4]}];hint ""'
 Require ((Eval 'triCheatInfiniteAmmo false')-ceq 'OK') 'Infinite ammo disable refused.'
 $result.player=@{class=(Eval 'typeOf player');weapons=(Eval 'weapons player');ammo=(Eval 'player ammo "M16"')};Require ($result.player.class-ceq 'SoldierWB'-and $result.player.ammo-gt 0) 'Default stock editor player inventory unavailable.'
 $world=Send @{cmd='dev_cave_editor';action='state';x=5500;y=0;z=10000};$result.world=$world;Require ($world.worldName.Replace('\','/').ToLowerInvariant()-match '(^|/)eden\.wrp$'-and $world.count-eq 0) 'Original unchanged Everon unavailable.'
 for($i=0;$i-lt $Classes.Count;++$i){
  $caseOut=Join-Path $out ('{0:00}-{1}'-f $i,$Classes[$i]);New-Item -ItemType Directory -Force $caseOut|Out-Null
  $case=[ordered]@{class=$Classes[$i];passed=$false;errors=@()};$startLine=@(LogLines).Count
  try{
   Exec ('setAccTime 0;rifleVictim=rifleVictim'+$i)
   Require ((Eval 'typeOf rifleVictim')-ceq $Classes[$i]-and (Eval 'alive rifleVictim')-eq $true) 'Native mission/editor default target unavailable.'
   $case.baseline=Diagnostics;Require ($case.baseline.owners-match 'enabled=1 active=0 frozen=0 pending=0') 'Default automatic admission or serial baseline differs.'
   $pos=Eval 'getPosASL rifleVictim';$case.position=$pos;$case.actorIdentity=Eval 'format ["%1",rifleVictim]'
   $case.obstacles=Send @{cmd='diag_near';pos=@($pos[0],$pos[1]);r=15}
   Exec ('player setPos ['+(Fmt $pos[0])+','+(Fmt ($pos[1]-6))+',0];player setDir 0;player selectWeapon "M16";player setUnitPos "UP";setAccTime 1');Start-Sleep -Milliseconds 650;Exec 'setAccTime 0'
   $case.inputBeforeDeath=Send @{cmd='player_weapon_state'};$case.deathTimeMs=Clock
   Exec 'rifleVictim allowDamage true;rifleVictim setDammage 1';Require ((Eval 'alive rifleVictim')-eq $false) 'Ordinary damage did not kill target.';$case.queued=Diagnostics
   Exec 'setAccTime .2';$until=[DateTime]::UtcNow.AddSeconds(6)
   do{$d=Diagnostics;if($d.owners-match 'active=1 frozen=0 pending=0'){break};if($d.owners-match 'active=0 frozen=0 pending=0'){break};Require ([DateTime]::UtcNow-lt $until) 'Admission deadline.';Start-Sleep -Milliseconds 60}while($true)
   Exec 'setAccTime 0';$case.admission=Diagnostics;$case.selection=Query 'corpse-auto-select:0'
   try{Shoot 'fresh'}catch{Release;Exec 'setAccTime 0';$case.errors+=,@{stage='fresh';message=$_.Exception.Message;position=$_.InvocationInfo.PositionMessage}}
   try{Settle;Shoot 'frozen'}catch{Release;Exec 'setAccTime 0';$case.errors+=,@{stage='frozen';message=$_.Exception.Message;position=$_.InvocationInfo.PositionMessage}}
   $case.final=Diagnostics;$case.passed=($case.fresh.passed-and $case.frozen.passed)
  }catch{$case.errors+=,@{stage='fixture';message=$_.Exception.Message;position=$_.InvocationInfo.PositionMessage}}finally{
   $case.nativeLines=@(LogLines|Select-Object -Skip $startLine|Where-Object {$_-match 'AUTORAGDOLL:|SNOW_BULLET_TERMINAL|Cannot unlock field|Validation Error|UNHANDLED EXCEPTION'})
   try{Release;Exec 'setAccTime 0;deleteVehicle rifleVictim;triClearView';$case.cleanup=Diagnostics;Require ($case.cleanup.owners-match 'active=0 frozen=0 pending=0'-and $case.cleanup.backend-match 'articulatedBodies=0 articulatedJoints=0$') 'Owner cleanup incomplete.'}catch{$case.cleanupError=$_.Exception.Message;$case.passed=$false}
   $casePath=Join-Path $caseOut 'case.json';$case|ConvertTo-Json -Depth 45|Set-Content -LiteralPath $casePath;$result.cases+=,@{class=$case.class;passed=$case.passed;errors=$case.errors;evidence=$casePath}
   Write-Host (('{0}: {1}'-f $case.class,$(if($case.passed){'PASS'}else{'FAIL'}))+" $casePath")
  }
  Require (!$case.cleanupError) 'Cannot continue after incomplete actual owner cleanup.'
 }
 Exec 'setAccTime 1';$null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)-and $p.ExitCode-eq 0) 'Normal owned game exit failed.';$result.exitCode=$p.ExitCode
 $after=Pair;$result.installedAfter=$after;Require (($before|ConvertTo-Json -Depth 8 -Compress)-ceq ($after|ConvertTo-Json -Depth 8 -Compress)) 'Installed pair/stamp changed during test.'
 Require (!(Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet)) 'Native runtime failure logged.'
 Require (@($result.cases|Where-Object {!$_.passed}).Count-eq 0) 'One or more actual player corpse rifle trials failed; inspect case.json and harness.jsonl.'
 $result.passed=$true;Write-Host "Installed normal player corpse rifle PASS: $out"
}catch{$result.error=$_.Exception.Message;throw}finally{
 if($p-and !$p.HasExited){try{if($client){Release;$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(10000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){if($null-eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
 $result|ConvertTo-Json -Depth 45|Set-Content -LiteralPath (Join-Path $out 'result.json')
}
