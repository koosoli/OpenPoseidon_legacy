# Actual ordinary rain on unedited stock native terrain. Invoke under with-game-lock.sh.
[CmdletBinding()]
param([ValidateSet('noe','eden')][string[]]$Maps=@('noe'),
 [ValidateSet('Pair','Off','On')][string]$SingleArm='Pair',
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='natural-native',
 [ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit='',
 [ValidateRange(30,600)][int]$RainSeconds=120,
 [ValidateRange(30,600)][int]$DrainSeconds=120,
 [ValidateRange(0,1800)][int]$RetirementSeconds=0,
 [string]$SourceCensus='',
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [switch]$SelfTest)
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
# Reuse the existing strict, pure validators; never evaluate the edit runner's
# executable body, launch logic or mutation commands. Record its exact hash.
$helperPath=Join-Path $PSScriptRoot 'Test-RainWaterFineRuntime.ps1'
$parseTokens=$null;$parseErrors=$null
$helperAst=[Management.Automation.Language.Parser]::ParseFile($helperPath,[ref]$parseTokens,[ref]$parseErrors)
Require ($parseErrors.Count -eq 0) 'Existing fine runtime helper source does not parse.'
$helperNames=@('Number','Decode-Eval','Restore-Environment','Assert-Budget','Rain-Ready','Find-Receipt','Complete-LogLines','Read-CompleteLogLines','Expect-Failure')
foreach($name in $helperNames){
 $fn=@($helperAst.FindAll({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name},$true))
 Require ($fn.Count -eq 1) ('Missing/ambiguous existing pure helper '+$name)
 Invoke-Expression $fn[0].Extent.Text
}
function Assert-Owners($fine,$state,[bool]$on,[uint64]$sourceRevision){
 Require ($fine.readonly -eq $true -and $fine.sourceReady -eq $true -and $fine.generation -eq $state.generation -and
          $fine.revision -eq $state.revision -and $fine.heightRevision -eq $sourceRevision) 'Snapshot is not this current unedited field/source.'
 if(!$on){Require (!$fine.fineActive -and !$fine.valid -and $fine.tiles -eq 0 -and $fine.cells -eq 0 -and @($fine.owners).Count -eq 0) 'OFF arm acquired native owners.';return}
 Require ($fine.fineActive -and $fine.valid -and $fine.tiles -ge 1 -and $fine.tiles -le 8 -and $fine.cells -eq 1024*$fine.tiles -and
          $fine.sourceRevision -eq $sourceRevision -and $fine.terrainSpacing -eq 6.25 -and $fine.terrainRange -eq 2048) 'ON arm lacks complete current native ownership.'
 Require (@($fine.owners).Count -eq $fine.tiles) 'Owner census missing or truncated.'
 $seen=@{};$volume=0;$integrated=0
 foreach($owner in $fine.owners){
  $key=([string]$owner.tileX+'/'+$owner.tileZ);Require (!$seen.ContainsKey($key)) 'Duplicate owner tile.';$seen[$key]=$true
  Require ($owner.finite -eq $true -and $owner.complete -eq $true -and $owner.sourceCurrent -eq $true -and $owner.cells -eq 1024 -and
           $owner.size -eq 200 -and $owner.x -eq 200*$owner.tileX-12.5 -and $owner.z -eq 200*$owner.tileZ-12.5 -and
           $owner.x -ge 0 -and $owner.z -ge 0 -and $owner.x+200 -le 12793.75 -and $owner.z+200 -le 12793.75) 'Owner is not a complete finite interior native tile.'
  foreach($name in @('volume','integratedVolume','bedMin','bedMax','headMin','headMax','minDepth','maxDepth')){$null=Number $owner.$name}
  Require ($owner.volume -ge 0 -and $owner.minDepth -ge 0 -and $owner.maxDepth -ge $owner.minDepth -and
           $owner.bedMin -le $owner.bedMax -and $owner.headMin -le $owner.headMax) 'Invalid held owner geometry/depth.'
  Require ([Math]::Abs($owner.volume-$owner.integratedVolume) -le [Math]::Max(.000001,$owner.volume*.000001)) 'Held owner heads disagree with exact triangle volume.'
  $volume+=$owner.volume;$integrated+=$owner.integratedVolume
 }
 Require ([Math]::Abs($volume-$fine.fineVolume) -le .000001 -and [Math]::Abs($integrated-$fine.integratedVolume) -le .000001) 'Owner totals disagree with global fine snapshot.'
}
function Depth-Class([double]$depth){
 $null=Number $depth;Require ($depth -ge 0) 'Negative physical depth.'
 if($depth -eq 0){return 'dry'};if($depth -lt .008){return 'thin-film'};if($depth -lt .05){return 'transition'};return 'standing-water'
}
function Natural-Cycles([string[]]$lines,[uint64]$sourceRevision,[int]$tiles){
 $pattern='RAIN_WATER_NATURAL_SOURCE world=(.+?) sourceRevision=(\d+) admitted=(\d+) retired=(\d+) refused=(\d+) examined=(\d+) valid=(true|false) tiles=(\d+) enabled=(true|false) presentation=pending\s*$'
 $rows=@();$admitted=0;$retired=0
 foreach($line in $lines){if($line.Contains('RAIN_WATER_NATURAL_SOURCE')){
  Require ($line -cmatch $pattern) 'Unknown/incomplete natural source trace grammar.';$m=$Matches
  Require ([uint64]$m[2] -eq $sourceRevision -and [int]$m[3] -le 1 -and [int]$m[6] -le 2 -and
           [int]$m[8] -le 8 -and $m[7] -ceq 'true' -and $m[9] -ceq 'true') 'Natural source/cycle/budget bounds failed.'
  if([int]$m[3] -gt 0){Require ([int]$m[6] -ge 1) 'Admission has no examined actual candidate.'}
  $admitted+=[int]$m[3];$retired+=[int]$m[4];$rows+=@{line=$line;world=$m[1];sourceRevision=[uint64]$m[2];admitted=[int]$m[3];retired=[int]$m[4];refused=[int]$m[5];examined=[int]$m[6];tiles=[int]$m[8]}
 }}
 Require ($admitted -gt 0 -and $admitted-$retired -eq $tiles) 'No exact natural ownership lifecycle accounting for this fresh unedited world.'
 return @{rows=$rows;admitted=$admitted;retired=$retired;currentTiles=$tiles;scope='Per-cycle source event counts; elapsed cadence/native read timings separately require profiling'}
}
function Frozen($a,$b){
 Require ($a.time -eq $b.time) 'Paused simulation clock advanced.'
 foreach($name in @('revision','generation','volume','rainVolume','infiltrationVolume','evaporationVolume','outletVolume','pendingSeconds','fineTiles')){
  Require ($a.field.$name -eq $b.field.$name) ('Paused water changed: '+$name)
 }
 Require ($a.source.heightRevision -eq $b.source.heightRevision -and !$b.source.baseline) 'Paused terrain source changed/was edited.'
 foreach($name in @('rain','liquidRain','particleDensity','particleSnowflakes')){Require ($a.weather.$name -eq $b.weather.$name) 'Paused actual weather changed.'}
}
if($SelfTest){
 foreach($v in @(0,.000001,.007999,.008,.049999,.05,2)){
  $expected=if($v -eq 0){'dry'}elseif($v -lt .008){'thin-film'}elseif($v -lt .05){'transition'}else{'standing-water'}
  Require ((Depth-Class $v) -ceq $expected) 'Physical optical class boundary changed.'
 }
 Expect-Failure {Depth-Class -1};Expect-Failure {Depth-Class ([double]::NaN)}
 $state=@{generation=2;revision=8};$owner=@{tileX=24;tileZ=23;x=4787.5;z=4587.5;size=200;cells=1024;finite=$true;complete=$true;sourceCurrent=$true;volume=1;integratedVolume=1;bedMin=10;bedMax=12;headMin=10;headMax=12;minDepth=0;maxDepth=.01}
 $fine=@{readonly=$true;sourceReady=$true;generation=2;revision=8;heightRevision=2;fineActive=$true;valid=$true;tiles=1;cells=1024;sourceRevision=2;terrainSpacing=6.25;terrainRange=2048;owners=@($owner);fineVolume=1;integratedVolume=1}
 Assert-Owners $fine $state $true 2
 foreach($change in @(@('sourceCurrent',$false),@('complete',$false),@('x',4787.6),@('cells',1023),@('integratedVolume',.8))){$saved=$owner[$change[0]];$owner[$change[0]]=$change[1];Expect-Failure {Assert-Owners $fine $state $true 2};$owner[$change[0]]=$saved}
 $fine.owners=@($owner,$owner);$fine.tiles=2;$fine.cells=2048;Expect-Failure {Assert-Owners $fine $state $true 2}
 $cycle='RAIN_WATER_NATURAL_SOURCE world=noe sourceRevision=2 admitted=1 retired=0 refused=0 examined=1 valid=true tiles=1 enabled=true presentation=pending'
 $null=Natural-Cycles @($cycle) 2 1
 foreach($bad in @($cycle.Replace('admitted=1','admitted=2'),$cycle.Replace('examined=1','examined=3'),$cycle.Replace('sourceRevision=2','sourceRevision=3'),$cycle.Replace('enabled=true','enabled=1'))){Expect-Failure {Natural-Cycles @($bad) 2 1}}
 Write-Host 'Natural runtime strict source/owner/triangle/depth helper checks PASS; no game launched.';return
}
Require ([bool]$env:LOCK_OWNER) 'Invoke through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve the running game.'
Require ($Maps.Count -eq @($Maps|Select-Object -Unique).Count) 'Duplicate map arms would overwrite evidence.'
if(!$SourceCensus){$SourceCensus=Join-Path $root 'docs/roadmap/modernisation/ORIGINAL-OFP-CONTACT-SOURCE-CENSUS-20261001.json'}
$census=Get-Content -LiteralPath $SourceCensus -Raw|ConvertFrom-Json
function Pair{
 $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim();Require ([bool]$stamp) 'No installed provenance.'
 if($ExpectedCommit){Require ($stamp -match ('(?i)\b'+[regex]::Escape($ExpectedCommit)+'[a-f0-9]*\b')) 'Installed source differs from expected commit.'}
 $files=@();foreach($name in @('OpenPoseidon.exe','wgpu_renderer.dll')){$path=Join-Path $GameDir $name;$f=Get-Item -LiteralPath $path;$files+=@{name=$name;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=$f.Length;writtenUtc=$f.LastWriteTimeUtc.ToString('o')}}
 return @{stamp=$stamp;exe=$files[0].sha256;dll=$files[1].sha256;files=$files}
}
function Same-Pair($a,$b){Require ($a.stamp -ceq $b.stamp -and $a.exe -ceq $b.exe -and $a.dll -ceq $b.dll) 'Installed executable/renderer changed during campaign.'}
$output=Join-Path $root ('build/rain-water-natural/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6));New-Item -ItemType Directory -Force $output|Out-Null
$before=Pair;$result=[ordered]@{status='running';installed=$before;scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;pureHelpersSha256=(Get-FileHash -LiteralPath $helperPath).Hash;sourceCensusSha256=(Get-FileHash -LiteralPath $SourceCensus).Hash;sourceHead=(& git -C $root rev-parse HEAD);lockOwner=$env:LOCK_OWNER;maps=@{};
 limitations='Opt-in eight globally selected owners; no terrain edits, mass seed, owner forcing or shelter-aware hydrology. Native centre-grid depth classification is bounded. Across-process rain/lighting may differ; no pixel-perfect ablation claim. Retained wet owners are not forced to retire.'}
$settings=@{WGR_RAIN_WATER='1';WGR_RAIN_WATER_TRACE='1';WGR_RAIN_WATER_SSR='1';WGR_TERRAIN_PUDDLES='0';WGR_GRASS='0';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';WGR_TERRAIN_JITTER='0';WGR_TEMPORAL='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_WEATHER_COVER='1';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0'}
$clear=@('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOW_SHELTER','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_EXPOSURE','WGR_AUTO_EXPOSURE','WGR_TONEMAP','WGR_HDR_ENCODE','POSEIDON_RAIN_WATER_FINE','WGR_RAIN_WATER_FINE','POSEIDON_PLAYER_GROUND_TRACE','POSEIDON_UNIFORM_WET_TRACE','POSEIDON_SAND_TRACE','POSEIDON_MUD_TRACE')
$keys=@($settings.Keys)+$clear+@('POSEIDON_USER_DIR');$savedEnv=@{};foreach($key in $keys){$savedEnv[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$log=$null;$reader=$null;$writer=$null
function Lines{if(!(Test-Path -LiteralPath $log)){return @()};return @(Read-CompleteLogLines $log)}
function Health{
 Require (!$p.HasExited) 'Owned game exited unexpectedly.';Require ([DateTime]::UtcNow -lt $deadline) 'Owned arm exceeded bounded wall time.'
 if([DateTime]::UtcNow -ge $nextOwnershipCheck){
  $processes=@(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)
  Require ($processes.Count -eq 1 -and $processes[0].Id -eq $p.Id) 'Foreign game contention detected; acceptance is invalid.';$script:nextOwnershipCheck=[DateTime]::UtcNow.AddSeconds(1)
 }
 Require (!(Lines|Select-String 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator|StartAutoTest could not boot')) 'Installed validation/script failure.'
}
function Send($command){Health;$line=$command|ConvertTo-Json -Compress;$line|Add-Content -LiteralPath $rpc;$writer.WriteLine($line)
 do{$replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.';$replyLine|Add-Content -LiteralPath $rpc;$reply=$replyLine|ConvertFrom-Json}while($null -eq $reply.ok);Require $reply.ok $replyLine;return $reply
}
function Eval([string]$code){return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Clock{ $q=Send @{cmd='query';what='play_state'};Require ($q.has_player -and $q.player_active -and $q.player_local) 'Actual controlled stock player unavailable.';return [long]$q.time_ms}
function State{return Send @{cmd='dev_rain_water';action='state'}}
function Fine([double]$x,[double]$z){return Send @{cmd='dev_rain_water';action='fine_state';x=$x;z=$z}}
function Bed([double]$x,[double]$z){return Send @{cmd='dev_terrain_brush';action='state';x=$x;z=$z}}
function Snapshot{return @{time=(Clock);field=(State);source=(Bed $site.x $site.z);weather=(Send @{cmd='weather_visibility'})}}
function Wait-Sim([int]$seconds){
 $start=Clock;$until=[DateTime]::UtcNow.AddSeconds([Math]::Max(40,$seconds/2+35))
 do{Start-Sleep -Milliseconds 150;Health;Require ([DateTime]::UtcNow -lt $until) 'Actual simulation clock did not advance within bounded wall time.'}while((Clock)-$start -lt $seconds*1000)
}
function View([double]$x,[double]$z,[double]$y,[double]$az,[double]$el){Require ((Eval ('triFreeFlyPose "'+((@($x,$z,$y,$az,$el)|ForEach-Object{Number $_}) -join ' ')+'"')) -ceq 'OK') 'Camera pose refused.'}
function Capture([string]$name){
 Start-Sleep -Milliseconds 500;$path=Join-Path $armDir ($name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(10)
 do{Health;$bytes=$null;if(Test-Path -LiteralPath $path){try{$bytes=[IO.File]::ReadAllBytes($path)}catch [IO.IOException]{}}
  if($bytes -and $bytes.Length -gt 36 -and ($bytes[($bytes.Length-8)..($bytes.Length-5)]-join ',') -eq '73,69,78,68'){break}
  Require ([DateTime]::UtcNow -lt $until) 'Complete PNG absent.';Start-Sleep -Milliseconds 100
 }while($true)
 Require ([BitConverter]::ToString($bytes[0..7]) -ceq '89-50-4E-47-0D-0A-1A-0A' -and ($bytes[16..19]-join ',') -eq '0,0,5,0' -and ($bytes[20..23]-join ',') -eq '0,0,2,208') 'PNG not valid1280x720.'
 $arm.captures[$name]=@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;snapshot=(Snapshot)}
}
function Unedited($header){Require ($header.readonly -eq $true -and $header.baseline -eq $false -and $header.range -eq 2048 -and $header.spacing -eq 6.25 -and $header.heightRevision -eq $sourceRevision) 'Actual native source changed or was edited.'}
function Native-Tile($owner,[string]$name){
 $a=Snapshot;$header=Bed $owner.x $owner.z;Unedited $header;$vertices=@();$until=[DateTime]::UtcNow.AddSeconds(180)
 for($z=0;$z -le 32;++$z){for($x=0;$x -le 32;++$x){
  Require ([DateTime]::UtcNow -lt $until) 'Native source collector exceeded180s.';$px=$owner.x+6.25*$x;$pz=$owner.z+6.25*$z;$v=Bed $px $pz;Unedited $v
  Require ($v.vertexWorldX -eq $px -and $v.vertexWorldZ -eq $pz -and $v.seaLevel -eq $header.seaLevel) 'Native source coordinate/sea mismatch.';$null=Number $v.vertexHeight;$vertices+=,@($px,$pz,$v.vertexHeight)
 }}
 $b=Snapshot;Frozen $a $b;$path=Join-Path $armDir ($name+'.json');@{scope='Actual unedited33x33 native vertices; no resampling or edited baseline';map=$map;source=$source;installed=$before;header=$header;origin=@($owner.x,$owner.z);side=33;order='z-major then x';vertices=$vertices;before=$a;after=$b}|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $path
 return @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;count=1089;origin=@($owner.x,$owner.z);header=$header}
}
function Sample-Owner($owner){
 $a=Snapshot;$points=@();$counts=@{dry=0;'thin-film'=0;transition=0;'standing-water'=0};$until=[DateTime]::UtcNow.AddSeconds(180)
 for($z=0;$z -lt 32;++$z){for($x=0;$x -lt 32;++$x){
  Require ([DateTime]::UtcNow -lt $until) 'Native depth-grid collector exceeded180s.';$px=$owner.x+($x+.5)*6.25;$pz=$owner.z+($z+.5)*6.25
  $sample=Send @{cmd='dev_rain_water';action='sample';x=$px;z=$pz};Require $sample.valid 'Selected owner centre query invalid.'
  foreach($name in @('depth','height','terrainY','flowX','flowZ')){$null=Number $sample.$name};$class=Depth-Class $sample.depth;$counts[$class]++
  $points+=@{x=$px;z=$pz;class=$class;sample=$sample}
 }}
 $representatives=@();foreach($class in $counts.Keys){$pick=@($points|Where-Object{$_.class -ceq $class}|Sort-Object{[double]$_.sample.depth})|Select-Object -Last 1
  if($pick){$geometry=Fine $pick.x $pick.z;Require ($geometry.selected -and [Math]::Abs($geometry.selected.localDepth-$pick.sample.depth) -lt .0001 -and [Math]::Abs($geometry.selected.head-$pick.sample.height) -lt .0001) 'Depth representative differs from held horizontal native head.'
   $representatives+=@{point=$pick;selected=$geometry.selected}
  }
 }
 $b=Snapshot;Frozen $a $b;return @{scope='1024 actual native cell-centres; extrema/edge location between centres not exhausted';owner=$owner;counts=$counts;points=$points;representatives=$representatives;before=$a;after=$b}
}
function Receipt($fine,$state){
 $until=[DateTime]::UtcNow.AddSeconds(10)
 do{Health;try{return Find-Receipt @(Lines) $fine $state}catch{$arm.gates.lastReceiptError=$_.Exception.Message};Require ([DateTime]::UtcNow -lt $until) 'No exact current accepted renderer receipt.';Start-Sleep -Milliseconds 150}while($true)
}
function Zero-OwnerReceipt($fine,$state){
 $pattern='Rain water publication: worldToken=(\d+) generation=(\d+) heightRevision=(\d+) terrainRange=(\d+) terrainSpacing=([0-9.eE+-]+) revision=(\d+) flags=(\d+) copiedCoarse=(\d+) copiedFine=(\d+) cachedCoarse=(\d+) cachedFine=(\d+) tiles=(\d+) enabled=(0|1) accepted=(true|false)\s*$'
 $until=[DateTime]::UtcNow.AddSeconds(10)
 do{Health;foreach($line in @(Lines)){if($line -cmatch $pattern){$m=$Matches
  if($m[1] -ceq [string]$fine.worldToken -and [decimal]$m[2] -eq $fine.generation -and [decimal]$m[3] -eq $fine.heightRevision -and
     [int]$m[4] -eq 2048 -and [double]::Parse($m[5],$culture) -eq 6.25 -and [decimal]$m[6] -eq $fine.revision -and
     [int]$m[7] -eq 3 -and [int]$m[10] -eq $state.width*$state.height -and [int]$m[11] -eq 0 -and [int]$m[12] -eq 0 -and $m[13] -ceq '1' -and $m[14] -ceq 'true'){return $line}
 }};Require ([DateTime]::UtcNow -lt $until) 'Retirement lacks exact accepted current flags3 zero-owner pair.';Start-Sleep -Milliseconds 150}while($true)
}
function Close-Owned{
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game failed normal zero exit.'
 $meta=@{status='ok';timed_out=$false;contended=$false;exit_code=[int]$p.ExitCode};$path=Join-Path $armDir 'lifecycle.json';$meta|ConvertTo-Json|Set-Content -LiteralPath $path
 & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath $path -LogPath $log;$arm.lifecycle=$meta
}
try{
 foreach($map in $Maps){
  $source=$census.maps.$map;Require ($null -ne $source) 'Map absent from exact original census.'
  $asset=if($map -eq 'noe'){Join-Path $GameDir 'AddOns/Noe.pbo'}else{Join-Path $GameDir 'Worlds/eden.wrp'};$expected=if($map -eq 'noe'){$census.noeArchiveSha256}else{$source.wrpSha256}
  Require ((Get-FileHash -LiteralPath $asset).Hash.Equals($expected,[StringComparison]::OrdinalIgnoreCase)) 'Installed original map corpus differs from source census.'
  $site=if($map -eq 'noe'){@{x=4975;z=4675;tileX=24;tileZ=23}}else{@{x=6475;z=7075;tileX=32;tileZ=35}}
  $mapRecord=@{source=$source;asset=@{path=$asset;sha256=$expected};candidate=$site;arms=@{}};$result.maps[$map]=$mapRecord
  $modes=if($SingleArm -ceq 'Pair'){@('off','on')}elseif($SingleArm -ceq 'On'){@('on')}else{@('off')}
  foreach($mode in $modes){
   Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Game ownership conflict.';Same-Pair $before (Pair)
   foreach($key in $keys){Restore-Environment $key $null};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
   $on=$mode -ceq 'on';$env:POSEIDON_RAIN_WATER_FINE=[string][int]$on;$env:WGR_RAIN_WATER_FINE=[string][int]$on
   $armDir=Join-Path $output ($map+'/'+$mode);$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Force $profile|Out-Null
   & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $profile -DlssMode 0 -MsaaSamples 4
   $cfg=Join-Path $profile 'graphics.cfg';[IO.File]::WriteAllText($cfg,([IO.File]::ReadAllText($cfg).Replace('brightness=1.6;','brightness=1;').Replace('fpsCap=0;','fpsCap=60;')))
   $mission=Join-Path $armDir ('ordinary.'+$map);New-Item -ItemType Directory -Force $mission|Out-Null;Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Destination (Join-Path $mission 'mission.sqm')
   $env:POSEIDON_USER_DIR=$profile;$arm=@{status='running';cpuFine=$on;rendererFine=$on;settings=$settings;missionSha256=(Get-FileHash -LiteralPath (Join-Path $mission 'mission.sqm')).Hash;profileSha256=(Get-FileHash -LiteralPath $cfg).Hash;captures=@{};gates=@{}};$mapRecord.arms[$mode]=$arm
   $log=Join-Path $armDir 'engine.log';$rpc=Join-Path $armDir 'harness.jsonl';$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
   $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
   $deadline=[DateTime]::UtcNow.AddSeconds(900+$RetirementSeconds/2);$p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $armDir 'stdout.txt') -RedirectStandardError (Join-Path $armDir 'stderr.txt');$null=$p.Handle;$arm.pid=$p.Id;$nextOwnershipCheck=[DateTime]::MinValue
   $until=[DateTime]::UtcNow.AddSeconds(120);do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Harness startup timeout.';Start-Sleep -Milliseconds 250}}while(!$client)
   $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
   Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Actual original mission not ready.'
   Exec 'player allowDamage false;0 setFog 0;0 setOvercast 0;0 setRain 0;setDate [1985,6,21,16,0];triSetBrightness 1;setAccTime 0'
   $null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
   $original=Bed $site.x $site.z;$sourceRevision=$original.heightRevision;Unedited $original
   $actualWorld=Send @{cmd='dev_mud';action='sample';x=$site.x;z=$site.z}
   Require ($actualWorld.world.Replace('\','/').ToLowerInvariant() -match ('(^|/)'+$map+'\.wrp$')) 'Actual mounted world differs from selected original fixture.';$arm.gates.actualWorld=$actualWorld.world
   $dry=Snapshot;Assert-Budget $dry.field;Require ($dry.field.volume -eq 0 -and !$dry.field.fineActive -and $dry.weather.liquidRain -eq 0) 'Fresh original world not dry.';$arm.gates.dry=$dry
   $candidate=@{x=(200*$site.tileX-12.5);z=(200*$site.tileZ-12.5);tileX=$site.tileX;tileZ=$site.tileZ};$arm.gates.candidateNative=Native-Tile $candidate 'actual-unedited-candidate-native-tile'
   View $site.x ($site.z-18) ($original.vertexHeight+4) 0 -12;Capture 'candidate-dry-near'
   Exec '0 setOvercast 1;0 setRain 1;setAccTime 4';Wait-Sim $RainSeconds;Exec 'setAccTime 0'
   $weather=Send @{cmd='weather_visibility'};$extraStart=Clock
   while(!(Rain-Ready $weather) -and (Clock)-$extraStart -lt 59000){Exec 'setAccTime 4';Wait-Sim 5;Exec 'setAccTime 0';$weather=Send @{cmd='weather_visibility'}}
   Require ((Clock)-$extraStart -le 60000 -and (Rain-Ready $weather)) 'Natural rain forcing did not become ready within additional60simsec.'
   $wet=Snapshot;Unedited $wet.source;Assert-Budget $wet.field;$fine=Fine $site.x $site.z;Assert-Owners $fine $wet.field $on $sourceRevision
   Require ($wet.field.rainVolume -gt 0 -and $wet.field.volume -gt 0) 'Actual natural rain produced no retained water.';$arm.gates.wet=$wet;$arm.gates.wetFine=$fine
   $arm.gates.candidateOwned=([bool]@($fine.owners|Where-Object{$_.tileX -eq $site.tileX -and $_.tileZ -eq $site.tileZ}).Count)
   $arm.gates.candidateSample=Send @{cmd='dev_rain_water';action='sample';x=$site.x;z=$site.z}
   View $site.x ($site.z-18) ($original.vertexHeight+4) 0 -12;Capture 'candidate-real-rain-near';View $site.x $site.z ($original.vertexHeight+35) 0 -89;Capture 'candidate-real-rain-top'
   if($on){
    $arm.gates.rendererReceipt=Receipt $fine $wet.field
    $arm.gates.naturalCycles=Natural-Cycles @(Lines) $sourceRevision $fine.tiles
    $owner=@($fine.owners|Where-Object{$_.tileX -eq $site.tileX -and $_.tileZ -eq $site.tileZ})|Select-Object -First 1
    if(!$owner){$owner=@($fine.owners|Sort-Object{[double]$_.maxDepth})|Select-Object -Last 1}
    $arm.gates.sampledOwnerNative=Native-Tile $owner 'actual-unedited-selected-owner-native-tile';$depths=Sample-Owner $owner;$arm.gates.depths=$depths
    $actualCorners=Get-Content -LiteralPath $arm.gates.sampledOwnerNative.path -Raw|ConvertFrom-Json
    foreach($representative in $depths.representatives){
     $cell=$representative.selected;$ix=[int](($cell.x-$owner.x)/6.25);$iz=[int](($cell.z-$owner.z)/6.25);$i=$iz*33+$ix
     $expectedCorners=@($actualCorners.vertices[$i][2],$actualCorners.vertices[$i+1][2],$actualCorners.vertices[$i+33][2],$actualCorners.vertices[$i+34][2])
     for($j=0;$j -lt 4;++$j){Require ($cell.corners[$j] -eq $expectedCorners[$j]) 'Published fine geometry differs from exact unedited native corners.'}
     Require ([Math]::Abs($cell.supportY-$representative.point.sample.terrainY) -lt .001) 'Horizontal fine query support differs from actual physical terrain.'
    }
    $focus=@($depths.points|Sort-Object{[double]$_.sample.depth})|Select-Object -Last 1;Require ($focus.sample.depth -gt 0) 'Sampled actual native owner has no wet cell-centre.'
    $arm.gates.focus=$focus;View $focus.x ($focus.z-18) ($focus.sample.terrainY+4) 0 -12;Capture 'natural-owned-water-near';View $focus.x $focus.z ($focus.sample.terrainY+35) 0 -89;Capture 'natural-owned-water-top'
   }
   $frozen=Snapshot;View ($site.x+350) ($site.z-350) ($original.vertexHeight+100) 315 -10;Capture 'camera-away';View $site.x ($site.z-18) ($original.vertexHeight+4) 0 -12;Capture 'candidate-camera-return';$returned=Snapshot;Frozen $frozen $returned;$arm.gates.cameraReturn=@{before=$frozen;after=$returned}
   $returnedFine=Fine $site.x $site.z;Require (($returnedFine.owners|ConvertTo-Json -Depth 10 -Compress) -ceq ($fine.owners|ConvertTo-Json -Depth 10 -Compress)) 'Camera/sample changed ownership or held water geometry.'
   Exec '0 setOvercast 0.6;0 setRain 0;setAccTime 4';Wait-Sim 3;Exec 'setAccTime 0';$stopped=Snapshot;Require ($stopped.weather.liquidRain -eq 0) 'Actual rain did not stop.';$arm.gates.stopped=$stopped
   Exec 'setAccTime 4';Wait-Sim $DrainSeconds;Exec 'setAccTime 0';$drained=Snapshot;Assert-Budget $drained.field;Unedited $drained.source
   Require ($drained.field.rainVolume -eq $stopped.field.rainVolume -and $drained.field.volume -lt $stopped.field.volume) 'Actual no-input runoff did not drain without mass invention.';$arm.gates.drained=$drained;Capture 'after-rain-drained-candidate'
   $retirement=@{status='not-claimed-wet-retention-is-legitimate';secondsRequested=$RetirementSeconds;states=@()}
   if($on -and $RetirementSeconds -gt 0){
    $start=Clock
    while((Clock)-$start -lt $RetirementSeconds*1000 -and (State).fineTiles -gt 0){Exec 'setAccTime 4';Wait-Sim 15;Exec 'setAccTime 0';$s=Snapshot;Assert-Budget $s.field;Unedited $s.source;Require ($s.field.rainVolume -eq $stopped.field.rainVolume) 'Retirement wait invented liquid input.';$retirement.states+=$s}
    $retirement.actualMs=(Clock)-$start;$last=State
    if($last.fineTiles -eq 0){
     Require ($last.fineActive -and $last.sourceReady -and $last.generation -gt $fine.generation) 'Zero owners destroyed backend or did not change topology.'
     $empty=Fine $site.x $site.z;Require (!$empty.valid -and $empty.sourceReady -and $empty.fineActive -and $empty.cells -eq 0 -and @($empty.owners).Count -eq 0 -and $empty.sourceRevision -eq $sourceRevision -and $empty.revision -eq $last.revision) 'Invalid retired ownership/source snapshot.'
     $retirement.snapshot=$empty;$retirement.rendererReceipt=Zero-OwnerReceipt $empty $last;$retirement.status='observed-exact-dry-full-owner-retirement'
    }
    else{$retirement.status='not-observed-within-bounded-dry-wait'}
   }
   $arm.gates.retirement=$retirement;Close-Owned;Same-Pair $before (Pair);$client.Dispose();$client=$null;$p=$null;$arm.status='source-clock-mass-owner-lifecycle-passed-visual-performance-pending'
  }
 }
 $result.status='bounded-ordinary-no-edit-source-lifecycle-passed-visual-performance-pending'
}catch{$result.status='failed';$result.error=$_.Exception.Message;$result.errorSource=@{line=$_.InvocationInfo.ScriptLineNumber;position=$_.InvocationInfo.PositionMessage;stack=$_.ScriptStackTrace}}
finally{
 if($p -and !$p.HasExited -and $client){try{Close-Owned}catch{$result.cleanupError=$_.Exception.Message;$result.status='failed'}}
 if($client){$client.Dispose()};if($p -and !$p.HasExited){$result.status='failed';$result.forcedCleanup=$true;Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue;$null=$p.WaitForExit(10000)}
 foreach($key in $keys){Restore-Environment $key $savedEnv[$key]}
 try{$result.after=Pair;Same-Pair $before $result.after}catch{$result.status='failed';$result.provenanceError=$_.Exception.Message}
 $result|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Actual unedited natural-water record: $output"
 if($result.status -eq 'failed'){throw ($result.error+' '+$result.cleanupError+' '+$result.provenanceError)}
}
