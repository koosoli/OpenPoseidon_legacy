<#
.SYNOPSIS
    Prove automatic CPU-only modern geometry residency on the dedicated server.
.DESCRIPTION
    Run -Disabled first, then run without it. Uses an original generated fixture,
    the normal World owner pump, and read-only harness probes. No ResolveObject,
    camera admission, manual region queue, or permanent object pin is used.
    The server must expose the read-only streaming probes in its harness.
    -ColdPositive separately proves opt-in cold positive admission in a world
    whose 257 referenced models prevent complete query readiness.
#>
param(
 [switch]$Disabled,
 [switch]$ColdPositive,
 [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Label='simulation-residency-headless',
 [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
 [string]$Python='C:\Program Files\Python311\python.exe',
 [string]$ServerExe=''
)
$ErrorActionPreference='Stop'
if ($Disabled -and $ColdPositive) {throw 'Disabled and ColdPositive are separate scenarios'}
if (!$env:LOCK_OWNER) {
 $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python)
 if ($Disabled) {$argv+='-Disabled'}
 if ($ColdPositive) {$argv+='-ColdPositive'}
 if ($ServerExe) {$argv+=@('-ServerExe',$ServerExe)}
 $env:LOCK_OWNER='headless automatic simulation residency fixture'
 try {& 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Headless fixture exited $LASTEXITCODE"}} finally {Remove-Item Env:LOCK_OWNER}
 return
}
if (Get-Process OpenPoseidon,PoseidonServer -ErrorAction SilentlyContinue) {throw 'Game or server already running'}
$root=Split-Path $PSScriptRoot -Parent
if (!$ServerExe) {
 $ServerExe=Join-Path $root 'dist/x64-win-rwdi/PoseidonServer.exe'
}
$ServerExe=(Resolve-Path -LiteralPath $ServerExe).Path
$GameDir=(Resolve-Path -LiteralPath $GameDir).Path
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$fixture=Join-Path $out 'fixture'
if ($ColdPositive) {$fixture=Join-Path ([IO.Path]::GetTempPath()) ('op-hcold-'+[guid]::NewGuid().ToString('N').Substring(0,8))}
$mission=Join-Path $out 'simulation-headless.eden'
$log=Join-Path $out 'engine.log'
$environment=@{
 POSEIDON_USER_DIR=(Join-Path $out 'user'); WGR_SIMULATION_RESIDENCY=$(if ($Disabled) {'0'} else {'1'});
 WGR_SIMULATION_POSITIVE_COLD=$(if ($ColdPositive) {'1'} else {'0'}); POSEIDON_MODEL_DDC='0';
 WGR_SIMULATION_RESIDENCY_TEST='0'; WGR_OBJECT_STREAM_WINDOW_GROWTH='0'; WGR_OBJECT_STREAM_RADIUS_CELLS='8';
 WGR_OBJECT_STREAM_GPU_BUDGET='0'; WGR_OBJECT_STREAM_PBO='0'; WGR_OBJECT_STREAM_PBO_TEXTURES='0';
 WGR_NATIVE_DDS_PREPARE='0'; WGR_NATIVE_DDS_BC3_ONLY='0'; WGR_ADAPTIVE_TEXTURE_DETAIL='0';
 POSEIDON_REFORGER_WORLD=$null; POSEIDON_REFORGER_OBJECTS=$null; POSEIDON_REFORGER_STREAM=$null;
 POSEIDON_TEST_RAIN=$null; WGR_WATER_CURLING_BREAKER=$null; WGR_LOD_GOVERNOR_RANGE=$null;
 POSEIDON_LOCKSTEP_HZ=$null; WGR_OBJECT_STREAM_TEST_BUDGET=$null
}
$oldEnvironment=@{};$p=$null;$client=$null
try {
 foreach ($key in $environment.Keys) {
  $oldEnvironment[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
  [Environment]::SetEnvironmentVariable($key,$environment[$key],'Process')
 }
 New-Item -ItemType Directory -Force $mission,$environment.POSEIDON_USER_DIR | Out-Null
 if ($ColdPositive) {
  & $Python (Join-Path $PSScriptRoot 'streaming/build_positive_cold_fixture.py') $fixture
 } else {
  & $Python (Join-Path $PSScriptRoot 'streaming/build_simulation_residency_fixture.py') $fixture --absolute-model-path
 }
 if ($LASTEXITCODE) {throw 'Original fixture generation failed'}
 @'
version=11;
class Mission {
 randomSeed=1234;
 class Intel { year=1985; month=6; day=21; hour=12; minute=0; };
 class Groups { items=2;
  class Item0 { side="WEST"; class Vehicles { items=1; class Item0 {
   position[]={100,0,100}; id=0; side="WEST"; vehicle="SoldierWB";
   player="PLAYER COMMANDER"; text="streamFixtureAnchor"; leader=1; skill=1;
   init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""; this disableAI ""TARGET""";
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
 function Free-Port {
  $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
  try {$listener.Start();return $listener.LocalEndpoint.Port} finally {$listener.Stop()}
 }
 $port=Free-Port;$networkPort=Free-Port
 while ($networkPort -eq $port) {$networkPort=Free-Port}
 $installedMarker=Join-Path $GameDir 'DEPLOYED-FROM.txt'
 $argv=@('-C',('"'+$GameDir+'"'),'--simulate',('"'+$mission+'"'),'--duration','240','--stats','2','--private','--bind-address','127.0.0.1','--port',"$networkPort",'--nosound','--harness',"$port",'--test-world',('"'+(Join-Path $fixture 'simulation-residency.wrp')+'"'),'--log-file',('"'+$log+'"'))
 $provenance=@{
  scriptSha256=(Get-FileHash $PSCommandPath).Hash; head=(& git -C $root rev-parse HEAD);
  serverExe=$ServerExe; serverHash=(Get-FileHash $ServerExe); gameDir=$GameDir;
  installed=$(if (Test-Path -LiteralPath $installedMarker) {Get-Content -LiteralPath $installedMarker} else {$null});
  disabled=[bool]$Disabled; coldPositive=[bool]$ColdPositive; environment=$environment; fixturePath=$fixture; fixture=(Get-Content (Join-Path $fixture 'fixture.json') -Raw | ConvertFrom-Json);
  missionHash=(Get-FileHash (Join-Path $mission 'mission.sqm')); harnessPort=$port; networkPort=$networkPort; arguments=$argv;
  scope='Original static two-wall fixture; does not prove generic query completeness or gameplay consumer migration'
 }
 $provenance | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'provenance.json')
 $p=Start-Process -FilePath $ServerExe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $argv -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError (Join-Path $out 'stderr.log')
 $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(90)
 while (!$client.Connected) {
  try {$client.Connect('127.0.0.1',$port)} catch {
   if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw "Server harness did not start; $out"}
   Start-Sleep -Milliseconds 250
  }
 }
 $stream=$client.GetStream();$stream.ReadTimeout=30000
 $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command) {
  $request=$command | ConvertTo-Json -Compress
  $request | Add-Content (Join-Path $out 'harness.jsonl');$writer.WriteLine($request)
  do {$line=$reader.ReadLine();if ($null -eq $line) {throw 'Server harness closed'};$line | Add-Content (Join-Path $out 'harness.jsonl');$reply=$line | ConvertFrom-Json} while ($null -eq $reply.ok)
  if (!$reply.ok) {throw "Server harness rejected command: $line"}
  return $reply
 }
 function State {Send @{cmd='stream_simulation_residency'}}
 function Ids {Send @{cmd='stream_identity_probe';ids=@(1001,1002)}}
 function Wait-State($predicate,$description,[bool]$AllowCapacity=$false) {
  $until=[DateTime]::UtcNow.AddSeconds(90)
  do {
   Start-Sleep -Milliseconds 500
   if ($p.HasExited) {throw 'Server exited during fixture'}
   $r=State
   if (& $predicate $r) {return $r}
   if ($r.status -in @('Invalid','WrongOwner','Unknown') -or (!$AllowCapacity -and $r.status -eq 'CapacityExceeded')) {throw "Server refused $description : $($r | ConvertTo-Json -Compress)"}
  } while ([DateTime]::UtcNow -lt $until)
  throw "Server did not establish $description : $($r | ConvertTo-Json -Compress)"
 }
 # Wall IDs occur solely in FindObject probes. The cold scenario moves the
 # mission actor after admission; it never resolves or pins a wall identity.
 $until=[DateTime]::UtcNow.AddSeconds(90)
 do {
  Start-Sleep -Milliseconds 500
  if ($p.HasExited) {throw 'Server exited before its mission started'}
  $actor=Send @{cmd='eval';code='(time > 3) && !(isNull streamFixtureActor)'}
 } while ($actor.result -notmatch 'true' -and [DateTime]::UtcNow -lt $until)
 if ($actor.result -notmatch 'true') {throw 'Dedicated mission/authoritative actor did not initialize'}
 $actorPosition=Send @{cmd='eval';code='getPos streamFixtureActor'}
 if ($Disabled) {
  $cold=State;$coldIds=Ids
  if ($cold.status -ne 'Disabled' -or $cold.ownerPumps -ne 0 -or $cold.inventoryRows -ne 0 -or $cold.leasedPlacements -ne 0 -or @($coldIds.objects | Where-Object {$_.present}).Count) {throw 'Disabled dedicated fixture loaded remote walls or performed simulation residency work'}
  if (!$cold.PSObject.Properties['requiredObjects'] -or $cold.requiredObjects -ne 0) {throw 'Disabled dedicated fixture cannot establish zero permanent geometry pins'}
  $result=@{cold=$cold;coldIds=$coldIds;actor=$actor;actorPosition=$actorPosition}
 } elseif ($ColdPositive) {
  $cold=Wait-State {param($r) $r.status -eq 'CapacityExceeded' -and $r.metadataComplete -and $r.inventoryRows -eq 257 -and $r.positiveColdCreated -eq 2 -and $r.positiveLeasedPlacements -eq 2 -and $r.ownerPumps -gt 0} 'source-bound cold dedicated actor walls' $true
  if ($cold.models -ne 257 -or $cold.referencedModels -ne 257 -or $cold.preparedModels -ne 0 -or $cold.readyRegions -ne 0 -or $cold.readyQueries -ne 0 -or $cold.queryRegions -ne 0 -or $cold.positiveColdRequested -ne 2 -or $cold.positiveColdPrepared -ne 2 -or $cold.positiveColdRefused -ne 0 -or $cold.positiveColdInterrupted -ne 0 -or $cold.positiveColdWorkingModels -gt 256 -or $cold.payloadCapacityCharge -gt 67108864 -or $cold.shapeSchedulingCharge -gt 268435456) {throw 'Cold dedicated admission changed complete coverage or bounded source scope'}
  if (!$cold.PSObject.Properties['requiredObjects'] -or $cold.requiredObjects -ne 0) {throw 'Cold dedicated admission cannot establish zero permanent pins'}
  $coldIds=Ids
  if (@($coldIds.objects).Count -ne 2) {throw 'Cold identity probe omitted an authored wall'}
  foreach ($wall in $coldIds.objects) {
   if (!$wall.present -or $wall.visualResident -or !$wall.shapeVisualDeferred -or $wall.normalVertexBuffers -ne 0 -or !$wall.hasGeometry -or !$wall.hasFireGeometry -or !$wall.hasViewGeometry) {throw 'Cold dedicated wall lacks CPU-only three-role geometry'}
  }
  $hits=Send @{cmd='stream_fire_probe';from=@(1190,11.5,1200);to=@(1210,11.5,1200)}
  if (!@($hits.contacts | Where-Object {$_.id -eq 1001}).Count) {throw 'Cold dedicated wall did not obstruct the original fixture segment'}
  $null=Send @{cmd='exec';code='streamFixtureActor setPos [700,900,0]'}
  $released=Wait-State {param($r) $r.leasedPlacements -eq 0 -and $r.positiveColdWorkingModels -eq 0 -and $r.positiveColdFactsBytes -eq 0 -and $r.payloadCapacityCharge -eq 0 -and $r.shapeSchedulingCharge -eq 0} 'cold dedicated actor replacement retirement' $true
  $releasedIds=Ids
  if (@($releasedIds.objects | Where-Object {$_.present}).Count -or $released.status -ne 'CapacityExceeded' -or $released.readyRegions -ne 0 -or $released.readyQueries -ne 0 -or $released.requiredObjects -ne 0) {throw 'Cold dedicated retirement retained walls, pins or query readiness'}
  $result=@{coldPositive=$true;cold=$cold;coldIds=$coldIds;hits=$hits;actor=$actor;actorPosition=$actorPosition;released=$released;releasedIds=$releasedIds;performanceMeasured=$false}
 } else {
  $cold=Wait-State {param($r) $r.status -eq 'Ready' -and $r.inventoryRows -eq 2 -and $r.preparedModels -eq 1 -and $r.leasedPlacements -eq 2 -and $r.ownerPumps -gt 0 -and $r.readyRegions -ge 1 -and !$r.pendingRegions -and !$r.unknownRegions -and !$r.refusedRegions -and !$r.queryRegions} 'automatic dedicated actor residency'
  $coldIds=Ids
  if (@($coldIds.objects).Count -ne 2) {throw 'Identity probe did not return both authored wall IDs'}
  foreach ($wall in $coldIds.objects) {
   if (!$wall.present -or $wall.visualResident -or !$wall.shapeVisualDeferred -or $wall.normalVertexBuffers -ne 0 -or !$wall.hasGeometry -or !$wall.hasFireGeometry -or !$wall.hasViewGeometry) {throw 'Dedicated actor wall lacks CPU query geometry or has visual buffers'}
  }
  if (!$cold.PSObject.Properties['requiredObjects'] -or $cold.requiredObjects -ne 0) {throw 'Automatic dedicated admission cannot establish zero permanent required IDs'}
  $hits=Send @{cmd='stream_fire_probe';from=@(1190,11.5,1200);to=@(1210,11.5,1200)}
  if (!@($hits.contacts | Where-Object {$_.id -eq 1001}).Count) {throw 'Automatically admitted dedicated wall did not obstruct fixture fire segment'}
  $result=@{cold=$cold;coldIds=$coldIds;hits=$hits;actor=$actor;actorPosition=$actorPosition}
 }
 # Renderer name is optional until this read-only field is exposed by the server.
 $renderer=$null
 foreach ($field in @('rendererName','renderer')) {if ($cold.PSObject.Properties[$field]) {$renderer=[string]$cold.$field;break}}
 if ($renderer -and $renderer -notin @('None','No','dummy','Dummy')) {throw "Dedicated server unexpectedly uses renderer '$renderer'"}
 $result.renderer=$renderer;$result.rendererNameExposed=[bool]$renderer
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(15000) -or $p.ExitCode) {throw 'Dedicated server failed to exit cleanly'}
 if (!(Test-Path -LiteralPath $log) -or (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|failed to parse|simulate-smoke.*failed' -Quiet)) {throw 'Dedicated engine log missing or contains a fatal error'}
 $result.passed=$true;$result.disabled=[bool]$Disabled;$result.headless=$true;$result.cleanExit=$true
 $result | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
 Write-Output "Headless simulation residency evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 # Only this runner's process can be stopped; never an existing/user session.
 if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id -Force}
 foreach ($key in $oldEnvironment.Keys) {[Environment]::SetEnvironmentVariable($key,$oldEnvironment[$key],'Process')}
}
