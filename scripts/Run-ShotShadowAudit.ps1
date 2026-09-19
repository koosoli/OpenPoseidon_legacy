param(
 [ValidateSet('Shadows')][string]$Scenario='Shadows',
 [switch]$Reference,
 [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
 $shellExe=Join-Path $PSHOME $(if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'})
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$shellExe.Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Scenario',$Scenario,'-GameDir',$GameDir)
 if ($Reference) {$argv+='-Reference'}
 $env:LOCK_OWNER='isolated shot-effect shadow audit'
 try { & 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Audit exited $LASTEXITCODE"} }
 finally {Remove-Item Env:LOCK_OWNER}
 return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
$out=Join-Path $root ('build/audit-runs/'+$Scenario+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $out | Out-Null
$bin=Join-Path $root 'dist/x64-win-rwdi'
if ($Reference) {
 $bin=Join-Path $root 'build/audit-reference'
 New-Item -ItemType Directory -Force $bin | Out-Null
 foreach ($name in @('OpenPoseidon.exe','wgpu_renderer.dll','OpenAL32.dll')) {
  Copy-Item -LiteralPath (Join-Path $GameDir $name) -Destination (Join-Path $bin $name)
 }
}
$exe=Join-Path $bin 'OpenPoseidon.exe'
if (!(Test-Path $exe)) {throw "Missing build: $exe"}
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllLines((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),@('qualityPreset=3;','msaaSamples=4;','dlssMode=0;','vsync=0;','fpsCap=60;'))
$env:POSEIDON_COMBAT_AUDIT='0'
$env:POSEIDON_TARGET_SCAN_LINEAR='0'
$env:POSEIDON_SIM_VEHICLE_COST=$(if ($Scenario -eq 'Stress') {'1'} else {'0'})
$env:POSEIDON_SMOKE_SYSTEM='0'
$env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE='0.15'
$mission=Join-Path $root 'tests/perf/missions/combat_audit.Intro'
if ($Scenario -eq 'Helicopter') {$mission=Join-Path $root 'tests/perf/missions/perf_heli_distance.Intro'}
if ($Scenario -eq 'Stress') {$mission=Join-Path $GameDir 'Mods/@63units/63MAX.west_ls2'}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$log=Join-Path $out 'engine.log'
$gameArgs=@('-C',('"'+$GameDir+'"'),'--render=wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
if ($Scenario -eq 'Stress') {$gameArgs+=@('--addon-root',('"'+(Join-Path $GameDir 'Mods/@63units')+'"'))}
@{head=(& git -C $root rev-parse HEAD);reference=[bool]$Reference;mission=$mission;gameArgs=$gameArgs;environment=@(Get-ChildItem Env: | Where-Object Name -Match '^(POSEIDON|WGR)' | Select-Object Name,Value);hashes=@(Get-FileHash (Join-Path $bin 'OpenPoseidon.exe'),(Join-Path $bin 'wgpu_renderer.dll'));deployment=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt') -ErrorAction SilentlyContinue)} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'provenance.json')
$p=Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $bin -PassThru -WindowStyle Hidden
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
 $stream=$client.GetStream(); $stream.ReadTimeout=30000
 $reader=[IO.StreamReader]::new($stream)
 $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush=$true
 function Send($command) {
  $writer.WriteLine(($command | ConvertTo-Json -Compress))
  do { $line=$reader.ReadLine(); if ($null -eq $line) {throw 'Harness closed'}; $r=$line | ConvertFrom-Json } while ($null -eq $r.ok)
  ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl')
  $line | Add-Content (Join-Path $out 'harness.jsonl')
  if (!$r.ok) {throw $line}
  return $r
 }
 Start-Sleep -Seconds 15
  $null=Send @{cmd='exec';code='hint ""; auditCamera="camera" camCreate [9696,3548,2]; auditCamera cameraEffect ["internal","back"]; auditCamera camSetTarget [9700,3551,1.2]; auditCamera camCommit 0; showCinemaBorder false; auditPlayer setDir 0'}
  Start-Sleep -Seconds 2
  $null=Send @{cmd='screenshot';path=(Join-Path $out 'empty.png')}
  foreach ($phase in @('rifle','smoke','mixed')) {
   if ($phase -eq 'smoke') {$null=Send @{cmd='exec';code='auditSmoke="SmokeShell" createVehicle [9702,3553,0]'}}
   foreach ($shot in 1..8) {
    if ($phase -ne 'smoke') {$null=Send @{cmd='exec';code='auditPlayer fire "M16"'}}
    Start-Sleep -Milliseconds 350
    if ($shot -eq 4) {$null=Send @{cmd='screenshot';path=(Join-Path $out ($phase+'.png'))}}
   }
   Start-Sleep -Seconds 3
  }
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(30000)) {throw 'Exit timed out'}
 if ($p.ExitCode -ne 0) {throw "Game exit $($p.ExitCode)"}
 if (Select-String -LiteralPath $log -Pattern 'Script error|Validation Error|panicked at|UNHANDLED EXCEPTION') {throw 'Runtime error; inspect engine.log'}
 Write-Output "Audit complete: $out"
} finally {
 if ($client) {$client.Dispose()}
 if (!$p.HasExited) {Stop-Process -Id $p.Id; $p.WaitForExit()}
}
