param([switch]$Disabled,[switch]$Capacity,[switch]$ActiveCapacity,[switch]$MovedWatcher,[switch]$LargeWall,[switch]$SourceDiagnostic,[switch]$PositiveWarming,[switch]$ColdPositive,[switch]$RegisteredWarming,[switch]$SharedMeshLifetime,[switch]$SharedMeshLedger,[switch]$StandaloneMesh,[string]$Label='simulation-residency',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',[string]$Python='C:\Program Files\Python311\python.exe')
$ErrorActionPreference='Stop'
if ($SharedMeshLedger -and !$SharedMeshLifetime) {throw 'Shared mesh ledger validation requires the private lifetime fixture'}
if ($StandaloneMesh -and (!$SharedMeshLifetime -or !$SharedMeshLedger)) {throw 'Standalone mesh validation requires the private lifetime fixture and its owner ledger'}
if (@($Disabled,$Capacity,$ActiveCapacity,$MovedWatcher,$LargeWall,$SourceDiagnostic,$PositiveWarming,$ColdPositive,$RegisteredWarming,$SharedMeshLifetime | Where-Object {$_}).Count -gt 1) {throw 'Simulation fixture switches are separate scenarios'}
if (!$env:LOCK_OWNER) {
 $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python)
 foreach ($flag in @('Disabled','Capacity','ActiveCapacity','MovedWatcher','LargeWall','SourceDiagnostic','PositiveWarming','ColdPositive','RegisteredWarming','SharedMeshLifetime','SharedMeshLedger','StandaloneMesh')) {if ((Get-Variable $flag -ValueOnly)) {$argv+=('-'+$flag)}}
 $env:LOCK_OWNER='simulation residency fixture'
 try {& 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Simulation fixture exited $LASTEXITCODE"}} finally {Remove-Item Env:LOCK_OWNER}
 return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$fixture=Join-Path $out 'fixture'
if ($SourceDiagnostic -or $ColdPositive -or $RegisteredWarming) {$fixture=Join-Path ([IO.Path]::GetTempPath()) ('op-src-'+[guid]::NewGuid().ToString('N').Substring(0,8))}
$mission=Join-Path $out 'simulation-fixture.eden'
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
New-Item -ItemType Directory -Force $mission,$env:POSEIDON_USER_DIR | Out-Null
$fixtureArgs=@($fixture,'--absolute-model-path')
if ($Capacity) {$fixtureArgs+=@('--model-count','8193')}
if ($ActiveCapacity) {$fixtureArgs+=@('--model-count','257','--reference-all-models')}
if ($PositiveWarming) {$fixtureArgs+=@('--model-count','257','--reference-all-models','--positive-warming')}
if ($RegisteredWarming) {$fixtureArgs+=@('--model-count','257','--reference-all-models','--registered-warming')}
if ($LargeWall) {$fixtureArgs+='--large-wall'}
if ($SourceDiagnostic) {& $Python (Join-Path $PSScriptRoot 'streaming/build_source_diagnostic_fixture.py') $fixture}
elseif ($ColdPositive) {& $Python (Join-Path $PSScriptRoot 'streaming/build_positive_cold_fixture.py') $fixture}
else {& $Python (Join-Path $PSScriptRoot 'streaming/build_simulation_residency_fixture.py') @fixtureArgs}
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
$env:WGR_SIMULATION_RESIDENCY=if ($Disabled -or $SourceDiagnostic -or $SharedMeshLifetime) {'0'} else {'1'}
$env:WGR_SIMULATION_RESIDENCY_TEST=if ($Disabled -or $SourceDiagnostic -or $SharedMeshLifetime) {'0'} else {'1'}
$env:WGR_SHARED_MESH_LIFETIME_FIXTURE=if ($SharedMeshLifetime) {'1'} else {'0'}
$env:WGR_GEOMETRY_OWNER_LEDGER=if ($SharedMeshLedger) {'1'} else {'0'}
$env:WGR_SIMULATION_POSITIVE_COLD=if ($ColdPositive) {'1'} else {'0'}
$env:WGR_OBJECT_STREAM_WINDOW_GROWTH='0'
$env:WGR_OBJECT_STREAM_RADIUS_CELLS='8'
$env:WGR_OBJECT_STREAM_GPU_BUDGET='0'
$env:WGR_OBJECT_STREAM_PBO='0'
if ($RegisteredWarming) {$env:WGR_OBJECT_STREAM_EVICT_CACHE='0'}
$env:WGR_NATIVE_DDS_PREPARE='0'
$env:WGR_NATIVE_DDS_BC3_ONLY='0'
$env:WGR_ADAPTIVE_TEXTURE_DETAIL='0'
foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','WGR_WATER_CURLING_BREAKER','WGR_LOD_GOVERNOR_RANGE')) {Remove-Item ('Env:'+ $key) -ErrorAction SilentlyContinue}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
$log=Join-Path $out 'engine.log'
@{scriptSha256=(Get-FileHash $PSCommandPath).Hash;head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));disabled=[bool]$Disabled;registeredWarming=[bool]$RegisteredWarming;shapeCacheLimit=$env:WGR_OBJECT_STREAM_EVICT_CACHE;fixture=(Get-Content (Join-Path $fixture 'fixture.json') -Raw | ConvertFrom-Json);hashes=@(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe'),(Join-Path $GameDir 'wgpu_renderer.dll'))} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'provenance.json')
$p=$null;$client=$null;$heartbeatProof=$null
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
 if ($SharedMeshLifetime) {
  $simulationOff=State
  if ($simulationOff.status -ne 'Disabled' -or $simulationOff.ownerPumps -ne 0) {throw 'Mesh lifetime fixture unexpectedly enabled simulation residency'}
  $null=Send @{cmd='eval';code='triFreeFlyPose "1188 1200 18 90 -8"'}
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {Start-Sleep -Milliseconds 250;$meshSource=Ids} while ((!$meshSource.objects[0].visualResident -or $meshSource.objects[0].normalVertexBuffers -eq 0) -and [DateTime]::UtcNow -lt $until)
  if (!$meshSource.objects[0].visualResident -or $meshSource.objects[0].normalVertexBuffers -eq 0) {throw 'Original mesh fixture source was not already rendered'}
  function Mesh($action) {Send @{cmd='shared_mesh_lifetime_test';action=$action;id=1001}}
  function Observe-Mesh($expectedState,$borrowers,$producerOwned,$expectedHandle='') {
   $until=[DateTime]::UtcNow.AddSeconds(15);$priorCuts=@()
   do {
    $request=Mesh 'observe'
    do {Start-Sleep -Milliseconds 100;$observed=Mesh 'status'} while ($observed.status -eq 'Pending' -and [DateTime]::UtcNow -lt $until)
    # The final CPU destructor uses the uploader queue. An earlier copied fact
    # cut may still report Present; wait for a new owner cut, without GPU waits.
    if ($expectedState -eq 2 -and $observed.status -eq 'Ready' -and $observed.meshState -eq 1) {$priorCuts+=,$observed}
    else {break}
   } while ([DateTime]::UtcNow -lt $until)
   if ($observed.status -ne 'Ready' -or $observed.observedRequestId -ne $request.requestId -or
       $observed.meshState -ne $expectedState -or $observed.cpuBorrowers -ne $borrowers -or
       $observed.producerOwned -ne $producerOwned -or !$observed.sourceCopied -or !$observed.privateAllocation -or
       $observed.knownPayloadBytes -le 0 -or $observed.knownPayloadBytes -gt 1048576 -or
       ($expectedHandle -and $observed.rendererHandle -ne $expectedHandle)) {
    throw "Unexpected private mesh lifetime cut: $($observed | ConvertTo-Json -Compress)"
   }
   if ($SharedMeshLedger -and (!$observed.ledgerEnabled -or !$observed.ledgerRecordObserved -or
       $observed.ledgerCpuBorrowers -ne $borrowers -or $observed.ledgerProducerOwned -ne $producerOwned -or
       $observed.ledgerScheduledRetired -ne ($expectedState -eq 2))) {throw "Shared retirement ledger disagreed with its owner cut: $($observed | ConvertTo-Json -Compress)"}
   $observed | Add-Member -NotePropertyName priorCuts -NotePropertyValue $priorCuts
   return $observed
  }
  $meshCases=@()
  if ($StandaloneMesh) {
   $null=Mesh 'beginStandalone';$created=Observe-Mesh 1 1 $false
   if (!$created.standalone -or $created.witnessKnownMetadataBytes -le 0 -or
       $created.witnessKnownMetadataBytes -gt 1048576 -or $created.witnessPeakKnownMetadataBytes -gt 1048576 -or
       $created.witnessCapacityRefused -ne 0) {throw 'Standalone diagnostic witness exceeded its bounded metadata allowance'}
   $null=Mesh 'dropOlder';$last=Observe-Mesh 2 0 $false $created.rendererHandle
   if (!$last.standalone -or $last.witnessKnownMetadataBytes -gt 1048576 -or
       $last.witnessPeakKnownMetadataBytes -gt 1048576 -or $last.witnessCapacityRefused -ne 0) {throw 'Standalone diagnostic witness metadata accounting failed'}
   $meshCases+=@{order='standalone';created=$created;last=$last}
   $null=Mesh 'abort'
  } else { foreach ($order in @('newest-first','producer-first')) {
   $null=Mesh 'begin';$created=Observe-Mesh 1 2 $true
   $handle=$created.rendererHandle
   if ($order -eq 'newest-first') {
    $null=Mesh 'dropNewest';$first=Observe-Mesh 1 1 $true $handle
    $null=Mesh 'dropProducer';$second=Observe-Mesh 1 1 $false $handle
   } else {
    $null=Mesh 'dropProducer';$first=Observe-Mesh 1 2 $false $handle
    $null=Mesh 'dropNewest';$second=Observe-Mesh 1 1 $false $handle
   }
   $null=Mesh 'dropOlder';$last=Observe-Mesh 2 0 $false $handle
   $meshCases+=@{order=$order;created=$created;first=$first;second=$second;last=$last}
   $null=Mesh 'abort'
  } }
  $afterFixture=Ids;$ordinary=Send @{cmd='stream_residency'}
  if (!$afterFixture.objects[0].present -or !$afterFixture.objects[0].visualResident -or $ordinary.required -ne 0) {throw 'Private mesh fixture changed ordinary world identity or pinned its source'}
  $result=@{kind='shared-retained-mesh-lifetime';simulationOff=$simulationOff;source=$meshSource;cases=$meshCases;after=$afterFixture;ordinary=$ordinary;
    standalone=$StandaloneMesh.IsPresent;
    limitation='Actual private copied mesh through normal renderer drains; Present/Absent facts are record lifetime, not pixel drawing, complete GPU ownership, queue completion or device memory release.'}
 } elseif ($SourceDiagnostic) {
  $cold=State; $coldIds=Ids; $baseline=Send @{cmd='stream_residency'}
  if ($cold.status -ne 'Disabled' -or $cold.ownerPumps -ne 0 -or $baseline.sourceEnvelopeBytes -ne 0 -or $baseline.sourceEnvelopeScans -ne 0 -or @($coldIds.objects | Where-Object {$_.present}).Count) {throw 'Ordinary disabled fixture performed source or simulation work'}
  $unrequested=Send @{cmd='stream_source_diagnostic';modelIndex=0}
  if ($unrequested.status -ne 'NotRequested' -or $unrequested.diagnosticActive) {throw 'Read-only diagnostic poll created demand'}
  $requested=@(); $snapshots=@()
  for ($i=0;$i -lt 8;$i++) {
   $r=Send @{cmd='stream_source_diagnostic';modelIndex=$i;action='request'}
   if ($r.requestResult -ne 'Requested') {throw "Selected source request refused: $($r | ConvertTo-Json -Compress)"}
   $requested+=,$r
  }
  $sourceCapacityResult=Send @{cmd='stream_source_diagnostic';modelIndex=8;action='request'}
  if ($sourceCapacityResult.requestResult -ne 'Capacity') {throw 'Selected diagnostics exceeded eight slots'}
  for ($i=0;$i -lt 8;$i++) {
   $until=[DateTime]::UtcNow.AddSeconds(15)
   do {Start-Sleep -Milliseconds 100;$r=Send @{cmd='stream_source_diagnostic';modelIndex=$i}} while ($r.status -eq 'Pending' -and [DateTime]::UtcNow -lt $until)
   if ($r.status -ne 'LatestSnapshot' -or !$r.attempted -or !$r.sourceEvidence -or !$r.plainSourceFacts -or $r.pendingDemand -or $r.modelIdentity -ne $requested[$i].modelIdentity -or $r.inventoryGeneration -le 0 -or $r.latestSnapshotClassObservation -notin @('EmptyClassNow','Unsupported')) {throw "Paired original source snapshot missing: $($r | ConvertTo-Json -Compress)"}
   $snapshots+=,$r
  }
  $late=Send @{cmd='stream_source_diagnostic';modelIndex=0;action='request'}
  if ($late.requestResult -ne 'TooLate' -or $late.status -ne 'LatestSnapshot') {throw 'Late request upgraded or discarded an existing snapshot'}
  $after=Send @{cmd='stream_residency'}; $afterIds=Ids; $simulationAfter=State
  if ($after.sourceEnvelopeScans -ne 8 -or $after.sourceEnvelopePublished -ne 8 -or $after.sourceEnvelopeBytes -le 0 -or $after.required -ne 0 -or $after.resident -ne $baseline.resident -or @($afterIds.objects | Where-Object {$_.present}).Count -or $simulationAfter.status -ne 'Disabled' -or $simulationAfter.ownerPumps -ne 0) {throw 'Source diagnostics created geometry, pinned identities or simulation work'}
  Start-Sleep -Seconds 31
  $expired=Send @{cmd='stream_source_diagnostic';modelIndex=0}
  if ($expired.status -ne 'Expired' -or $expired.pendingDemand -or $expired.diagnosticActive) {throw 'Owner maintenance did not expire diagnostic demand with simulation disabled'}
  # TTL removes diagnostic demand, not ordinary preparer cache. Exercise the
  # real camera DropStale path while keeping the remote objects out of range.
  $null=Send @{cmd='eval';code='triFreeFlyPose "200 200 80 45 -25"'}
  $until=[DateTime]::UtcNow.AddSeconds(15)
  do {Start-Sleep -Milliseconds 100;$retired=Send @{cmd='stream_residency'}} while (($retired.readyPayloadBytes -ne 0 -or $retired.parseReservedBytes -ne 0) -and [DateTime]::UtcNow -lt $until)
  if ($retired.readyPayloadBytes -ne 0 -or $retired.parseReservedBytes -ne 0 -or $retired.required -ne 0 -or $retired.resident -ne $baseline.resident) {throw 'Normal cancellation retained diagnostic-only Ready payload or admitted remote objects'}
  $result=@{sourceDiagnostic=$true;cold=$cold;coldIds=$coldIds;baseline=$baseline;unrequested=$unrequested;requested=$requested;snapshots=$snapshots;capacity=$sourceCapacityResult;late=$late;after=$after;afterIds=$afterIds;simulationAfter=$simulationAfter;expired=$expired;retiredAfterCameraRecentre=$retired}
 } elseif ($Disabled) {
  Start-Sleep -Seconds 4
  $cold=State;$coldIds=Ids
  if ($cold.status -ne 'Disabled' -or $cold.ownerPumps -ne 0 -or @($coldIds.objects | Where-Object {$_.present}).Count) {throw 'Default-off fixture performed simulation work or loaded remote walls'}
  $result=@{cold=$cold;coldIds=$coldIds}
 } elseif ($RegisteredWarming) {
  $cold=Wait-State {param($r) $r.status -eq 'CapacityExceeded' -and $r.metadataComplete} 'unregistered 257-model inventory'
  $coldIds=Ids
  if ($cold.models -ne 257 -or $cold.referencedModels -ne 257 -or $cold.inventoryRows -ne 258 -or $cold.preparedModels -ne 0 -or $cold.registeredMetadataEverComplete -or $cold.queryBorrowedModels -ne 0 -or @($coldIds.objects | Where-Object {$_.present}).Count) {throw 'Cold registered fixture eagerly retained geometry'}
  $null=Query 'queue';$coldQuery=Query 'status';$coldCollision=Query 'collide'
  if ($coldQuery.status -ne 'CapacityExceeded' -or $coldCollision.status -ne 'CapacityExceeded' -or @($coldCollision.contacts).Count) {throw 'Incomplete registered inventory lent query readiness'}
  # Both clusters are inside the ordinary eight-cell camera window. Camera
  # objects, rather than this harness, supply the strong owners for 257 shapes.
  $null=Send @{cmd='eval';code='triFreeFlyPose "850 850 200 225 -35"'}
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {
   Start-Sleep -Milliseconds 500;$warmed=State;$warmIds=Ids;$warmResidency=Send @{cmd='stream_residency'}
   if ($p.HasExited) {throw 'Game exited during registered camera warming'}
  } while (($warmed.status -ne 'Ready' -or $warmed.queryBorrowedEntries -ne 2 -or $warmed.readyQueries -ne 1) -and [DateTime]::UtcNow -lt $until)
  if ($warmed.status -ne 'Ready' -or !$warmed.registeredMetadataEverComplete -or $warmed.preparedModels -ne 0 -or $warmed.queryBorrowedModels -ne 1 -or $warmed.queryBorrowedEntries -ne 2 -or $warmed.registeredBorrowedModels -ne 1 -or $warmed.registeredBorrowShapeCharge -le 0 -or $warmed.registeredBorrowShapeCharge -gt 268435456 -or $warmResidency.required -ne 0) {throw 'Registered warming did not establish complete metadata with bounded selected ownership'}
  if (@($warmIds.objects | Where-Object {$_.present -and $_.visualResident -and $_.hasGeometry -and $_.hasFireGeometry}).Count -ne 2) {throw 'Ordinary camera did not create both query walls'}
  # Aggregate metadata Ready and borrowed entries do not prove publication of
  # this exact Fire query. Each status/collision operation validates afresh and
  # may independently exhaust its unchanged two-millisecond owner deadline.
  $until=[DateTime]::UtcNow.AddSeconds(60);$warmCollision=$null
  do {
   $null=Query 'queue' # Continuing explicit interest; status/collision polls are not heartbeats.
   $warmQuery=Query 'status'
   if ($warmQuery.status -eq 'Ready') {$warmCollision=Query 'collide'}
   if ($warmQuery.status -notin @('Ready','Pending') -or ($null -ne $warmCollision -and $warmCollision.status -notin @('Ready','Pending'))) {
    $queryState=State
    throw "Registered exact Fire query refused: status=$($warmQuery | ConvertTo-Json -Compress), collision=$($warmCollision | ConvertTo-Json -Compress), residency=$($queryState | ConvertTo-Json -Compress)"
   }
   if ($warmQuery.status -eq 'Ready' -and $null -ne $warmCollision -and $warmCollision.status -eq 'Ready') {break}
   if ($p.HasExited) {throw 'Game exited while awaiting registered exact Fire readiness'}
   Start-Sleep -Milliseconds 250
  } while ([DateTime]::UtcNow -lt $until)
  if ($warmQuery.status -ne 'Ready' -or $null -eq $warmCollision -or $warmCollision.status -ne 'Ready') {
   $queryState=State
   throw "Registered exact Fire query did not converge within 60 seconds; no obstruction result established: status=$($warmQuery | ConvertTo-Json -Compress), collision=$($warmCollision | ConvertTo-Json -Compress), residency=$($queryState | ConvertTo-Json -Compress)"
  }
  if (@($warmCollision.contacts | Where-Object {$_.id -eq 1001}).Count -ne 1) {throw 'Ready registered Fire collision missed its authored obstruction or duplicated the contact'}
  $null=Query 'release';$queryReleased=State
  if ($queryReleased.queryRegions -ne 0 -or $queryReleased.queryBorrowedModels -ne 1 -or $queryReleased.queryBorrowedEntries -ne 2) {throw 'Independent Fire release retired actor ownership'}
  # Cache=0 is explicit fixture isolation. The camera must actually destroy
  # off-query owners; metadata may not retain all models to keep Ready alive.
  $null=Send @{cmd='eval';code='triFreeFlyPose "100 100 80 45 -25"'}
  $until=[DateTime]::UtcNow.AddSeconds(30)
  do {Start-Sleep -Milliseconds 500;$away=State;$awayIds=Ids} while (($away.status -ne 'Unknown' -or @($awayIds.objects | Where-Object {$_.visualResident}).Count) -and [DateTime]::UtcNow -lt $until)
  if ($away.status -ne 'Unknown' -or !$away.registeredMetadataEverComplete -or $away.queryBorrowedModels -ne 1 -or $away.queryBorrowedEntries -ne 2 -or @($awayIds.objects | Where-Object {$_.present -and !$_.visualResident -and $_.hasFireGeometry}).Count -ne 2) {throw 'Off-query weak expiry retained Ready or lost selected actor geometry'}
  $null=Query 'queue';$awayQuery=Query 'status';$awayCollision=Query 'collide'
  if ($awayQuery.status -ne 'Unknown' -or $awayCollision.status -ne 'Unknown' -or @($awayCollision.contacts).Count) {throw 'Expired global metadata published partial Fire clearance'}
  $damage=Send @{cmd='stream_existing_damage';id=1002;damage=0.25}
  if (!$damage.mustBeSaved -or [Math]::Abs($damage.rawDamage-0.25) -gt 0.001) {throw 'Registered selected mutation was not retained'}
  $null=Query 'release';$null=Send @{cmd='exec';code='deleteVehicle streamFixtureActor'}
  $released=Wait-State {param($r) $r.leasedPlacements -eq 0 -and $r.queryBorrowedModels -eq 0 -and $r.queryBorrowedEntries -eq 0 -and $r.registeredBorrowedModels -eq 0 -and $r.registeredBorrowShapeCharge -eq 0} 'registered selected ownership retirement'
  $releasedIds=Ids;$final=Send @{cmd='stream_residency'}
  if ($releasedIds.objects[0].present -or !$releasedIds.objects[1].present -or !$releasedIds.objects[1].mustBeSaved -or [Math]::Abs($releasedIds.objects[1].rawDamage-0.25) -gt 0.001 -or $final.required -ne 0 -or $released.status -eq 'Ready') {throw 'Registered release lost saved state, kept transient objects or lent stale coverage'}
  $result=@{registeredWarming=$true;cold=$cold;coldIds=$coldIds;coldQuery=$coldQuery;coldCollision=$coldCollision;warmed=$warmed;warmIds=$warmIds;warmResidency=$warmResidency;warmQuery=$warmQuery;warmCollision=$warmCollision;queryReleased=$queryReleased;away=$away;awayIds=$awayIds;awayQuery=$awayQuery;awayCollision=$awayCollision;damage=$damage;released=$released;releasedIds=$releasedIds;final=$final;performanceMeasured=$false;shapeCacheLimit=0}
 } elseif ($ColdPositive) {
  $cold=Wait-State {param($r) $r.status -eq 'CapacityExceeded' -and $r.metadataComplete -and $r.positiveColdCreated -eq 2 -and $r.positiveLeasedPlacements -eq 2 -and $r.positiveScannedRegions -ge 1} 'two source-bound cold actor walls'
  $coldIds=Ids;$coldResidency=Send @{cmd='stream_residency'}
  if ($cold.models -ne 257 -or $cold.referencedModels -ne 257 -or $cold.inventoryRows -ne 257 -or $cold.preparedModels -ne 0 -or $cold.readyRegions -ne 0 -or $cold.readyQueries -ne 0 -or $cold.positiveColdRequested -ne 2 -or $cold.positiveColdPrepared -ne 2 -or $cold.positiveColdRefused -ne 0 -or $cold.positiveColdInterrupted -ne 0) {throw 'Cold actor stage expanded unrelated models or query readiness'}
  foreach($wall in $coldIds.objects){if(!$wall.present -or $wall.visualResident -or !$wall.shapeVisualDeferred -or $wall.normalVertexBuffers -ne 0 -or !$wall.hasGeometry -or !$wall.hasFireGeometry){throw 'Cold actor wall absent or visually uploaded'}}
  if($coldResidency.required -ne 0 -or $coldResidency.sourceEnvelopeScans -ne 2 -or $coldResidency.sourceEnvelopePublished -ne 2 -or $cold.payloadCapacityCharge -gt 67108864 -or $cold.shapeSchedulingCharge -gt 268435456 -or $cold.positiveColdWorkingModels -gt 256){throw 'Cold actor stage exceeded source/capacity scope'}
  $coldQueryQueued=Query 'queue';$coldQuery=Query 'status';$coldCollision=Query 'collide'
  if($coldQuery.status -ne 'CapacityExceeded' -or $coldCollision.status -ne 'CapacityExceeded' -or @($coldCollision.contacts).Count -ne 0){throw 'Cold partial stage lent complete Fire readiness'}
  $null=Send @{cmd='exec';code='streamFixtureActor setPos [700,900,0]'}
  $released=Wait-State {param($r) $r.leasedPlacements -eq 0 -and $r.positiveColdWorkingModels -eq 0 -and $r.positiveColdFactsBytes -eq 0 -and $r.payloadCapacityCharge -eq 0 -and $r.shapeSchedulingCharge -eq 0} 'cold actor replacement and working charge retirement'
  $until=[DateTime]::UtcNow.AddSeconds(30)
  do{Start-Sleep -Milliseconds 500;$releasedIds=Ids}while(@($releasedIds.objects | Where-Object {$_.present}).Count -and [DateTime]::UtcNow -lt $until)
  if(@($releasedIds.objects | Where-Object {$_.present}).Count -or $released.status -ne 'CapacityExceeded' -or $released.readyRegions -ne 0){throw 'Cold replacement retained unmodified remote walls or readiness'}
  $finalResidency=Send @{cmd='stream_residency'}
  if($finalResidency.required -ne 0 -or $finalResidency.parseReservedBytes -ne 0 -or $finalResidency.readyPayloadBytes -ne 0 -or $finalResidency.conversionReservedBytes -ne 0){throw 'Cold actor replacement retained preparation debt or pins'}
  $result=@{coldPositive=$true;cold=$cold;coldIds=$coldIds;coldResidency=$coldResidency;coldQueryQueued=$coldQueryQueued;coldQuery=$coldQuery;coldCollision=$coldCollision;released=$released;releasedIds=$releasedIds;finalResidency=$finalResidency;performanceMeasured=$false}
 } elseif ($PositiveWarming) {
  $cold=Wait-State {param($r) $r.status -eq 'CapacityExceeded' -and $r.metadataComplete} 'complete capacity-limited inventory'
  $coldIds=Ids
  if ($cold.models -ne 257 -or $cold.referencedModels -ne 257 -or $cold.inventoryRows -ne 257 -or $cold.preparedModels -ne 0 -or $cold.activeModels -ne 0 -or $cold.leasedPlacements -ne 0 -or $cold.created -ne 0 -or @($coldIds.objects | Where-Object {$_.present}).Count) {throw 'Positive warming loaded a cold partial world'}
  $coldQueryQueued=Query 'queue'
  $coldQuery=Query 'status'
  if ($coldQuery.status -ne 'CapacityExceeded') {throw 'Cold query borrowed positive warming readiness'}
  $pose=Send @{cmd='eval';code='triFreeFlyPose "1188 1200 18 90 -8"'}
  if ($pose.result.Trim('"') -ne 'OK') {throw 'Near camera pose refused'}
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {Start-Sleep -Milliseconds 500;$visual=Ids} while ((@($visual.objects | Where-Object {$_.visualResident -and $_.normalVertexBuffers -gt 0}).Count -ne 2) -and [DateTime]::UtcNow -lt $until)
  if (@($visual.objects | Where-Object {$_.visualResident -and $_.normalVertexBuffers -gt 0}).Count -ne 2) {throw 'Camera did not load both authored walls'}
  $warmed=Wait-State {param($r) $r.positiveLeasedPlacements -eq 2 -and $r.positiveScannedRegions -ge 1} 'positive loaded actor leases'
  $warmedResidency=Send @{cmd='stream_residency'}
  if ($warmed.status -ne 'CapacityExceeded' -or $warmed.readyRegions -ne 0 -or $warmed.readyQueries -ne 0 -or $warmed.preparedModels -ne 0 -or $warmed.activeModels -ne 0 -or $warmed.created -ne 0 -or $warmedResidency.required -ne 0) {throw 'Positive loaded warming changed query readiness or cold admission'}
  $null=Send @{cmd='screenshot';path=(Join-Path $out 'positive-loaded.png')}
  $null=Send @{cmd='eval';code='triFreeFlyPose "100 100 80 45 -25"'}
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {Start-Sleep -Milliseconds 500;$away=Ids} while (@($away.objects | Where-Object {$_.visualResident}).Count -and [DateTime]::UtcNow -lt $until)
  if (@($away.objects | Where-Object {$_.present -and !$_.visualResident -and $_.hasGeometry -and $_.hasFireGeometry}).Count -ne 2) {throw 'Camera eviction lost positive CPU actor leases'}
  $awayState=State;$awayQuery=Query 'status';$awayCollision=Query 'collide';$awayResidency=Send @{cmd='stream_residency'}
  if ($awayState.status -ne 'CapacityExceeded' -or $awayQuery.status -ne 'CapacityExceeded' -or $awayCollision.status -ne 'CapacityExceeded' -or @($awayCollision.contacts).Count -ne 0 -or $awayState.positiveLeasedPlacements -ne 2 -or $awayState.preparedModels -ne 0 -or $awayState.created -ne 0 -or $awayResidency.required -ne 0) {throw 'Away positive leases changed capacity/query/admission state'}
  $null=Send @{cmd='exec';code='streamFixtureActor setPos [700,900,0]'}
  $released=Wait-State {param($r) $r.leasedPlacements -eq 0 -and $r.positiveLeasedPlacements -eq 0 -and $r.positiveScannedRegions -ge 1} 'positive actor replacement release'
  $until=[DateTime]::UtcNow.AddSeconds(30)
  do {Start-Sleep -Milliseconds 500;$releasedIds=Ids} while (@($releasedIds.objects | Where-Object {$_.present}).Count -and [DateTime]::UtcNow -lt $until)
  if (@($releasedIds.objects | Where-Object {$_.present}).Count -or $released.status -ne 'CapacityExceeded' -or $released.readyRegions -ne 0 -or $released.preparedModels -ne 0 -or $released.created -ne 0) {throw 'Positive release retained authored walls or changed global readiness'}
  $result=@{positiveWarming=$true;cold=$cold;coldIds=$coldIds;coldQueryQueued=$coldQueryQueued;coldQuery=$coldQuery;visual=$visual;warmed=$warmed;warmedResidency=$warmedResidency;away=$away;awayState=$awayState;awayQuery=$awayQuery;awayCollision=$awayCollision;awayResidency=$awayResidency;released=$released;releasedIds=$releasedIds}
 } elseif ($Capacity -or $ActiveCapacity) {
  $cold=Wait-State {param($r) $r.status -eq 'CapacityExceeded'} 'pre-admission model capacity refusal'
  $coldIds=Ids
  if ($cold.preparedModels -ne 0 -or $cold.activeModels -ne 0 -or $cold.leasedPlacements -ne 0 -or $cold.sparseLeaseRecords -ne 0 -or $cold.created -ne 0 -or @($coldIds.objects | Where-Object {$_.present}).Count) {throw 'Capacity refusal admitted a partial world'}
  if ($Capacity -and ($cold.models -ne 8193 -or $cold.inventoryRows -ne 0 -or $cold.metadataComplete -or $cold.metadataModelRefusals -ne 1 -or $cold.metadataRecordBytes -ne 0)) {throw 'Metadata capacity refusal performed a partial allocation/scan'}
  if ($ActiveCapacity -and ($cold.models -ne 257 -or $cold.inventoryRows -ne 257 -or !$cold.metadataComplete -or $cold.referencedModels -ne 257 -or $cold.activeModelRefusals -ne 1 -or $cold.metadataRecordBytes -gt 4194304)) {throw 'Active model cap was conflated with compact metadata capacity'}
  $result=@{cold=$cold;coldIds=$coldIds;capacity=[bool]$Capacity;activeCapacity=[bool]$ActiveCapacity}
 } else {
  $cold=Wait-State {param($r) $r.status -eq 'Ready' -and $r.inventoryRows -eq 2 -and $r.preparedModels -eq 1 -and $r.leasedPlacements -eq 2 -and $r.readyRegions -ge 2 -and !$r.pendingRegions -and !$r.unknownRegions -and !$r.refusedRegions} 'complete actor region'
  $coldIds=Ids
  foreach ($wall in $coldIds.objects) {if (!$wall.present -or $wall.visualResident -or !$wall.shapeVisualDeferred -or $wall.normalVertexBuffers -ne 0 -or !$wall.hasGeometry -or !$wall.hasFireGeometry) {throw 'Remote wall was absent or visually uploaded'}}
  $residency=Send @{cmd='stream_residency'}
  if ($residency.required -ne 0) {throw 'Actor admission used permanent required IDs'}
  $query=@{cmd='stream_fire_probe';from=@(1190,11.5,1200);to=@(1210,11.5,1200)}
  $hits=Send $query
  if (!$LargeWall -and !@($hits.contacts | Where-Object {$_.id -eq 1001}).Count) {throw 'Cold actor wall did not obstruct fire'}
  if ($LargeWall -and @($hits.contacts | Where-Object {$_.id -eq 1001}).Count) {throw 'Large origin-offset fixture no longer exposes legacy omission; reassess this test'}
  $queryBefore=Query 'status'
  if ($queryBefore.status -ne 'Pending') {throw 'Neighborhood readiness was reused as a fire query'}
  $collisionBefore=Query 'collide'
  if ($collisionBefore.status -ne 'Pending' -or @($collisionBefore.contacts).Count) {throw 'Unqueued collision published contacts'}
  $invalidPurpose=Query 'queue' -purpose 2
  if ($invalidPurpose.status -ne 'Invalid') {throw 'Unsupported query purpose was admitted'}
  $null=Query 'queue'
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {Start-Sleep -Milliseconds 250;$null=Query 'queue';$queryReady=Query 'status'} while ($queryReady.status -eq 'Pending' -and [DateTime]::UtcNow -lt $until)
  if ($queryReady.status -ne 'Ready') {throw 'Exact fire residency did not complete'}
  $until=[DateTime]::UtcNow.AddSeconds(10)
  do {Start-Sleep -Milliseconds 100;$null=Query 'queue';$collision=Query 'collide'} while ($collision.status -eq 'Pending' -and [DateTime]::UtcNow -lt $until)
  if ($collision.status -ne 'Ready' -or @($collision.contacts | Where-Object {$_.id -eq 1001}).Count -ne 1) {throw 'Complete fire query missed the wall or duplicated its contacts'}
  $wrongRay=Query 'status' -from @(100,11.5,100) -to @(120,11.5,100)
  if ($wrongRay.status -ne 'Pending') {throw 'Unrelated ray inherited fire readiness'}
  $independent=State
  if ($independent.regions -lt 2 -or $independent.readyRegions -lt 2 -or $independent.queryRegions -ne 1 -or $independent.readyQueries -ne 1) {throw 'Query channel replaced automatic neighborhood'}
  if ($independent.queryBorrowedModels -ne 1 -or $independent.queryBorrowedEntries -ne 2) {throw 'Shared model query ownership was missing or duplicated'}
  # Deliberately abandon Fire interest. Polls remain read-only and must neither
  # renew the two-second owner heartbeat nor retire the automatic actor's walls.
  $until=[DateTime]::UtcNow.AddSeconds(15)
  do {
   Start-Sleep -Milliseconds 250
   $heartbeatStatus=Query 'status';$heartbeatExpired=State
  } while (($heartbeatExpired.queryRegions -ne 0 -or $heartbeatStatus.status -ne 'Pending') -and [DateTime]::UtcNow -lt $until)
  if ($heartbeatExpired.queryRegions -ne 0 -or $heartbeatExpired.readyQueries -ne 0 -or $heartbeatStatus.status -ne 'Pending') {throw 'Read-only polling retained abandoned Fire interest'}
  $heartbeatCollision=Query 'collide';$heartbeatIds=Ids
  if ($heartbeatCollision.status -ne 'Pending' -or @($heartbeatCollision.contacts).Count -or
      $heartbeatExpired.leasedPlacements -ne 2 -or $heartbeatExpired.queryBorrowedModels -ne 1 -or $heartbeatExpired.queryBorrowedEntries -ne 2 -or
      @($heartbeatIds.objects | Where-Object {$_.present -and !$_.visualResident -and $_.hasFireGeometry}).Count -ne 2) {throw 'Fire expiry published contacts or lost overlapping automatic ownership'}
  # A new valid queue heartbeat may reacquire the exact query. Continue renewing
  # only while this bounded wait intentionally owns interest in the result.
  $until=[DateTime]::UtcNow.AddSeconds(60)
  do {
   $null=Query 'queue';$heartbeatRequeued=Query 'status';$heartbeatRecollision=Query 'collide'
   if ($heartbeatRequeued.status -eq 'Ready' -and $heartbeatRecollision.status -eq 'Ready') {break}
   Start-Sleep -Milliseconds 250
  } while ([DateTime]::UtcNow -lt $until)
  if ($heartbeatRequeued.status -ne 'Ready' -or $heartbeatRecollision.status -ne 'Ready' -or @($heartbeatRecollision.contacts | Where-Object {$_.id -eq 1001}).Count -ne 1) {throw 'Fire heartbeat requeue failed to restore the authored obstruction'}
  $heartbeatProof=@{expired=$heartbeatExpired;status=$heartbeatStatus;collision=$heartbeatCollision;ids=$heartbeatIds;requeued=$heartbeatRequeued;recollision=$heartbeatRecollision}
  $null=Query 'release'
  $queryReleased=State
  if ($queryReleased.queryRegions -ne 0 -or $queryReleased.leasedPlacements -ne 2) {throw 'Query release retired another interest owner'}
  if ($queryReleased.queryBorrowedModels -ne 1 -or $queryReleased.queryBorrowedEntries -ne 2) {throw 'Query release lost overlapping actor model ownership'}
  if ($LargeWall) {
   if ($coldIds.objects[0].z -ne 1400 -or $coldIds.objects[0].radius -lt 200) {throw 'Large original wall did not retain its distant origin/envelope'}
   $after=State
   if ($after.fireCollisionCandidateTests -lt 1 -or $after.fireCollisionCalls -lt 2) {throw 'Large wall did not exercise the supplementary query path'}
   $result=@{cold=$cold;coldIds=$coldIds;legacyHits=$hits;collisionBefore=$collisionBefore;queryReady=$queryReady;collision=$collision;independent=$independent;queryReleased=$queryReleased;after=$after;largeWall=$true}
  } elseif ($MovedWatcher) {
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
  if ($movedAway.queryBorrowedModels -ne 0 -or $movedAway.queryBorrowedEntries -ne 0) {throw 'Moving actor retained query model ownership'}
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
  if ($released.queryBorrowedModels -ne 0 -or $released.queryBorrowedEntries -ne 0) {throw 'Saved survivor retained joined query model ownership'}
  $result=@{cold=$cold;coldIds=$coldIds;residency=$residency;hits=$hits;queryBefore=$queryBefore;queryReady=$queryReady;invalidPurpose=$invalidPurpose;wrongRay=$wrongRay;independent=$independent;queryReleased=$queryReleased;visual=$visual;away=$away;awayHits=$awayHits;movedAway=$movedAway;movedAwayIds=$movedAwayIds;reacquired=$reacquired;damage=$damage;mutated=$mutated;released=$released;releasedIds=$releasedIds;final=$final}
  }
 }
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(15000) -or $p.ExitCode) {throw 'Game failed to exit cleanly'}
 if (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error'}
 $result.passed=$true;$result.disabled=[bool]$Disabled
 if ($heartbeatProof) {$result.fireHeartbeat=$heartbeatProof}
 $result | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
 Write-Output "Simulation residency evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id -Force}
}
