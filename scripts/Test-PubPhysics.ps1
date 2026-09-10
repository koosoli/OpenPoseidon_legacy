param([switch]$Furniture, [switch]$Inspect, [switch]$Trace,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$label=if ($Furniture) {'furniture'} else {'sphere'}
$out=Join-Path $root ('build/pub-physics/'+$label+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$mission=Join-Path $out 'pub.Eden'
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
New-Item -ItemType Directory -Force $mission,$env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
[IO.File]::WriteAllText((Join-Path $mission 'mission.sqm'), @'
version=11;
class Mission {
 randomSeed=1234;
 class Intel { year=1985; month=6; day=21; hour=12; minute=0; startWeather=0.05; forecastWeather=0.05; startFog=0; forecastFog=0; };
 class Groups { items=1; class Item0 { side="WEST";
 class Vehicles { items=1; class Item0 { position[]={5080,0,3990}; id=0; side="WEST"; vehicle="SoldierWB"; player="PLAYER COMMANDER"; leader=1; skill=1; }; }; }; };
};
class Intro { randomSeed=1; class Intel {}; };
class OutroWin { randomSeed=2; class Intel {}; };
class OutroLoose { randomSeed=3; class Intel {}; };
'@)
$env:POSEIDON_LOOSE_OBJECTS=if ($Furniture) {'1'} else {'0'}
if ($Trace) {$env:POSEIDON_PHYSICS_COMPONENT_TRACE='hospoda'}
$env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE='0.25'
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$log=Join-Path $out 'game.log'
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
$null=$p.Handle
try {
 $client=[Net.Sockets.TcpClient]::new()
 $deadline=[DateTime]::UtcNow.AddSeconds(90)
 while (!$client.Connected) {
  try {$client.Connect('127.0.0.1',$port)} catch {
   if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw}
   Start-Sleep -Milliseconds 250
  }
 }
 $stream=$client.GetStream(); $stream.ReadTimeout=120000
 $reader=[IO.StreamReader]::new($stream)
 $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush=$true
 function Send($command) {
  $writer.WriteLine(($command | ConvertTo-Json -Compress))
  do {
   $line=$reader.ReadLine()
   if ($null -eq $line) {throw 'Harness closed'}
   $r=$line | ConvertFrom-Json
  } while ($null -eq $r.ok)
  Add-Content -LiteralPath (Join-Path $out 'responses.jsonl') -Value $line
  if (!$r.ok) {throw $line}
  return $r
 }
 Start-Sleep -Seconds 15
 Send @{cmd='exec';code='labCamera="camera" camCreate [5092.16,3994.46,2]; labCamera cameraEffect ["internal","back"]; labCamera camSetTarget [5082.16,3994.46,2]; labCamera camCommit 0; showCinemaBorder false'} | Out-Null
 if ($Furniture) {
  Send @{cmd='exec';code='labTable=nearestObject [[5092.16,3994.46,0],"TableHospoda"]'} | Out-Null
  $table=Send @{cmd='eval';code='[typeOf labTable,getPosASL labTable]'}
  Write-Output ($table|ConvertTo-Json -Compress)
  if ($table.result -notmatch 'TableHospoda') {throw 'Expected pub table was not found'}
  Send @{cmd='exec';code='labCamera camSetPos [5093,3992,2]; labCamera camSetTarget labTable; labCamera camCommit 0'} | Out-Null
  Start-Sleep -Seconds 2
  Send @{cmd='screenshot';path=(Join-Path $out 'table-before.png')} | Out-Null
  Send @{cmd='exec';code='labTable setVelocity [-20,0,0]'} | Out-Null
  for ($i=0; $i -lt 20; $i++) {
   Start-Sleep -Milliseconds 100
   $table=Send @{cmd='eval';code='[getPosASL labTable,velocity labTable]'}
   Write-Output ($table|ConvertTo-Json -Compress)
  }
  Send @{cmd='dev_physics_probe';action='state'} | ConvertTo-Json -Compress -Depth 6 | Write-Output
  Send @{cmd='screenshot';path=(Join-Path $out 'table-after.png')} | Out-Null
 } else {
 Send @{cmd='dev_physics_probe';action='init'} | Out-Null
 $from=@(5092.16,18.34,3994.46)
 foreach ($to in @(@(5082.16,18.34,3994.46),@(5102.16,18.34,3994.46),@(5092.16,18.34,3984.46),@(5092.16,18.34,4004.46))) {
  $r=Send @{cmd='dev_physics_probe';action='ray';from=$from;to=$to}
  Write-Output (($to -join ',')+': '+($r|ConvertTo-Json -Compress))
 }
 Send @{cmd='screenshot';path=(Join-Path $out 'pub-before.png')} | Out-Null
 if (!$Inspect) {
  $to=@(5082.16,18.34,3994.46)
  $ray=Send @{cmd='dev_physics_probe';action='ray';from=$from;to=$to}
  if (!$ray.solid) {throw 'No wall collider on acceptance ray'}
  Send @{cmd='dev_physics_probe';action='spawn';from=$from;to=$to} | Out-Null
  for ($i=0; $i -lt 20; $i++) {
   Start-Sleep -Milliseconds 100
   $state=Send @{cmd='dev_physics_probe';action='state'}
   if ($state.positions.Count -ne 1) {throw 'Expected exactly one live probe'}
   $travel=$from[0]-$state.positions[0][0]
   if ($travel -gt ($ray.distance+0.3)) {throw "Probe crossed wall: $travel m, wall $($ray.distance) m"}
  }
  Write-Output ('Final probe: '+($state|ConvertTo-Json -Compress -Depth 6))
  Send @{cmd='screenshot';path=(Join-Path $out 'pub-after.png')} | Out-Null
 }
 }
 Send @{cmd='exit'} | Out-Null
 if (!$p.WaitForExit(20000)) {throw 'Game did not exit'}
 if ($p.ExitCode -ne 0) {throw "Game exit $($p.ExitCode)"}
 if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|Cannot load --test-world' -Quiet) {throw 'Runtime error'}
 Select-String -LiteralPath $log -Pattern 'additional world-list buildings|PHYSCOST:|PHYSCORPUS: instances' | ForEach-Object {$_.Line}
 Write-Output "Evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 if (!$p.HasExited) {Stop-Process -Id $p.Id; $p.WaitForExit()}
}
