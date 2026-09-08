param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault', [switch]$Menu)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) { throw 'Use scripts/with-game-lock.sh' }
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/showcase/smoke'
New-Item -ItemType Directory -Force $out | Out-Null
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start()
$port=$listener.LocalEndpoint.Port
$listener.Stop()
$log=Join-Path $out 'showcase.log'
$mission=Join-Path $GameDir 'Missions/ShowcaseLab.Intro'
$argsGame=@('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'),'--log-level','debug')
if ($Menu) {
 $argsGame=@('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--log-file',('"'+$log+'"'),'--log-level','debug')
}
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -ArgumentList $argsGame -WorkingDirectory $GameDir -PassThru -WindowStyle Hidden
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
 $stream=$client.GetStream()
 $stream.ReadTimeout=30000
 $reader=[IO.StreamReader]::new($stream)
 $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false))
 $writer.AutoFlush=$true
 function Send($command) {
  $writer.WriteLine(($command | ConvertTo-Json -Compress))
  do {
   $line=$reader.ReadLine()
   if ($null -eq $line) {throw 'Harness closed'}
   $r=$line | ConvertFrom-Json
  } while ($null -eq $r.ok)
  if (!$r.ok) {throw $line}
  Write-Output $line
 }
 Start-Sleep -Seconds 18
 if ($Menu) {
  Send @{cmd='exec';code='triSetLanguage "English"; triClickText "SINGLE MISSION"'}
  Start-Sleep -Seconds 3
  Send @{cmd='eval';code='triAssertListText [101,"Showcase Lab - Physics and Light"]'}
  Send @{cmd='eval';code='triSelectListByData [101,"ShowcaseLab.Intro"]'}
  Send @{cmd='screenshot';path=(Join-Path $out 'mission-menu.png')}
  Send @{cmd='exec';code='triClickText "Play"'}
  Start-Sleep -Seconds 18
 }
 Send @{cmd='eval';code='typeOf labNormal'}
 Send @{cmd='exec';code='hint ""; showCinemaBorder false; labCamera = "camera" camCreate [9483,3488,9]; labCamera cameraEffect ["internal","back"]; labCamera camSetTarget [9503,3507,0]; labCamera camCommit 0'}
 Start-Sleep -Seconds 4
 Send @{cmd='screenshot';path=(Join-Path $out 'campus.png')}
 Send @{cmd='exec';code='[player,player,labReset] exec "action.sqs"'}
 Start-Sleep -Seconds 2
 Send @{cmd='exec';code='labCamera camSetPos [9515,3499,2]; labCamera camSetTarget [9515,3510,1.5]; labCamera camCommit 0; [player,player,labLight] exec "action.sqs"'}
 Start-Sleep -Seconds 2
 Send @{cmd='exec';code='[player,player,labDrop] exec "action.sqs"'}
 Start-Sleep -Seconds 3
 Send @{cmd='screenshot';path=(Join-Path $out 'materials-day.png')}
 Send @{cmd='exec';code='[player,player,labDay] exec "action.sqs"'}
 Start-Sleep -Seconds 4
 Send @{cmd='screenshot';path=(Join-Path $out 'materials-night.png')}
 Send @{cmd='eval';code='triPhysicsShowcase "invalid"'}
 Send @{cmd='exec';code='[player,player,labClear] exec "action.sqs"'}
 Send @{cmd='exit'}
 if (!$p.WaitForExit(30000)) {throw 'Exit timed out'}
 if ($p.ExitCode -ne 0) {throw "Game exit $($p.ExitCode)"}
} finally {
 if ($client) {$client.Dispose()}
 if (!$p.HasExited) {$p.Kill()}
}
if (!(Select-String -LiteralPath $log -SimpleMatch 'SHOWCASE: ready')) {throw 'Mission init did not finish'}
Write-Output "Showcase captures: $out"
