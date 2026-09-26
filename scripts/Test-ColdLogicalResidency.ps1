param([switch]$DestroyCold,[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {
 $env:LOCK_OWNER='cold logical residency check'
 try {
  $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME 'pwsh.exe').Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-GameDir',$GameDir)
  if ($DestroyCold) {$argv+='-DestroyCold'}
  & 'C:\Program Files\Git\bin\bash.exe' @argv
  if ($LASTEXITCODE) {throw "Cold logical test exited $LASTEXITCODE"}
 } finally {Remove-Item Env:LOCK_OWNER}
 return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/dayz-cold-logical-'+(Get-Date -Format yyyyMMdd-HHmmss))
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','WGR_WATER_CURLING_BREAKER')) {Remove-Item ('Env:'+$key) -ErrorAction SilentlyContinue}
$env:WGR_OBJECT_STREAM_TEST_BUDGET='1'
$env:WGR_OBJECT_STREAM_MAX_OBJECTS='1000'
$env:WGR_OBJECT_STREAM_WINDOW_GROWTH='0'
$env:WGR_OBJECT_STREAM_GPU_BUDGET='0'
$env:WGR_OBJECT_STREAM_PBO='0'
$env:WGR_OBJECT_STREAM_PBO_TEXTURES='0'
$env:WGR_ADAPTIVE_TEXTURE_DETAIL='0'
$env:WGR_RESIDENCY_TRACE='1'
$world=(Resolve-Path -LiteralPath (Join-Path $root '../../../packages/dayz-compat/world/chernarusplus/chernarusplus.wrp')).Path
$mission=Join-Path $root 'tests/perf/missions/perf_field.eden'
$log=Join-Path $out 'engine.log'
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
@{scriptSha256=(Get-FileHash $PSCommandPath).Hash;head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));hashes=@(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe'),(Join-Path $GameDir 'wgpu_renderer.dll'));scope='Cold explicit script identity and fire geometry, then visual promotion. Not automatic simulation demand or performance acceptance.'} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'provenance.json')
$p=$null;$client=$null
try {
 # The freefly launcher also grounds the player at the requested x/z. Use
 # inland coordinates; a sea origin ends the mission when that player drowns.
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-world-freefly','10000','10000','450','45','-10','--test-mission',('"'+$mission+'"'),'--test-world',('"'+$world+'"'),'--addon-root','"D:\SteamLibrary\steamapps\common\DayZ\Addons"','--test-world-hour','10','--log-file',('"'+$log+'"'))
 $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(90)
 while (!$client.Connected) {try {$client.Connect('127.0.0.1',$port)} catch {if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw};Start-Sleep -Milliseconds 250}}
 $stream=$client.GetStream();$stream.ReadTimeout=60000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command) {
  $writer.WriteLine(($command | ConvertTo-Json -Compress))
  do {$line=$reader.ReadLine();if ($null -eq $line) {throw 'Harness closed'};$reply=$line | ConvertFrom-Json} while ($null -eq $reply.ok)
  ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl');$line | Add-Content (Join-Path $out 'harness.jsonl')
  if (!$reply.ok) {throw $line};return $reply
 }
 function Wait-Settled {
  $until=[DateTime]::UtcNow.AddSeconds(240)
  do {
   Start-Sleep -Seconds 2
   if ($p.HasExited) {throw 'Game exited before settlement'}
   if ((Get-Content -LiteralPath $log -Tail 150) -match 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION') {throw 'Renderer failure'}
   $r=Send @{cmd='stream_residency'}
   if ([DateTime]::UtcNow -gt $until) {throw 'Residency did not settle'}
  } while (!$r.valid -or $r.pending)
  return $r
 }
 $null=Wait-Settled
 # Visual admission completion precedes queued GPU operations and lazy draw
 # buffers. Require a stable live-geometry baseline before attributing a delta
 # to this cold logical load; never relax the equality check after loading it.
 Start-Sleep -Seconds 8
 $before=Send @{cmd='stream_residency'}
 $stable=0;$stabilityDeadline=[DateTime]::UtcNow.AddSeconds(60)
 do {
  Start-Sleep -Seconds 2
  $sample=Send @{cmd='stream_residency'}
  if (!$sample.pending -and $sample.geometryLiveBytes -eq $before.geometryLiveBytes -and $sample.admitUpdates -eq $before.admitUpdates) {$stable++} else {$stable=0}
  $before=$sample
  if ([DateTime]::UtcNow -gt $stabilityDeadline) {throw 'Initial GPU geometry did not reach a stable baseline; no cold allocation attribution'}
 } while ($stable -lt 3)
 $id=83912800
 $coldBefore=Send @{cmd='stream_identity_probe';ids=@($id)}
 if ($coldBefore.objects[0].present) {throw 'Fixture already present; cold proof invalid'}
 $r=Send @{cmd='exec';code="streamColdRequired = object $id; streamColdRequired setDammage 0.25"}
 $coldAfter=Send @{cmd='stream_identity_probe';ids=@($id)}
 $fixture=$coldAfter.objects[0]
 if (!$fixture.present -or $fixture.visualResident -or !$fixture.shapeVisualDeferred -or !$fixture.hasGeometry -or !$fixture.hasFireGeometry) {throw 'Cold logical shape was not CPU-only with physical payload'}
 $fire=@{cmd='stream_fire_probe';from=@(5705.061035,68.4318695,2512.391357);to=@(5717.061035,68.4318695,2512.391357)}
 $coldFire=Send $fire
 if (!@($coldFire.contacts | Where-Object {$_.id -eq $id}).Count) {throw 'Cold physical shape did not obstruct known ray'}
 Start-Sleep -Seconds 2
 $after=Send @{cmd='stream_residency'}
 if (!$before.gpuMemoryValid -or !$after.gpuMemoryValid -or $before.geometryLiveBytes -ne $after.geometryLiveBytes) {throw 'Cold load changed GPU live geometry, or telemetry unavailable'}
 $destroyedCold=$null
 if ($DestroyCold) {
  # Destruction may legitimately emit other effect meshes. Check this target's
  # normal-level buffers directly, after first proving its intact CPU-only load.
  $null=Send @{cmd='exec';code='streamColdRequired setDammage 1.1'}
  $destructionDeadline=[DateTime]::UtcNow.AddSeconds(20)
  do {
   Start-Sleep -Seconds 1
   $destroyedCold=Send @{cmd='stream_identity_probe';ids=@($id)}
   if ([DateTime]::UtcNow -gt $destructionDeadline) {throw 'Fixture did not reach an actual destruction phase'}
  } while (!$destroyedCold.objects[0].destroyed -or $destroyedCold.objects[0].destroyPhase -lt 0.99)
  $d=$destroyedCold.objects[0]
  if ($d.visualResident -or !$d.shapeVisualDeferred -or $d.normalVertexBuffers -ne 0) {throw 'Cold destruction created target visual buffers'}
 }
 $null=Send @{cmd='stream_residency';budget=20000}
 $r=Send @{cmd='eval';code='triFreeFlyPose "5704.58 2523.37 73.74 260.9 -3.8"'}
 if ($r.result.Trim('"') -ne 'OK') {throw 'Visual return pose refused'}
 $promotedState=Wait-Settled
 $promoted=Send @{cmd='stream_identity_probe';ids=@($id)}
 if (!$promoted.objects[0].present -or !$promoted.objects[0].visualResident -or $promoted.objects[0].shapeVisualDeferred) {throw 'Logical shape failed visual promotion'}
 $r=Send @{cmd='eval';code="streamColdRequired == object $id"}
 if ($r.result -notmatch 'true') {throw 'Promotion changed object identity'}
 # The script getter clamps raw damage to 1, including destruction at 1.1.
 $damageCheck=if ($DestroyCold) {'(getDammage streamColdRequired > 0.99) && (getDammage streamColdRequired < 1.01)'} else {'(getDammage streamColdRequired > 0.24) && (getDammage streamColdRequired < 0.26)'}
 $r=Send @{cmd='eval';code=$damageCheck}
 if ($r.result -notmatch 'true') {throw 'Promotion changed object damage'}
 if ($DestroyCold -and (!$promoted.objects[0].destroyed -or $promoted.objects[0].destroyPhase -lt 0.99 -or $promoted.objects[0].normalVertexBuffers -eq 0)) {throw 'Destroyed object failed deferred dynamic buffer completion'}
 $promotedFire=Send $fire
 if (!$DestroyCold -and !@($promotedFire.contacts | Where-Object {$_.id -eq $id}).Count) {throw 'Promoted fixture lost physical obstruction'}
 Start-Sleep -Seconds 8
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'promoted.png')}
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(15000) -or $p.ExitCode) {throw 'Game failed clean exit'}
 if (Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error'}
 @{passed=$true;destroyCold=[bool]$DestroyCold;destroyedCold=$destroyedCold;before=$before;coldBefore=$coldBefore;coldAfter=$coldAfter;coldFire=$coldFire;after=$after;promotedState=$promotedState;promoted=$promoted;promotedFire=$promotedFire} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
 Write-Output "Cold logical residency evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id -Force}
}
