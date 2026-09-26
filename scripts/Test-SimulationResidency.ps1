param([switch]$Disabled,[switch]$Capacity,[switch]$MovedWatcher,[string]$Label='simulation-residency',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',[string]$Python='C:\Program Files\Python311\python.exe')
$ErrorActionPreference='Stop'
if (@($Disabled,$Capacity,$MovedWatcher | Where-Object {$_}).Count -gt 1) {throw 'Disabled, Capacity and MovedWatcher are separate scenarios'}
if (!$env:LOCK_OWNER) {
 $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python)
 foreach ($flag in @('Disabled','Capacity','MovedWatcher')) {if ((Get-Variable $flag -ValueOnly)) {$argv+=('-'+$flag)}}
 $env:LOCK_OWNER='simulation residency fixture'
 try {& 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Simulation fixture exited $LASTEXITCODE"}} finally {Remove-Item Env:LOCK_OWNER}
 return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$fixture=Join-Path $out 'fixture'
$mission=Join-Path $out 'simulation-fixture.eden'
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
New-Item -ItemType Directory -Force $mission,$env:POSEIDON_USER_DIR | Out-Null
$fixtureArgs=@($fixture,'--absolute-model-path')
if ($Capacity) {$fixtureArgs+=@('--model-count','257')}
& $Python (Join-Path $PSScriptRoot 'streaming/build_simulation_residency_fixture.py') @fixtureArgs
if ($LASTEXITCODE) {throw 'Original fixture generation failed'}
@'
version=11;
class Mission {
 randomSeed=1234;
 class Intel { year=1985; month=6; day=21; hour=12; minute=0; };
 class Groups { items=2;
  class Item0 { side="WEST"; class Vehicles { items=1; class Item0 {
   position[]={100,0,100}; id=0; side="WEST"; vehicle="SoldierWB";
   player="PLAYER COMMANDER"; leader=1; skill=1;
   init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""";
  }; }; };
  class Item1 { side="WEST"; class Vehicles { items=1; class Item0 {
   position[]={1190,0,1200}; id=1; side="WEST"; vehicle="SoldierWB";
   text="streamFixtureActor"; leader=1; skill=1;
   init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""; this disableAI ""TARGET""";
  }; }; };
 };
};
class Intro { randomSeed=1; class Intel {}; };
class OutroWin { randomSeed=2; class Intel {}; };
class OutroLoose { randomSeed=3; class Intel {}; };
'@ | Set-Content (Join-Path $mission 'mission.sqm')
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$env:WGR_SIMULATION_RESIDENCY=if ($Disabled) {'0'} else {'1'}
$env:WGR_SIMULATION_RESIDENCY_TEST=if ($Disabled) {'0'} else {'1'}
$env:WGR_OBJECT_STREAM_WINDOW_GROWTH='0'
$env:WGR_OBJECT_STREAM_RADIUS_CELLS='8'
$env:WGR_OBJECT_STREAM_GPU_BUDGET='0'
$env:WGR_OBJECT_STREAM_PBO='0'
$env:WGR_NATIVE_DDS_PREPARE='0'
$env:WGR_NATIVE_DDS_BC3_ONLY='0'
$env:WGR_ADAPTIVE_TEXTURE_DETAIL='0'
foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','WGR_WATER_CURLING_BREAKER','WGR_LOD_GOVERNOR_RANGE')) {Remove-Item ('Env:'+ $key) -ErrorAction SilentlyContinue}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
$log=Join-Path $out 'engine.log'
@{scriptSha256=(Get-FileHash $PSCommandPath).Hash;head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));disabled=[bool]$Disabled;fixture=(Get-Content (Join-Path $fixture 'fixture.json') -Raw | ConvertFrom-Json);hashes=@(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe'),(Join-Path $GameDir 'wgpu_renderer.dll'))} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'provenance.json')
$p=$null;$client=$null
try {
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-world-freefly','100','100','80','45','-25','--test-mission',('"'+$mission+'"'),'--test-world',('"'+(Join-Path $fixture 'simulation-residency.wrp')+'"'),'--log-file',('"'+$log+'"'))
 $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(90)
 while (!$client.Connected) {try {$client.Connect('127.0.0.1',$port)} catch {if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw};Start-Sleep -Milliseconds 250}}
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command) {
  $writer.WriteLine(($command | ConvertTo-Json -Compress))
  do {$line=$reader.ReadLine();if ($null -eq $line) {throw 'Harness closed'};$reply=$line | ConvertFrom-Json} while ($null -eq $reply.ok)
  ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl');$line | Add-Content (Join-Path $out 'harness.jsonl')
  if (!$reply.ok) {throw $line};return $reply
 }
 function State {Send @{cmd='stream_simulation_residency'}}
 function Ids {Send @{cmd='stream_identity_probe';ids=@(1001,1002)}}
 function Query($action,$from=@(1190,11.5,1200),$to=@(1210,11.5,1200),$radius=0.01,$purpose=1) {
  Send @{cmd='stream_simulation_query';actor='streamFixtureActor';action=$action;from=$from;to=$to;radius=$radius;purpose=$purpose}
 }
 function Wait-State($predicate,$description) {
  $until=[DateTime]::UtcNow.AddSeconds(90)
  do {
   Start-Sleep -Milliseconds 500
   if ($p.HasExited) {throw 'Game exited during simulation fixture'}
   $r=State
   if (& $predicate $r) {return $r}
   if ($r.status -in @('Invalid','CapacityExceeded','WrongOwner')) {throw "Simulation refused $description : $($r | ConvertTo-Json -Compress)"}
  } while ([DateTime]::UtcNow -lt $until)
  throw "Simulation did not establish $description : $($r | ConvertTo-Json -Compress)"
 }
 $actor=Send @{cmd='eval';code='!(isNull streamFixtureActor)'}
 if ($actor.result -notmatch 'true') {throw 'Remote authoritative actor did not initialize'}
 $null=Send @{cmd='eval';code='getPos streamFixtureActor'}
 if ($Disabled) {
  Start-Sleep -Seconds 4
  $cold=State;$coldIds=Ids
  if ($cold.status -ne 'Disabled' -or $cold.ownerPumps -ne 0 -or @($coldIds.objects | Where-Object {$_.present}).Count) {throw 'Default-off fixture performed simulation work or loaded remote walls'}
  $result=@{cold=$cold;coldIds=$coldIds}
 } elseif ($Capacity) {
  $cold=Wait-State {param($r) $r.status -eq 'CapacityExceeded'} 'pre-admission model capacity refusal'
  $coldIds=Ids
  if ($cold.models -ne 257 -or $cold.inventoryRows -ne 0 -or $cold.preparedModels -ne 0 -or $cold.leasedPlacements -ne 0 -or $cold.created -ne 0 -or @($coldIds.objects | Where-Object {$_.present}).Count) {throw 'Capacity refusal admitted a partial world'}
  $result=@{cold=$cold;coldIds=$coldIds;capacity=$true}
 } else {
  $cold=Wait-State {param($r) $r.status -eq 'Ready' -and $r.inventoryRows -eq 2 -and $r.preparedModels -eq 1 -and $r.leasedPlacements -eq 2 -and $r.readyRegions -ge 2 -and !$r.pendingRegions -and !$r.unknownRegions -and !$r.refusedRegions} 'complete actor region'
  $coldIds=Ids
  foreach ($wall in $coldIds.objects) {if (!$wall.present -or $wall.visualResident -or !$wall.shapeVisualDeferred -or $wall.normalVertexBuffers -ne 0 -or !$wall.hasGeometry -or !$wall.hasFireGeometry) {throw 'Remote wall was absent or visually uploaded'}}
  $residency=Send @{cmd='stream_residency'}
  if ($residency.required -ne 0) {throw 'Actor admission used permanent required IDs'}
  $query=@{cmd='stream_fire_probe';from=@(1190,11.5,1200);to=@(1210,11.5,1200)}
  $hits=Send $query
  if (!@($hits.contacts | Where-Object {$_.id -eq 1001}).Count) {throw 'Cold actor wall did not obstruct fire'}
  $queryBefore=Query 'status'
  if ($queryBefore.status -ne 'Pending') {throw 'Neighborhood readiness was reused as a fire query'}
  $invalidPurpose=Query 'queue' -purpose 2
  if ($invalidPurpose.status -ne 'Invalid') {throw 'Unsupported query purpose was admitted'}
  $null=Query 'queue'
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {Start-Sleep -Milliseconds 250;$queryReady=Query 'status'} while ($queryReady.status -eq 'Pending' -and [DateTime]::UtcNow -lt $until)
  if ($queryReady.status -ne 'Ready') {throw 'Exact fire residency did not complete'}
  $wrongRay=Query 'status' -from @(100,11.5,100) -to @(120,11.5,100)
  if ($wrongRay.status -ne 'Pending') {throw 'Unrelated ray inherited fire readiness'}
  $independent=State
  if ($independent.regions -lt 2 -or $independent.readyRegions -lt 2 -or $independent.queryRegions -ne 1 -or $independent.readyQueries -ne 1) {throw 'Query channel replaced automatic neighborhood'}
  $null=Query 'release'
  $queryReleased=State
  if ($queryReleased.queryRegions -ne 0 -or $queryReleased.leasedPlacements -ne 2) {throw 'Query release retired another interest owner'}
  if ($MovedWatcher) {
   $null=Send @{cmd='stream_existing_move';id=1002;position=@(100,11.5,100)}
   $moved=State;$movedIds=Ids
   if ($moved.status -ne 'Unknown' -or $moved.liveWatchedPlacements -ne 2 -or $movedIds.objects[1].x -ne 100 -or $movedIds.objects[1].z -ne 100) {throw 'Movement outside authored bins reused a stale complete-world certificate'}
   $movedQuery=Query 'status' -from @(100,11.5,100) -to @(120,11.5,100)
   if ($movedQuery.status -ne 'Unknown') {throw 'Moved live placement was ignored by an unrelated query'}
   $result=@{cold=$cold;coldIds=$coldIds;queryBefore=$queryBefore;queryReady=$queryReady;invalidPurpose=$invalidPurpose;wrongRay=$wrongRay;independent=$independent;queryReleased=$queryReleased;moved=$moved;movedIds=$movedIds;movedQuery=$movedQuery;movedWatcher=$true}
  } else {
  $pose=Send @{cmd='eval';code='triFreeFlyPose "1188 1200 18 90 -8"'}
  if ($pose.result.Trim('"') -ne 'OK') {throw 'Near camera pose refused'}
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {Start-Sleep -Milliseconds 500;$visual=Ids} while ((!$visual.objects[0].visualResident -or $visual.objects[0].normalVertexBuffers -eq 0) -and [DateTime]::UtcNow -lt $until)
  if (!$visual.objects[0].visualResident -or $visual.objects[0].normalVertexBuffers -eq 0) {throw 'Camera did not promote actor wall'}
  $null=Send @{cmd='screenshot';path=(Join-Path $out 'visual.png')}
  $null=Send @{cmd='eval';code='triFreeFlyPose "100 100 80 45 -25"'}
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {Start-Sleep -Milliseconds 500;$away=Ids} while ($away.objects[0].visualResident -and [DateTime]::UtcNow -lt $until)
  if (!$away.objects[0].present -or $away.objects[0].visualResident) {throw 'Camera eviction lost actor lease or did not retire visual residency'}
  $awayHits=Send $query
  if (!@($awayHits.contacts | Where-Object {$_.id -eq 1001}).Count) {throw 'Camera eviction lost actor obstruction'}
  # Complete an unmodified moving-actor replacement before introducing state
  # that correctly makes the immutable model contract globally Unknown.
  $null=Send @{cmd='exec';code='streamFixtureActor setPos [100,100,0]'}
  $movedAway=Wait-State {param($r) $r.status -eq 'Ready' -and $r.leasedPlacements -eq 0 -and !$r.pendingRegions} 'moving actor replacement'
  $movedAwayIds=Ids
  if (@($movedAwayIds.objects | Where-Object {$_.present}).Count) {throw 'Moving actor retained unmodified remote placements'}
  $null=Send @{cmd='exec';code='streamFixtureActor setPos [1190,1200,0]'}
  $reacquired=Wait-State {param($r) $r.status -eq 'Ready' -and $r.leasedPlacements -eq 2 -and !$r.pendingRegions} 'moving actor reacquisition'
  $damage=Send @{cmd='stream_existing_damage';id=1002;damage=0.25}
  if (!$damage.mustBeSaved -or [Math]::Abs($damage.rawDamage-0.25) -gt 0.001) {throw 'Existing object mutation did not create saved state'}
  $mutated=State
  if ($mutated.status -ne 'Unknown') {throw 'Mutable geometry retained an immutable-world certificate'}
  $null=Send @{cmd='exec';code='deleteVehicle streamFixtureActor'}
  $released=Wait-State {param($r) $r.leasedPlacements -eq 0 -and !$r.pendingRegions} 'transient lease release'
  $releasedIds=Ids
  if ($releasedIds.objects[0].present -or !$releasedIds.objects[1].present -or !$releasedIds.objects[1].mustBeSaved -or [Math]::Abs($releasedIds.objects[1].rawDamage-0.25) -gt 0.001) {throw 'Lease retirement lost mutation or retained unmodified wall'}
  $final=Send @{cmd='stream_residency'}
  if ($final.required -ne 0) {throw 'Fixture permanently pinned an object'}
  if ($released.status -ne 'Unknown' -or $released.liveWatchedPlacements -ne 1) {throw 'Zero-lease mutable placement lost its freshness watcher'}
  $result=@{cold=$cold;coldIds=$coldIds;residency=$residency;hits=$hits;queryBefore=$queryBefore;queryReady=$queryReady;invalidPurpose=$invalidPurpose;wrongRay=$wrongRay;independent=$independent;queryReleased=$queryReleased;visual=$visual;away=$away;awayHits=$awayHits;movedAway=$movedAway;movedAwayIds=$movedAwayIds;reacquired=$reacquired;damage=$damage;mutated=$mutated;released=$released;releasedIds=$releasedIds;final=$final}
  }
 }
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(15000) -or $p.ExitCode) {throw 'Game failed to exit cleanly'}
 if (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error'}
 $result.passed=$true;$result.disabled=[bool]$Disabled
 $result | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
 Write-Output "Simulation residency evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id -Force}
}
