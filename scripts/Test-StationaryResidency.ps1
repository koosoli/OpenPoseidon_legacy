param([switch]$Baseline,[string]$Label='stationary-budget',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {
 $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir)
 if ($Baseline) {$argv+='-Baseline'}
 $env:LOCK_OWNER='stationary residency check'
 try {& 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Residency test exited $LASTEXITCODE"}} finally {Remove-Item Env:LOCK_OWNER}
 return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$env:POSEIDON_REFORGER_WORLD='worlds/eden'
$env:POSEIDON_REFORGER_OBJECTS='1'
$env:POSEIDON_REFORGER_STREAM='1'
$env:WGR_OBJECT_STREAM_TEST_BUDGET='1'
$env:WGR_OBJECT_STREAM_MAX_OBJECTS='8000'
$env:WGR_OBJECT_STREAM_GPU_BUDGET='0'
$env:WGR_PASS1_STATS='1'
Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue
Remove-Item Env:WGR_WATER_CURLING_BREAKER -ErrorAction SilentlyContinue
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
$mission=Join-Path $root 'tests/perf/missions/perf_field.eden';$log=Join-Path $out 'engine.log'
@{head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));hashes=@(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe'),(Join-Path $GameDir 'wgpu_renderer.dll'))} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'provenance.json')
$p=$null;$client=$null
try {
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-world','"C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons"','--test-world-freefly','5092.61','3995.64','18.3','200.9','-13.5','--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(90)
 while (!$client.Connected) {try {$client.Connect('127.0.0.1',$port)} catch {if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw};Start-Sleep -Milliseconds 250}}
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
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
   $r=Send @{cmd='stream_residency'}
   if ([DateTime]::UtcNow -gt $until) {throw 'Residency failed to settle in 240 seconds'}
  } while (!$r.valid -or $r.pending -or $r.resident -eq 0)
  return $r
 }
 $before=Wait-Settled
 $null=Send @{cmd='exec';code='logInfo "STREAM_PERF_BEGIN"; triPerfReset 0'}
 Start-Sleep -Seconds 12
 $idle=Send @{cmd='stream_residency'}
 if ($idle.admitUpdates -ne $before.admitUpdates) {throw 'Settled fast path did not remain idle'}
 $null=Send @{cmd='eval';code='triPerfStats 0'}
 $null=Send @{cmd='exec';code='logInfo "STREAM_PERF_END"'}
 $null=Send @{cmd='stream_residency';budget=4000}
 Start-Sleep -Seconds 3
 $small=Wait-Settled
 if ($small.centerX -ne $before.centerX -or $small.centerZ -ne $before.centerZ) {throw 'Camera moved during budget test'}
 if ($Baseline) {
  if ($small.desired -ne $before.desired -or $small.resident -ne $before.resident) {throw 'Baseline no longer reproduces the stationary invalidation defect'}
 } else {
  if ($small.desired -gt 4000 -or $small.resident -ge $before.resident) {throw 'Stationary shrink was ignored'}
 }
 $null=Send @{cmd='stream_residency';budget=8000}
 Start-Sleep -Seconds 2
 $restored=Wait-Settled
 if ($restored.desired -ne $before.desired -or $restored.resident -ne $before.resident) {throw 'Budget restoration lost coverage'}
 @{before=$before;small=$small;restored=$restored;baselineReproduced=[bool]$Baseline} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'result.json')
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'settled.png')}
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(15000) -or $p.ExitCode) {throw 'Game failed to exit cleanly'}
 if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error'}
 Write-Output "Stationary residency evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id -Force}
}
