param([switch]$Baseline,[switch]$Dry,[string]$Label='rain-terrain',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {
 $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir)
 if ($Baseline) {$argv+='-Baseline'}; if ($Dry) {$argv+='-Dry'}
 $env:LOCK_OWNER='rain terrain visibility check'
 try {& 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Rain test exited $LASTEXITCODE"}} finally {Remove-Item Env:LOCK_OWNER}
 return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/rain-terrain/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
if ($Dry) {Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue} else {$env:POSEIDON_TEST_RAIN='particle'}
Remove-Item Env:WGR_WATER_CURLING_BREAKER -ErrorAction SilentlyContinue
$env:WGR_OCEAN_PROFILE='1'
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
$mission=Join-Path $root 'tests/perf/missions/ocean_shore.noe';$log=Join-Path $out 'engine.log'
@{head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));dry=[bool]$Dry;hashes=@(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe'),(Join-Path $GameDir 'wgpu_renderer.dll'))} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'provenance.json')
$p=$null;$client=$null
try {
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(90)
 while (!$client.Connected) {try {$client.Connect('127.0.0.1',$port)} catch {if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw};Start-Sleep -Milliseconds 250}}
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command) {
  $writer.WriteLine(($command | ConvertTo-Json -Compress))
  do {$line=$reader.ReadLine();if ($null -eq $line) {throw 'Harness closed'};$reply=$line | ConvertFrom-Json} while ($null -eq $reply.ok)
  ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl');$line | Add-Content (Join-Path $out 'harness.jsonl')
  if (!$reply.ok) {throw $line};return $reply
 }
 Start-Sleep -Seconds 12
 $null=Send @{cmd='exec';code='0 setFog 0; player allowDamage false'}
 $null=Send @{cmd='eval';code='triFreeFlyPose "7900 4550 150 270 -20"'}
 Start-Sleep -Seconds 3
 $null=Send @{cmd='exec';code='logInfo "RAIN_PERF_BEGIN"; triPerfReset 0'}
 foreach ($pose in @('7900 4550 150 270 -20','7800 4750 150 270 -20','7700 4950 150 270 -20')) {
  $r=Send @{cmd='eval';code=('triFreeFlyPose "'+$pose+'"')};if ($r.result -notmatch 'OK') {throw 'Free flight pose refused'}
  Start-Sleep -Seconds 4
  if (!$Baseline) {
   $state=Send @{cmd='weather_visibility'}
   if ($state.fog -gt 0.001 -or $state.baseRange -lt $state.selectedRange-21 -or $state.terrainRange -lt $state.objectRange-1) {throw 'Render ranges diverged in clear-fog weather'}
   if (!$Dry -and ($state.rain -lt 0.95 -or $state.tacticalVisibility -gt 400)) {throw 'Rain/tactical regression'}
  }
 }
 $null=Send @{cmd='eval';code='triPerfStats 0'};$null=Send @{cmd='exec';code='logInfo "RAIN_PERF_END"'}
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'moved.png')}
 $null=Send @{cmd='eval';code='triFreeFlyPose "7900 4550 150 270 -20"'};Start-Sleep -Seconds 1
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'initial.png')}
 if (!$Baseline) {
  $null=Send @{cmd='exec';code='0 setFog 0.5'};Start-Sleep -Seconds 3
  $fogged=Send @{cmd='weather_visibility'}
  if ($fogged.fog -lt 0.45 -or $fogged.baseRange -ge $state.baseRange) {throw 'Explicit fog range no longer responds'}
 }
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(15000) -or $p.ExitCode) {throw 'Game failed to exit cleanly'}
 if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error'}
 Write-Output "Rain terrain evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id -Force}
}
