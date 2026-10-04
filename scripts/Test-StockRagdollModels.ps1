# Serial installed regression of census-selected original OFP infantry model families.
# No assets/config are committed. Census is an external runtime artefact.
# Invoke via with-game-lock.sh. Default startup override is deliberately ABSENT.
# A created stock BulletSingleW is an actual collision, not player input/Fired proof.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [string]$CensusPath,[string[]]$Classes=@(),[string[]]$Models=@(),[switch]$SaveLoad,[switch]$SkipProjectile,[switch]$SelfTest,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='original-ofp-stock-models',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [double]$SiteX=5500,[double]$SiteZ=10000,
 [ValidateRange(10,90)][int]$SettleDeadlineSeconds=35
)
$ErrorActionPreference='Stop';$taskRoot=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Num($v){$n=[double]::Parse([string]$v,[Globalization.NumberStyles]::Float,$culture);Require (![double]::IsNaN($n)-and ![double]::IsInfinity($n)) 'Nonfinite value.';return $n}
function Fmt($v){return (Num $v).ToString('R',$culture)}
function ModelKey([string]$v){return $v.Replace('/','\').TrimStart('\').ToLowerInvariant()}
function AssertFiniteTree($v){
 if($null-eq $v){return};if($v-is [ValueType]-and $v-isnot [bool]){$null=Num $v;return}
 if($v-is [System.Collections.IDictionary]){foreach($x in $v.Values){AssertFiniteTree $x};return}
 if($v-is [array]){foreach($x in $v){AssertFiniteTree $x};return}
 if($v-is [pscustomobject]){foreach($x in $v.PSObject.Properties){AssertFiniteTree $x.Value}}
}
function AssertGroundFreeze($sample){
 Require (!$sample.frozen-or ($sample.root[1]-le $sample.terrainY+1-and $sample.root[1]-ge $sample.terrainY-2)) 'Actual frozen corpse is airborne or below native ground.'
}
function Decode([string]$v){$v=$v.Trim();if($v.StartsWith('"')){Require $v.EndsWith('"') 'Incomplete SQF result.';return $v.Substring(1,$v.Length-2).Replace('""','"')};return ConvertFrom-Json $v -NoEnumerate}
function Targets($census,[string[]]$requested,[string[]]$requestedModels=@()){
 $rows=@();$models=@{};$names=@{};$selectedModels=@{};$wantedModels=@($requestedModels|ForEach-Object{ModelKey $_})
 if($null-ne $census){
  Require ($census.families.Count-gt 0) 'Census contains no model families.'
  foreach($family in $census.families){
   # Asset-present scope1 story models remain in the denominator. Editor scope is not script admission.
   $present=if($family.PSObject.Properties.Name-contains 'assetPresent'){[bool]$family.assetPresent}else{[bool]$family.spawnable}
   if(!$present){continue};$name=[string]$family.representativeClass;$model=ModelKey ([string]$family.model)
   Require ($name-match '^[A-Za-z][A-Za-z0-9_]*$'-and $model.EndsWith('.p3d')) 'Census class/model invalid.'
   Require (!$models.ContainsKey($model)) 'Duplicate census model family.';$models[$model]=$true
   if(($requested.Count-gt 0-and $requested-notcontains $name)-or ($wantedModels.Count-gt 0-and $wantedModels-notcontains $model)){continue}
   Require (!$names.ContainsKey($name)) 'Duplicate representative class.';$names[$name]=$true
   $selectedModels[$model]=$true;$sourceClass=@($census.classes|Where-Object {$_.class-ceq $name})
   $rows+=,@{class=$name;expectedModel=$model;scope=if($sourceClass.Count-eq 1){$sourceClass[0].scope}else{$null};editorPublic=if($sourceClass.Count-eq 1){$sourceClass[0].scope-eq 2}else{[bool]$family.spawnable};familyEditorPublic=[bool]$family.spawnable}
  }
  foreach($name in $requested){Require ($names.ContainsKey($name)) ('Requested class absent from present-model census: '+$name)}
  foreach($model in $wantedModels){Require ($selectedModels.ContainsKey($model)) ('Requested model absent from present-model census: '+$model)}
 }else{
  Require ($wantedModels.Count-eq 0) '-Models requires a census.'
  Require ($requested.Count-gt 0) 'Supply -CensusPath for the full census, or -Classes for an explicitly partial campaign.'
  foreach($name in $requested){Require ($name-match '^[A-Za-z][A-Za-z0-9_]*$'-and !$names.ContainsKey($name)) 'Invalid/duplicate requested class.';$names[$name]=$true;$rows+=,@{class=$name;expectedModel=$null}}
 }
 Require ($rows.Count-ge 1-and $rows.Count-le 64) 'Requested model family count outside bounded1..64.'
 return ,$rows
}
if($SelfTest){
 $fixture=[pscustomobject]@{families=@([pscustomobject]@{spawnable=$true;representativeClass='SoldierWB';model='data3d/MC vojakw2.p3d'},[pscustomobject]@{spawnable=$false;representativeClass='Man';model='data3d/vojakw.p3d'})}
 $fixture.families+=,[pscustomobject]@{spawnable=$false;assetPresent=$true;representativeClass='Angelina';model='data3d/angelina.p3d'}
 $t=Targets $fixture @();Require ($t.Count-eq 2-and $t[0].expectedModel-ceq 'data3d\mc vojakw2.p3d') 'Census asset exclusion/story inclusion/normalisation regression.'
 $subset=Targets $fixture @() @('DATA3D/angelina.p3d');Require ($subset.Count-eq 1-and $subset[0].class-ceq 'Angelina') 'Model subset failed.'
 foreach($bad in @('missing','SoldierWB";exec')){$refused=$false;try{$null=Targets $fixture @($bad)}catch{$refused=$true};Require $refused 'Invalid target was accepted.'}
 $refused=$false;try{AssertFiniteTree @{palette=@(1,[double]::NaN)}}catch{$refused=$true};Require $refused 'Nonfinite nested pose accepted.'
 $fixture.families+=,$fixture.families[0];$refused=$false;try{$null=Targets $fixture @()}catch{$refused=$true};Require $refused 'Duplicate model family accepted.'
 $refused=$false;try{AssertGroundFreeze @{frozen=$true;root=@(0,5,0);terrainY=0}}catch{$refused=$true};Require $refused 'Airborne frozen corpse accepted.'
 AssertGroundFreeze @{frozen=$false;root=@(0,5,0);terrainY=0};AssertGroundFreeze @{frozen=$true;root=@(0,.2,0);terrainY=0}
 $tokens=$null;$errors=$null;$null=[Management.Automation.Language.Parser]::ParseFile($PSCommandPath,[ref]$tokens,[ref]$errors);Require ($errors.Count-eq 0) 'Runner parse failure.'
 Write-Host 'Stock model runner SelfTest PASS (census selection, exclusion, invalid/duplicate targets, nested finite values, airborne freeze refusal, parse).';return
}
$census=$null;if($CensusPath){$census=Get-Content -LiteralPath $CensusPath -Raw|ConvertFrom-Json}
$targets=Targets $census $Classes $Models
if($CensusPath-and $Classes.Count-eq 0-and $Models.Count-eq 0){
 Require (@($census.families|Where-Object {$_.PSObject.Properties.Name-notcontains 'assetPresent'}).Count-eq 0) 'Full census requires inspected assetPresent evidence for every family; editor spawnable alone excludes story characters.'
 Require ($targets.Count-eq 37) 'Full original OFP census must include all37 present model families. Use an explicit subset only for debugging.'
}
$null=Num $SiteX;$null=Num $SiteZ
function Pair{
 $stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw;Require ($stamp-match [regex]::Escape($ExpectedCommit)) 'Installed source differs.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object{$f=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $f.FullName).Hash;bytes=$f.Length;writtenUtc=$f.LastWriteTimeUtc.ToString('o')}})}
}
Require ([bool]$env:LOCK_OWNER) 'Invoke through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing owner game is preserved.'
$before=Pair;$taskOut=Join-Path $taskRoot ('build/stock-ragdoll-models/'+$Label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$profile=Join-Path $taskOut 'user';New-Item -ItemType Directory -Force $profile|Out-Null
$settings=@{POSEIDON_USER_DIR=$profile;WGR_GRASS='0';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0';WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1'}
$clear=@('POSEIDON_AUTOMATIC_RAGDOLL','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')
$saved=@{};foreach($key in @($settings.Keys)+$clear){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$log=Join-Path $taskOut 'engine.log';$p=$null;$client=$null
$result=[ordered]@{passed=$false;expectedCommit=$ExpectedCommit;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;scope='One owned Everon game, serial normal deaths, actual Box3D11/10 and complete finite consumed poses, supported persistent freeze, actual stock projectile collision when enabled, cleanup and final Physics admission toggle. Geometric contact observations are not calibrated joint/convex-contact or visual acceptance. One representative per model family, not every class/animation/weapon.';completeCensus=([bool]$CensusPath-and $Classes.Count-eq 0-and $Models.Count-eq 0);projectileRequested=(!$SkipProjectile);saveLoadRequested=[bool]$SaveLoad;requested=$targets;denominator=$targets.Count;passedFamilies=0;failedFamilies=0;cases=@();gates=@{}}
if($CensusPath){$result.census=@{path=[IO.Path]::GetFullPath($CensusPath);sha256=(Get-FileHash -LiteralPath $CensusPath).Hash;sourceCommit=$census.baseCommit;sourceInstalledCommit=$census.installedCommit;editorPublicFamilies=@($census.families|Where-Object spawnable).Count;presentFamilies=@($census.families|Where-Object assetPresent).Count}}
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
 AssertFiniteTree $actual;Require ($actual.hulls.Count-gt 0) 'Actual authored hulls absent.'
 foreach($level in $actual.levels){Require ($level.points-ge 1-and $level.points-le 4096-and $level.compared+$level.skippedPointTails-eq $level.points) 'Actual point coverage invalid.';foreach($matrix in $level.palette){Require ($matrix.Count-eq 12) 'Affine palette incomplete.'};foreach($point in @($level.palettePoints)+@($level.consumerPoints)){Require ($point.Count-eq 3) 'Actual point incomplete.'}}
 return $actual
}
function PoseSummary($actual){
 $bytes=[Text.Encoding]::UTF8.GetBytes(((VisiblePose $actual)|ConvertTo-Json -Depth 20 -Compress));$sha=[Security.Cryptography.SHA256]::Create()
 try{$digest=[Convert]::ToHexString($sha.ComputeHash($bytes))}finally{$sha.Dispose()}
 return @{model=$actual.model;entity=$actual.entity;frozen=$actual.frozen;solver=$actual.solver;bones=$actual.bones.Count;levels=$actual.levels.Count;proxies=$actual.proxies.Count;objectTransform=$actual.objectTransform;minimum=$actual.retainedMinimum;maximum=$actual.retainedMaximum;consumedSha256=$digest;fullReceipt='harness.jsonl actual corpse_pose_state response'}
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
 Start-Sleep -Milliseconds 400;$path=Join-Path $caseOut ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
 $until=[DateTime]::UtcNow.AddSeconds(15);while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow-lt $until) 'Missing screenshot.';Start-Sleep -Milliseconds 150}
 $case.captures[$name]=@{path=$path;actorASL=$pos};Exec 'triClearView'
}
function Baseline([bool]$enabled=$true){
 $owners=AutoCounts $enabled;$backend=PhysicsCounts
 Require ($owners.active-eq 0-and $owners.frozen-eq 0-and $owners.pending-eq 0-and $backend.bodies-eq 0-and $backend.joints-eq 0) 'Previous model left owners or solver handles.'
 return @{owners=$owners;backend=$backend}
}
function Spawn([string]$class){
 Exec ('"'+$class+'" createUnit [['+(Fmt $SiteX)+','+(Fmt $SiteZ)+',0],ragGroup,"ragVictim=this;this setCombatMode ""BLUE"";this setBehaviour ""CARELESS"";this disableAI ""MOVE"""];ragVictim setDir 0;ragVictim setUnitPos "UP";doStop ragVictim')
 Exec 'setAccTime 1';Start-Sleep -Milliseconds 1600;Exec 'setAccTime 0'
 Require ((Eval 'alive ragVictim')-eq $true-and (Eval 'typeOf ragVictim')-ceq $class) 'Actual requested stock class did not spawn alive.'
 return Eval 'getPosASL ragVictim'
}
function Motion{
 $raw=Query 'corpse-auto-motion'
 Require ($raw-match '^OK corpse-auto-motion frozen=(\d) supported=(\d) time=([^ ]+) travel=([^ ]+) root=\(([^,]+),([^,]+),([^\)]+)\) speed=\(([^,]+),([^,]+),([^\)]+)\)$') ('Actual automatic motion unavailable: '+$raw)
 $sample=@{raw=$raw;frozen=([int]$Matches[1]-ne 0);supported=([int]$Matches[2]-ne 0);time=(Num $Matches[3]);travel=(Num $Matches[4]);root=@((Num $Matches[5]),(Num $Matches[6]),(Num $Matches[7]));speed=@((Num $Matches[8]),(Num $Matches[9]),(Num $Matches[10]))}
 $sample.terrainY=Num (Eval ('triTerrainHeight ['+(Fmt $sample.root[0])+','+(Fmt $sample.root[2])+']'))
 AssertGroundFreeze $sample
 return $sample
}
function Settle([string]$stage){
 $samples=@();$case[$stage]=@{samples=$samples;passed=$false};$until=[DateTime]::UtcNow.AddSeconds($SettleDeadlineSeconds)
 Exec 'setAccTime 4'
 do{
  $owners=AutoCounts;Require ($owners.active+$owners.frozen+$owners.pending-eq 1) 'Actual automatic owner disappeared during settling.'
  $sample=Motion;$samples+=,$sample;$case[$stage].samples=$samples
  if($sample.frozen){break};Require ([DateTime]::UtcNow-lt $until) 'Supported freeze deadline exceeded.';Start-Sleep -Milliseconds 100
 }while($true)
 Exec 'setAccTime 0';$backend=PhysicsCounts;Require ($owners.active-eq 0-and $owners.frozen-eq 1-and $owners.pending-eq 0-and $backend.bodies-eq 0-and $backend.joints-eq 0) 'Freeze did not retire actual handles.'
 $pose=Pose $true;$case[$stage].backend=$backend;$case[$stage].pose=PoseSummary $pose;$case[$stage].passed=$true
 return $pose
}
function Projectile($beforeHit){
 $hitBefore=HitStatus;Require ($hitBefore.recipe-eq 1) 'Actual frozen wake recipe absent.';$case.projectile=@{before=$hitBefore;passed=$false}
 $chest=@($beforeHit.hulls|Where-Object {$_.bone-match 'zebra|hrudnik'});Require ($chest.Count-gt 0) 'Actual authored chest Fire hull absent.'
 $target=@();for($axis=0;$axis-lt 3;++$axis){$target+=((Num $chest[0].worldMin[$axis])+(Num $chest[0].worldMax[$axis]))*.5}
 $dir=$null;foreach($candidate in @(@(1,0),@(-1,0),@(0,1),@(0,-1))){
  $clear=$true;for($step=0;$step-le 5;++$step){$t=$step/5.0;$x=$target[0]-$candidate[0]*1.25*(1-$t);$z=$target[2]-$candidate[1]*1.25*(1-$t);$native=Num (Eval ('triTerrainHeight ['+(Fmt $x)+','+(Fmt $z)+']'));if($target[1]-le $native+.01){$clear=$false;break}}
  if($clear){$dir=$candidate;break}
 }
 Require ($null-ne $dir) 'No bounded above-floor native bullet segment reaches actual chest hull.'
 $start=@($target[0]-$dir[0]*1.25,$target[1],$target[2]-$dir[1]*1.25);$speed=@($dir[0]*800,0,$dir[1]*800)
 Exec ('ragShotStartASL=['+(Fmt $start[0])+','+(Fmt $start[2])+','+(Fmt $start[1])+'];ragCorpseProjectile="BulletSingleW" createVehicle ['+(Fmt $start[0])+','+(Fmt $start[2])+',0];ragCorpseProjectile setPosASL ragShotStartASL;ragCorpseProjectile setVelocity ['+(Fmt $speed[0])+','+(Fmt $speed[2])+','+(Fmt $speed[1])+']')
 Require ((Eval '!isNull ragCorpseProjectile')-eq $true-and (Eval 'typeOf ragCorpseProjectile')-ceq 'BulletSingleW') 'Actual native rifle projectile creation failed.'
 $errors=Eval '[abs((getPosASL ragCorpseProjectile select 0)-(ragShotStartASL select 0)),abs((getPosASL ragCorpseProjectile select 1)-(ragShotStartASL select 1)),abs((getPosASL ragCorpseProjectile select 2)-(ragShotStartASL select 2))]'
 foreach($error in $errors){Require ((Num $error)-le .002) 'Native projectile position differs from issued ASL.'}
 $velocity=Eval 'velocity ragCorpseProjectile';Require ($velocity.Count-eq 3-and (Num $velocity[0])-eq $speed[0]-and (Num $velocity[1])-eq $speed[2]-and (Num $velocity[2])-eq $speed[1]) 'Native projectile velocity differs.'
 $case.projectile.segment=@{engineTarget=$target;engineStart=$start;engineVelocity=$speed;actualVelocity=$velocity;positionError=$errors;scope='Actual stock ammo collision, not player Fired/input proof.'}
 ExactPose $beforeHit (Pose $true);Exec 'setAccTime .2';$until=[DateTime]::UtcNow.AddSeconds(12)
 do{$hitAfter=HitStatus;if($hitAfter.wakes-gt $hitBefore.wakes){break};Require ([DateTime]::UtcNow-lt $until) 'Actual stock bullet did not wake retained owner.';Start-Sleep -Milliseconds 50}while($true)
 Exec 'setAccTime 0';$case.projectile.after=$hitAfter;$case.projectile.backend=PhysicsCounts
 Require ($hitAfter.received-gt $hitBefore.received-and $hitAfter.lastPart-ge 0-and (AutoCounts).active-eq 1-and $case.projectile.backend.bodies-eq 11-and $case.projectile.backend.joints-eq 10) 'Hit counters did not accompany real solver reactivation.'
 # Safe aggregate fallback is valid when a local trial exceeds solver angular limits.
 $case.projectile.mode=if($hitAfter.transfers-gt $hitBefore.transfers){'local-transfer-counter-increased'}else{'no-local-transfer-increase-aggregate-path-possible'}
 $reactivated=Pose $false;$case.projectile.reactivatedPose=PoseSummary $reactivated;Capture 'projectile-reactivated' 'ragVictim'
 $final=Settle 'projectileSettlement'
 Require (((VisiblePose $beforeHit)|ConvertTo-Json -Depth 20 -Compress)-cne ((VisiblePose $final)|ConvertTo-Json -Depth 20 -Compress)) 'Actual bullet did not change the retained consumed pose.'
 $case.projectile.passed=$true;Capture 'projectile-refrozen' 'ragVictim';Exec 'deleteVehicle ragCorpseProjectile'
 return $final
}
try{
 foreach($key in $clear){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
 Require ($null-eq [Environment]::GetEnvironmentVariable('POSEIDON_AUTOMATIC_RAGDOLL','Process')) 'Default admission startup override must be absent.'
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $mission=Join-Path $taskRoot 'tests/perf/missions/perf_field.eden'
 $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList $args -PassThru
 $until=[DateTime]::UtcNow.AddSeconds(120)
 do{Require (!$p.HasExited) 'Startup exited.';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt $until) 'Startup deadline.';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 $until=[DateTime]::UtcNow.AddSeconds(120);while((Eval 'triSceneReady')-cne 'OK'){Require ([DateTime]::UtcNow-lt $until) 'Scene-ready deadline.';Start-Sleep -Milliseconds 300}
 Exec ('setAccTime 0;player allowDamage false;player setPos ['+(Fmt ($SiteX-60))+','+(Fmt $SiteZ)+',0];0 setRain 0;0 setFog 0;0 setOvercast 0;ragGroup=createGroup west;ragVictim=objNull;ragCorpseProjectile=objNull')
 $world=Send @{cmd='dev_cave_editor';action='state';x=$SiteX;y=0;z=$SiteZ};Require ($world.worldName.Replace('\','/').ToLowerInvariant()-match '(^|/)eden\.wrp$'-and $world.count-eq 0) 'Original untouched Everon unavailable.';$result.world=$world
 $near=Send @{cmd='diag_near';pos=@($SiteX,$SiteZ);r=40};Require ($near.count-eq @($near.objects).Count) 'Actual obstacle census truncated.'
 $blockers=@($near.objects|Where-Object{($_.static-or (([int]$_.type-band 3)-ne 0))-and $_.shape-and [Math]::Abs((Num $_.pos[0])-$SiteX)-le (5+(Num $_.radius))-and [Math]::Abs((Num $_.pos[1])-$SiteZ)-le (5+(Num $_.radius))})
 $result.obstacles=@{receipt=$near;blockers=$blockers};Require ($blockers.Count-eq 0) 'Retail obstacle overlaps corpse/projectile fixture.'
 $result.gates.startupAutomaticOverride=[Environment]::GetEnvironmentVariable('POSEIDON_AUTOMATIC_RAGDOLL','Process');$result.gates.defaultOn=Baseline
 $registrations=@(Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: registered shared actual terrain'|ForEach-Object{$_.Line})
 Require ($registrations.Count-eq 1-and (Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: mission terrain preparation ready=(true|1)' -Quiet)) 'Shared actual terrain was not prepared before first damage.'
 $result.gates.sharedTerrain=$registrations;$seenActual=@{}
 for($index=0;$index-lt $targets.Count;++$index){
  $target=$targets[$index];$caseOut=Join-Path $taskOut ('{0:00}-{1}'-f $index,$target.class);New-Item -ItemType Directory -Force $caseOut|Out-Null
  $case=[ordered]@{class=$target.class;expectedModel=$target.expectedModel;scope=$target.scope;editorPublic=$target.editorPublic;passed=$false;captures=@{};gates=@{}};$casePath=Join-Path $caseOut 'case.json'
  try{
   $case.gates.baseline=Baseline;$case.position=Spawn $target.class
   $case.gates.zeroMotionBefore=@(Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: actual zero-motion consumers exact=(true|1)\b').Count
   Exec 'ragVictim setDammage 1';$case.gates.queued=AutoCounts;Require ($case.gates.queued.pending-eq 1-and $case.gates.queued.active-eq 0-and $case.gates.queued.frozen-eq 0-and (Eval 'alive ragVictim')-eq $false) 'Ordinary damage death did not queue actual requested model.'
   Exec 'setAccTime .05';$until=[DateTime]::UtcNow.AddSeconds(20)
   do{$owners=AutoCounts;if($owners.active-eq 1-and $owners.pending-eq 0){break};Require ([DateTime]::UtcNow-lt $until) 'Actual model admission deadline.';Start-Sleep -Milliseconds 100}while($true)
   Exec 'setAccTime 0';Require ((Query 'corpse-auto-select:0')-ceq 'OK corpse-auto-select selected') 'Independent current owner selection failed.'
   $case.gates.active=PhysicsCounts;Require ($case.gates.active.bodies-eq 11-and $case.gates.active.joints-eq 10) 'Actual model did not create Box3D11/10.'
   $initial=Pose $false;$case.initialPose=PoseSummary $initial;$case.actualModel=ModelKey $initial.model
   if($target.expectedModel){Require ($case.actualModel-ceq $target.expectedModel) 'Actual admitted model differs from census family.'}
   Require (!$seenActual.ContainsKey($case.actualModel)) 'Two representatives resolved to the same actual model.';$seenActual[$case.actualModel]=$true
   Capture 'initial' 'ragVictim'
   $case.gates.zeroMotionAfter=@(Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: actual zero-motion consumers exact=(true|1)\b').Count
   Require ($case.gates.zeroMotionAfter-gt $case.gates.zeroMotionBefore) 'Actual initial zero-motion consumers did not provide exact per-model proof.'
   $settled=Settle 'initialSettlement';Require ($initial.entity-ceq $settled.entity) 'Model owner changed during simulation.'
   Require (((VisiblePose $initial)|ConvertTo-Json -Depth 20 -Compress)-cne ((VisiblePose $settled)|ConvertTo-Json -Depth 20 -Compress)) 'Normal model death never changed actual consumed pose.'
   Capture 'frozen' 'ragVictim'
   $start=Num (Eval 'time');Exec 'setAccTime 4';$until=[DateTime]::UtcNow.AddSeconds(20)
   while((Num (Eval 'time'))-$start-lt 31){Require ([DateTime]::UtcNow-lt $until) 'Frozen persistence clock did not advance.';Start-Sleep -Milliseconds 200}
   Exec 'setAccTime 0';ExactPose $settled (Pose $true);$case.gates.persistence=@{passed=$true;simulationSeconds=((Num (Eval 'time'))-$start);motion=(Motion)}
   if(!$SkipProjectile){$settled=Projectile $settled}
   if($SaveLoad){
    $stream.ReadTimeout=120000
    try{
     Require ((Eval 'triSaveGame "stock-ragdoll-family"')-ceq 'OK') 'Actual mission save failed.'
     Exec 'deleteVehicle ragVictim';$null=Baseline;Require ((Eval 'triLoadGame "stock-ragdoll-family"')-ceq 'OK') 'Actual mission load failed.'
     Start-Sleep -Milliseconds 700;Exec 'setAccTime 0';Require ((AutoCounts).frozen-eq 1-and (PhysicsCounts).bodies-eq 0-and (PhysicsCounts).joints-eq 0) 'Loaded frozen model did not retain zero handles.'
     $null=Query 'corpse-auto-select:0';$loaded=Pose $true;CompareNumeric (VisiblePose $settled) (VisiblePose $loaded) .002
     $case.gates.saveLoad=@{passed=$true;pose=(PoseSummary $loaded);motion=(Motion)};Capture 'save-loaded' 'ragVictim'
    }finally{$stream.ReadTimeout=30000}
   }
   $case.passed=$true
  }catch{$case.error=$_.Exception.Message}finally{
   try{Exec 'setAccTime 0;deleteVehicle ragCorpseProjectile;deleteVehicle ragVictim;triClearView';$case.gates.cleanup=Baseline}catch{$case.passed=$false;$case.cleanupError=$_.Exception.Message}
   $case|ConvertTo-Json -Depth 30|Set-Content -LiteralPath $casePath
   $result.cases+=,@{class=$target.class;expectedModel=$target.expectedModel;actualModel=$case.actualModel;passed=$case.passed;error=$case.error;cleanupError=$case.cleanupError;evidence=$casePath}
   if($case.passed){$result.passedFamilies++}else{$result.failedFamilies++}
   Write-Host ('Stock family {0}/{1}: {2} {3} {4}'-f ($index+1),$targets.Count,$target.class,$(if($case.passed){'PASS'}else{'FAIL'}),$case.error)
  }
  Require (!$case.cleanupError) 'Cannot safely continue model census after failed owner cleanup.'
  Require (!$p.HasExited) 'Owned game exited during model campaign.'
 }
 # Final shared Physics switch smoke: retaining a frozen owner is independent of new-death admission.
 $caseOut=Join-Path $taskOut 'final-toggle';New-Item -ItemType Directory -Force $caseOut|Out-Null;$case=[ordered]@{captures=@{}}
 $toggleClass='SoldierWB';$null=Spawn $toggleClass;Exec 'ragVictim setDammage 1;setAccTime .05';$until=[DateTime]::UtcNow.AddSeconds(20)
 while((AutoCounts).active-ne 1){Require ([DateTime]::UtcNow-lt $until) 'Toggle owner admission deadline.';Start-Sleep -Milliseconds 100};Exec 'setAccTime 0';$null=Query 'corpse-auto-select:0';$toggleFrozen=Settle 'toggleSettlement'
 Require ((Query 'corpse-auto-disable')-ceq 'OK corpse-auto-toggle existing-owners-retained') 'Physics admission did not disable.'
 $off=AutoCounts $false;Require ($off.frozen-eq 1-and $off.active-eq 0-and (PhysicsCounts).bodies-eq 0-and (PhysicsCounts).joints-eq 0) 'Physics OFF removed frozen owner or leaked backend.';ExactPose $toggleFrozen (Pose $true)
 Exec ('"'+$toggleClass+'" createUnit [['+(Fmt ($SiteX+3))+','+(Fmt $SiteZ)+',0],ragGroup,"ragDisabled=this;this disableAI ""MOVE"""];ragDisabled setDammage 1')
 Require ((Eval 'alive ragDisabled')-eq $false-and (AutoCounts $false).frozen-eq 1-and (AutoCounts $false).pending-eq 0-and (AutoCounts $false).active-eq 0) 'Physics OFF admitted a new death.';ExactPose $toggleFrozen (Pose $true)
 Exec 'deleteVehicle ragDisabled;deleteVehicle ragVictim';$offCleanup=Baseline $false
 Require ((Query 'corpse-auto-enable')-ceq 'OK corpse-auto-toggle existing-owners-retained') 'Physics admission did not re-enable.';$result.gates.physicsToggle=@{passed=$true;class=$toggleClass;off=$off;cleanup=$offCleanup;enabledBaseline=(Baseline);retainedPose=(PoseSummary $toggleFrozen)}
 $result.gates.finalTerrainRegistrations=@(Select-String -LiteralPath $log -Pattern 'AUTORAGDOLL: registered shared actual terrain'|ForEach-Object{$_.Line})
 if(!$SaveLoad){Require ($result.gates.finalTerrainRegistrations.Count-eq $registrations.Count) 'Serial normal deaths rebuilt shared terrain.'}
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(30000)-and $p.ExitCode-eq 0) 'Normal game exit failed.';$result.exit=$p.ExitCode
 Require (!(Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet)) 'Actual runtime failure logged.'
 Require ((Pair|ConvertTo-Json -Depth 6 -Compress)-ceq ($before|ConvertTo-Json -Depth 6 -Compress)) 'Installed pair changed during model campaign.'
 Require ($result.cases.Count-eq $result.denominator-and $result.passedFamilies-eq $result.denominator-and $result.failedFamilies-eq 0) 'One or more requested stock model families failed; inspect case.json evidence.'
 $result.passed=$true;Write-Host "Installed stock model ragdoll PASS $($result.passedFamilies)/$($result.denominator): $taskOut"
}catch{$result.error=$_.Exception.Message;throw}finally{
 if($p-and !$p.HasExited){try{if($client){$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(10000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){if($null-eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
 $result|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
}
