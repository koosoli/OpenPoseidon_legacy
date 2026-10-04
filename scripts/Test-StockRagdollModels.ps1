# Serial installed regression of census-selected original OFP infantry model families.
# No assets/config are committed. Census is an external runtime artefact.
# Invoke via with-game-lock.sh. Default startup override is deliberately ABSENT.
# A created stock BulletSingleW is an actual collision, not player input/Fired proof.
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [string]$CensusPath,[string[]]$Classes=@(),[string[]]$Models=@(),[switch]$SaveLoad,[switch]$SkipProjectile,[switch]$BodyMotionDiagnostics,[switch]$SelfTest,
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
function AssertPair($before,$after){
 Require ($before.stamp-ceq $after.stamp-and $before.files.Count-eq 2-and $after.files.Count-eq 2) 'Installed source stamp or binary count changed.'
 foreach($name in @('OpenPoseidon.exe','wgpu_renderer.dll')){
  $a=@($before.files|Where-Object {$_.name-ceq $name});$b=@($after.files|Where-Object {$_.name-ceq $name})
  Require ($a.Count-eq 1-and $b.Count-eq 1) 'Installed binary identity missing or duplicated.'
  Require ($a[0].sha256-match '^[a-fA-F0-9]{64}$'-and $b[0].sha256-match '^[a-fA-F0-9]{64}$'-and $a[0].sha256-ieq $b[0].sha256-and $a[0].bytes-eq $b[0].bytes) ('Installed binary changed: '+$name)
 }
}
function AssertHitReactivation($before,$after,$owners,$backend){
 Require ($after.received-gt $before.received-and $after.wakes-gt $before.wakes-and $owners.active-eq 1-and $backend.bodies-eq 11-and $backend.joints-eq 10) 'Hit counters did not accompany real solver reactivation.'
 # A restored unsafe local trial may transfer aggregate impulse with lastPart=-1.
 # This admission is followed by actual retained-pose change and resettling checks.
}
function AssertProxyReceipt($actual){
 Require ($actual.PSObject.Properties.Name-contains 'proxies'-and $actual.proxies-is [array]-and $actual.proxies.Count-le (32*64)) 'Actual proxy field missing, untyped or beyond production per-LOD budget.'
 foreach($proxy in $actual.proxies){
  Require ($proxy.level-is [ValueType]-and $proxy.level-isnot [bool]-and $proxy.level-ge 0-and $proxy.level-lt 32-and [Math]::Floor((Num $proxy.level))-eq $proxy.level-and @($actual.levels.level)-contains $proxy.level) 'Actual proxy LOD absent or invalid.'
  Require ($proxy.selection-is [string]-and $proxy.selection.Length-gt 0-and $proxy.matrix-is [array]-and $proxy.matrix.Count-eq 12) 'Actual proxy source/affine matrix incomplete.'
  foreach($v in $proxy.matrix){Require ($v-is [ValueType]-and $v-isnot [bool]) 'Actual proxy matrix contains a nonnumeric component.';$null=Num $v}
 }
}
function ChestTarget($hull){
 Require ($hull.worldMin-is [array]-and $hull.worldMin.Count-eq 3-and $hull.worldMax-is [array]-and $hull.worldMax.Count-eq 3) 'Actual chest bounds incomplete.'
 $target=[double[]]::new(3)
 for($axis=0;$axis-lt 3;++$axis){$lo=Num $hull.worldMin[$axis];$hi=Num $hull.worldMax[$axis];Require ($lo-le $hi) 'Actual chest bounds reversed.';$target[$axis]=.5*($lo+$hi)}
 return ,$target
}
function ProjectileSegment($target,$direction){
 Require ($target-is [array]-and $target.Count-eq 3-and $direction-is [array]-and $direction.Count-eq 2) 'Projectile segment inputs incomplete.'
 $dx=Num $direction[0];$dz=Num $direction[1];Require ([Math]::Abs($dx)+[Math]::Abs($dz)-eq 1-and $dx*$dz-eq 0) 'Projectile direction must be one bounded cardinal route.'
 $start=[double[]]::new(3);$speed=[double[]]::new(3)
 # Explicit scalar assignments avoid PowerShell comma/operator array precedence.
 $start[0]=(Num $target[0])-$dx*1.25;$start[1]=Num $target[1];$start[2]=(Num $target[2])-$dz*1.25
 $speed[0]=$dx*800;$speed[1]=0;$speed[2]=$dz*800
 return @{start=$start;speed=$speed}
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
 $pairA=@{stamp='branch source time';files=@(@{name='OpenPoseidon.exe';sha256=('A'*64);bytes=10},@{name='wgpu_renderer.dll';sha256=('B'*64);bytes=20})}
 $pairB=@{files=@(@{bytes=20;sha256=('B'*64);name='wgpu_renderer.dll'},@{bytes=10;sha256=('A'*64);name='OpenPoseidon.exe'});stamp='branch source time'}
 AssertPair $pairA $pairB;$pairB.files[0].sha256='C'*64;$refused=$false;try{AssertPair $pairA $pairB}catch{$refused=$true};Require $refused 'Changed installed hash accepted.'
 $hitBefore=@{received=0;wakes=0;transfers=0;lastPart=-1};$hitAfter=@{received=1;wakes=1;transfers=0;lastPart=-1}
 AssertHitReactivation $hitBefore $hitAfter @{active=1} @{bodies=11;joints=10}
 $hitAfter.wakes=0;$refused=$false;try{AssertHitReactivation $hitBefore $hitAfter @{active=1} @{bodies=11;joints=10}}catch{$refused=$true};Require $refused 'Unreactivated received-hit counter accepted.'
 $hitAfter.wakes=1;$refused=$false;try{AssertHitReactivation $hitBefore $hitAfter @{active=1} @{bodies=0;joints=0}}catch{$refused=$true};Require $refused 'Wake without actual solver accepted.'
 $proxyFixture=[pscustomobject]@{proxies=@();levels=@([pscustomobject]@{level=0})};AssertProxyReceipt $proxyFixture
 $proxyFixture.proxies=@([pscustomobject]@{level=0;selection='proxy:actual';matrix=@(1,0,0,0,0,1,0,0,0,0,1,0)});AssertProxyReceipt $proxyFixture
 $proxyFixture.proxies[0].matrix[2]=[double]::NaN;$refused=$false;try{AssertProxyReceipt $proxyFixture}catch{$refused=$true};Require $refused 'Nonfinite actual proxy matrix accepted.'
 $proxyFixture.proxies[0].matrix=@(1,0,0);$refused=$false;try{AssertProxyReceipt $proxyFixture}catch{$refused=$true};Require $refused 'Truncated actual proxy matrix accepted.'
 $refused=$false;try{AssertProxyReceipt ([pscustomobject]@{levels=@()})}catch{$refused=$true};Require $refused 'Missing proxy field accepted as empty.'
 $chestFixture=[pscustomobject]@{worldMin=@(5500.125,21.75,9999.9);worldMax=@(5500.375,22.25,10000.1)}
 $chestTarget=ChestTarget $chestFixture;Require ($chestTarget-is [double[]]-and $chestTarget.Count-eq 3-and $chestTarget[0]-eq 5500.25-and $chestTarget[1]-eq 22-and $chestTarget[2]-eq 10000) 'Actual3D chest midpoint construction failed.'
 foreach($direction in @(@(1,0),@(-1,0),@(0,1),@(0,-1))){
  $segment=ProjectileSegment $chestTarget $direction
  Require ($segment.start-is [double[]]-and $segment.start.Count-eq 3-and $segment.speed-is [double[]]-and $segment.speed.Count-eq 3) 'Segment scalar construction became nested/untyped arrays.'
  Require ($segment.start[1]-eq 22-and $segment.speed[1]-eq 0-and [Math]::Abs($segment.speed[0])+[Math]::Abs($segment.speed[2])-eq 800) 'EngineXYZ segment/velocity invalid.'
  for($axis=0;$axis-lt 3;++$axis){Require ([Math]::Abs(($segment.start[$axis]+$segment.speed[$axis]*(1.25/800))-$chestTarget[$axis])-lt .0000001) 'Native projectile segment does not reach actual chest midpoint.'}
 }
 $refused=$false;try{$null=ProjectileSegment $chestTarget @(1,1)}catch{$refused=$true};Require $refused 'Non-cardinal segment accepted.'
 $tokens=$null;$errors=$null;$null=[Management.Automation.Language.Parser]::ParseFile($PSCommandPath,[ref]$tokens,[ref]$errors);Require ($errors.Count-eq 0) 'Runner parse failure.'
 # Reproduce the real observation race: owners ACTIVE before motion, then FROZEN.
 $settleAst=[Management.Automation.Language.Parser]::ParseFile($PSCommandPath,[ref]$tokens,[ref]$errors)
 $settleFunction=$settleAst.Find({param($node)$node-is [Management.Automation.Language.FunctionDefinitionAst]-and $node.Name-eq 'Settle'},$true)
 & {
  $case=@{};$SettleDeadlineSeconds=10;$state=@{ownerQueries=0;paused=$false}
  function Exec([string]$code){if($code-ceq 'setAccTime 0'){$state.paused=$true}}
  function AutoCounts{$state.ownerQueries++;if($state.ownerQueries-eq 1){return @{active=1;frozen=0;pending=0}};Require $state.paused 'Retirement query did not follow pause.';return @{active=0;frozen=1;pending=0}}
  function Motion{return @{frozen=$true;supported=$true;root=@(0,.2,0);terrainY=0}}
  function PhysicsCounts{Require $state.paused 'Backend retirement sampled before pause.';return @{bodies=0;joints=0}}
  function Pose([bool]$frozen){return @{frozen=$frozen}}
  function PoseSummary($value){return $value}
  Invoke-Expression $settleFunction.Extent.Text
  $null=Settle 'race';Require ($state.ownerQueries-eq 2-and $case.race.pausedOwners.active-eq 0-and $case.race.backend.bodies-eq 0-and $case.race.passed) 'Actual Settle observer used stale pre-freeze owner count.'
 }
 Write-Host 'Stock model runner SelfTest PASS (census selection, exclusion, invalid/duplicate targets, nested finite values, airborne freeze refusal, unordered/stable pair, aggregate hit acceptance and wake/backend negatives, zero actual proxies and invalid matrix negatives, exact3D bullet segment, actual Settle observation race, parse).';return
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
$result=[ordered]@{passed=$false;expectedCommit=$ExpectedCommit;installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;scope='One owned Everon game, serial normal deaths, actual Box3D11/10 and complete finite consumed poses, supported persistent freeze, actual stock projectile collision when enabled, cleanup and final Physics admission toggle. Geometric contact observations are not calibrated joint/convex-contact or visual acceptance. One representative per model family, not every class/animation/weapon.';completeCensus=([bool]$CensusPath-and $Classes.Count-eq 0-and $Models.Count-eq 0);projectileRequested=(!$SkipProjectile);saveLoadRequested=[bool]$SaveLoad;bodyMotionDiagnosticsRequested=[bool]$BodyMotionDiagnostics;requested=$targets;denominator=$targets.Count;passedFamilies=0;failedFamilies=0;cases=@();gates=@{}}
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
 Require ($actual.readonly-and $actual.frozen-eq $frozen-and $actual.solver-eq !$frozen-and $actual.retainedBounds-and $actual.bones.Count-eq 33-and $actual.levels.Count-ge 6-and $actual.levels.Count-le 32) 'Full consumed stock pose unavailable.'
 foreach($level in $actual.levels){Require ($level.palette.Count-eq 33-and $level.palettePoints.Count-eq $level.points-and $level.consumerPoints.Count-eq $level.compared-and $level.consumerPointIndices.Count-eq $level.compared) 'Incomplete actual point/palette arrays.'}
 AssertProxyReceipt $actual;AssertFiniteTree $actual;Require ($actual.hulls.Count-gt 0) 'Actual authored hulls absent.'
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
 if($BodyMotionDiagnostics){
  # Observations only: a frozen owner normally refuses the live-body query.
  try{$sample.bodyMotionRaw=Query 'corpse-auto-bodymotion'}catch{$sample.bodyMotionDiagnosticError=$_.Exception.Message}
  $sample.bodyMotionScope='Read-only limb motion, not an acceptance gate; frozen-owner refusal is expected.'
 }
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
 Exec 'setAccTime 0';$pausedOwners=AutoCounts;$backend=PhysicsCounts
 $case[$stage].pausedOwners=$pausedOwners;$case[$stage].backend=$backend
 Require ($pausedOwners.active-eq 0-and $pausedOwners.frozen-eq 1-and $pausedOwners.pending-eq 0-and $backend.bodies-eq 0-and $backend.joints-eq 0) 'Freeze did not retire actual handles.'
 $pose=Pose $true;$case[$stage].pose=PoseSummary $pose;$case[$stage].passed=$true
 return $pose
}
function Projectile($beforeHit){
 $hitBefore=HitStatus;Require ($hitBefore.recipe-eq 1) 'Actual frozen wake recipe absent.';$case.projectile=@{before=$hitBefore;passed=$false}
 $chest=@($beforeHit.hulls|Where-Object {$_.bone-match 'zebra|hrudnik'});Require ($chest.Count-gt 0) 'Actual authored chest Fire hull absent.'
 $target=ChestTarget $chest[0]
 $dir=$null;foreach($candidate in @(@(1,0),@(-1,0),@(0,1),@(0,-1))){
  $clear=$true;for($step=0;$step-le 5;++$step){$t=$step/5.0;$x=$target[0]-$candidate[0]*1.25*(1-$t);$z=$target[2]-$candidate[1]*1.25*(1-$t);$native=Num (Eval ('triTerrainHeight ['+(Fmt $x)+','+(Fmt $z)+']'));if($target[1]-le $native+.01){$clear=$false;break}}
  if($clear){$dir=$candidate;break}
 }
 Require ($null-ne $dir) 'No bounded above-floor native bullet segment reaches actual chest hull.'
 $segment=ProjectileSegment $target $dir;$start=$segment.start;$speed=$segment.speed
 Exec ('ragShotStartASL=['+(Fmt $start[0])+','+(Fmt $start[2])+','+(Fmt $start[1])+'];ragCorpseProjectile="BulletSingleW" createVehicle ['+(Fmt $start[0])+','+(Fmt $start[2])+',0];ragCorpseProjectile setPosASL ragShotStartASL;ragCorpseProjectile setVelocity ['+(Fmt $speed[0])+','+(Fmt $speed[2])+','+(Fmt $speed[1])+']')
 Require ((Eval '!isNull ragCorpseProjectile')-eq $true-and (Eval 'typeOf ragCorpseProjectile')-ceq 'BulletSingleW') 'Actual native rifle projectile creation failed.'
 $errors=Eval '[abs((getPosASL ragCorpseProjectile select 0)-(ragShotStartASL select 0)),abs((getPosASL ragCorpseProjectile select 1)-(ragShotStartASL select 1)),abs((getPosASL ragCorpseProjectile select 2)-(ragShotStartASL select 2))]'
 foreach($error in $errors){Require ((Num $error)-le .002) 'Native projectile position differs from issued ASL.'}
 $velocity=Eval 'velocity ragCorpseProjectile';Require ($velocity.Count-eq 3-and (Num $velocity[0])-eq $speed[0]-and (Num $velocity[1])-eq $speed[2]-and (Num $velocity[2])-eq $speed[1]) 'Native projectile velocity differs.'
 $case.projectile.segment=@{engineTarget=$target;engineStart=$start;engineVelocity=$speed;actualVelocity=$velocity;positionError=$errors;scope='Actual stock ammo collision, not player Fired/input proof.'}
 ExactPose $beforeHit (Pose $true);Exec 'setAccTime .2';$until=[DateTime]::UtcNow.AddSeconds(12)
 do{$hitAfter=HitStatus;if($hitAfter.wakes-gt $hitBefore.wakes){break};Require ([DateTime]::UtcNow-lt $until) 'Actual stock bullet did not wake retained owner.';Start-Sleep -Milliseconds 50}while($true)
 Exec 'setAccTime 0';$case.projectile.after=$hitAfter;$case.projectile.backend=PhysicsCounts
 AssertHitReactivation $hitBefore $hitAfter (AutoCounts) $case.projectile.backend
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
 $null=$p.Handle # Cache the owned handle before exit so PS7 retains a reliable ExitCode.
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
  }catch{$case.error=$_.Exception.Message;$case.errorScriptStackTrace=$_.ScriptStackTrace;$case.errorPosition=$_.InvocationInfo.PositionMessage}finally{
   try{Exec 'setAccTime 0;deleteVehicle ragCorpseProjectile;deleteVehicle ragVictim;triClearView';$case.gates.cleanup=Baseline}catch{$case.passed=$false;$case.cleanupError=$_.Exception.Message;$case.cleanupScriptStackTrace=$_.ScriptStackTrace}
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
 $after=Pair;$result.installedAfter=$after;AssertPair $before $after
 Require ($result.cases.Count-eq $result.denominator-and $result.passedFamilies-eq $result.denominator-and $result.failedFamilies-eq 0) 'One or more requested stock model families failed; inspect case.json evidence.'
 $result.passed=$true;Write-Host "Installed stock model ragdoll PASS $($result.passedFamilies)/$($result.denominator): $taskOut"
}catch{$result.error=$_.Exception.Message;$result.errorScriptStackTrace=$_.ScriptStackTrace;$result.errorPosition=$_.InvocationInfo.PositionMessage;throw}finally{
 if($p-and !$p.HasExited){try{if($client){$null=Send @{cmd='exit'}}}catch{};if(!$p.WaitForExit(10000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()};foreach($key in $saved.Keys){if($null-eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
 $result|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $taskOut 'result.json')
}
