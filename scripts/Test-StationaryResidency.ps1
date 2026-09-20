param([ValidateSet("Native","DayZ","Legacy")][string]$Corpus="Native",[switch]$Baseline,[switch]$Normal,[switch]$Adaptive,[switch]$Controlled,[switch]$Identity,[switch]$DuringFill,[ValidateRange(-1,16384)][int]$ReadyMb=-1,[string]$Label='stationary-budget',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {
 $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir)
 if ($Baseline) {$argv+='-Baseline'}; if ($Normal) {$argv+='-Normal'}
 foreach ($flag in @('Adaptive','Controlled','Identity','DuringFill')) {if ((Get-Variable $flag -ValueOnly)) {$argv+=('-'+$flag)}}
 $argv+=@('-ReadyMb',[string]$ReadyMb,'-Corpus',$Corpus)
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
$baseX=5092.61; $baseZ=3995.64; $baseY=18.3; $azimuth=200.9; $elevation=-13.5
$worldArgs=@('--test-world','"C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons"')
if ($Corpus -eq 'Native') {
 $env:POSEIDON_REFORGER_WORLD='worlds/eden'
 $env:POSEIDON_REFORGER_OBJECTS='1'
 $env:POSEIDON_REFORGER_STREAM='1'
} else {
 foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM')) {Remove-Item ('Env:'+ $key) -ErrorAction SilentlyContinue}
 if ($Corpus -eq 'DayZ') {
  $baseX=5704.58; $baseZ=2523.37; $baseY=73.74; $azimuth=260.9; $elevation=-3.8
  $world=(Resolve-Path -LiteralPath (Join-Path $root '../../../packages/dayz-compat/world/chernarusplus/chernarusplus.wrp')).Path
  $worldArgs=@('--test-world',('"'+$world+'"'),'--addon-root','"D:\SteamLibrary\steamapps\common\DayZ\Addons"','--test-world-hour','10')
 } else {
  if (!$Normal) {throw 'Legacy lifecycle control requires -Normal'}
  $baseX=6378.7; $baseZ=5672.5; $baseY=74.4; $azimuth=120; $elevation=-12
  $worldArgs=@()
 }
}
function Pose($x,$height=$baseY) { return "$x $baseZ $height $azimuth $elevation" }
$env:WGR_OBJECT_STREAM_TEST_BUDGET='1'
$env:WGR_OBJECT_STREAM_MAX_OBJECTS=if ($Normal) {'20000'} else {'8000'}
$env:WGR_OBJECT_STREAM_GPU_BUDGET='0'
$env:WGR_PASS1_STATS='1'
$env:WGR_RESIDENCY_TRACE='1'
$env:WGR_ADAPTIVE_TEXTURE_DETAIL=if ($Adaptive) {'1'} else {'0'}
if ($ReadyMb -ge 0) {$env:WGR_OBJECT_STREAM_READY_MB=[string]$ReadyMb} else {Remove-Item Env:WGR_OBJECT_STREAM_READY_MB -ErrorAction SilentlyContinue}
Remove-Item Env:POSEIDON_TEST_RAIN -ErrorAction SilentlyContinue
Remove-Item Env:WGR_WATER_CURLING_BREAKER -ErrorAction SilentlyContinue
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
$mission=Join-Path $root 'tests/perf/missions/perf_field.eden';$log=Join-Path $out 'engine.log'
@{head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));corpus=$Corpus;adaptive=[bool]$Adaptive;controlled=[bool]$Controlled;readyMb=$ReadyMb;hashes=@(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe'),(Join-Path $GameDir 'wgpu_renderer.dll'))} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'provenance.json')
$p=$null;$client=$null
try {
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList (@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--capture-metrics',('"'+(Join-Path $out 'capture.json')+'"'),'--test-world-freefly',[string]$baseX,[string]$baseZ,[string]$baseY,[string]$azimuth,[string]$elevation,'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))+$worldArgs)
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
   if ($Corpus -eq 'Legacy') {if ($r.valid) {throw 'Legacy unexpectedly entered modern streamer'}; return $r}
   if ([DateTime]::UtcNow -gt $until) {throw 'Residency failed to settle in 240 seconds'}
  } while (!$r.valid -or $r.pending -or $r.resident -eq 0)
  return $r
 }
 $poseResults=@()
 $during=$null
 if ($DuringFill) {
  if ($Normal) {throw 'DuringFill uses the diagnostic 8000-object budget'}
  $until=[DateTime]::UtcNow.AddSeconds(180)
  do {Start-Sleep -Seconds 1; $during=Send @{cmd='stream_residency'}; if ([DateTime]::UtcNow -gt $until) {throw 'No active fill observed'}} while (!$during.valid -or $during.resident -eq 0)
  if (!$during.pending) {throw 'Fill already finished; cannot verify in-flight budget change'}
  $null=Send @{cmd='stream_residency';budget=4000}
  $shrunk=Wait-Settled
  if ($shrunk.desired -gt 4000) {throw 'Pending fill ignored changed budget'}
  $null=Send @{cmd='stream_residency';budget=8000}
 }
 $before=Wait-Settled
 if ($Controlled -or $Adaptive) {Start-Sleep -Seconds 8}
 $identityBefore=$null; $identityAway=$null; $identityReturned=$null
 if ($Identity) {$identityBefore=Send @{cmd='stream_identity_probe';x=$baseX;z=$baseZ}}
 $null=Send @{cmd='exec';code='logInfo "STREAM_PERF_BEGIN"; triPerfReset 0'}
 Start-Sleep -Seconds 12
 $idle=Send @{cmd='stream_residency'}
 if ($idle.admitUpdates -ne $before.admitUpdates) {throw 'Settled fast path did not remain idle'}
 $null=Send @{cmd='eval';code='triPerfStats 0'}
 $null=Send @{cmd='exec';code='logInfo "STREAM_PERF_END"'}
 if (!$Normal) {
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
 } else {
  $small=$null
  $null=Send @{cmd='exec';code='logInfo "STREAM_TRAVERSE_BEGIN"; triPerfReset 0'}
  $poseResults=@()
  $route=if ($Controlled) {@(($baseX+200),($baseX+400),($baseX+200),$baseX)} else {@(($baseX+100),($baseX+200),($baseX+300),($baseX+400),($baseX+300),($baseX+200),($baseX+100),$baseX)}
  $poseIndex=0
  foreach ($x in $route) {
   $r=Send @{cmd='eval';code=('triFreeFlyPose "'+(Pose $x)+'"')}
   if ($r.result -notmatch 'OK') {throw 'Traversal pose refused'}
   if ($Controlled) {
    $settled=Wait-Settled
    Start-Sleep -Seconds 8
    $null=Send @{cmd='exec';code=('logInfo "STREAM_POSE_'+$poseIndex+'_BEGIN"; triPerfReset 0')}
    Start-Sleep -Seconds 6
    $timing=Send @{cmd='eval';code='triPerfStats 0'}
    $null=Send @{cmd='exec';code=('logInfo "STREAM_POSE_'+$poseIndex+'_END"')}
    $poseResults+=@{x=$x;state=$settled;timing=$timing}
    $poseIndex++
   } else {Start-Sleep -Seconds 3}
  }
  $null=Send @{cmd='eval';code='triPerfStats 0'}
  $null=Send @{cmd='exec';code='logInfo "STREAM_TRAVERSE_END"'}
  $restored=Wait-Settled
  if ($restored.centerX -ne $before.centerX -or $restored.centerZ -ne $before.centerZ -or $restored.desired -ne $before.desired -or $restored.resident -ne $before.resident) {throw 'Traversal return lost coverage'}
 }
 if (!$Baseline -and $Corpus -ne 'Legacy') {
  $sameBudget=$restored.budget
  for ($i=0;$i -lt 5;$i++) {$null=Send @{cmd='stream_residency';budget=$sameBudget}}
  Start-Sleep -Seconds 2
  $unchanged=Send @{cmd='stream_residency'}
  if ($unchanged.admitUpdates -ne $restored.admitUpdates) {throw 'Repeated unchanged budgets caused admission churn'}
 }
 if ($Identity) {
  $null=Send @{cmd='eval';code=('triFreeFlyPose "'+(Pose ($baseX+1500) ($baseY+150))+'"')}
  $null=Wait-Settled
  $identityAway=Send @{cmd='stream_identity_probe';ids=@($identityBefore.objects | ForEach-Object {$_.id})}
  $null=Send @{cmd='eval';code=('triFreeFlyPose "'+(Pose $baseX)+'"')}
  $null=Wait-Settled
  $identityReturned=Send @{cmd='stream_identity_probe';ids=@($identityBefore.objects | ForEach-Object {$_.id})}
 }
 @{during=$during;identityBefore=$identityBefore;identityAway=$identityAway;identityReturned=$identityReturned;poses=$poseResults;before=$before;small=$small;restored=$restored;normal=[bool]$Normal;baselineReproduced=([bool]$Baseline -and !$Normal)} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'result.json')
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'settled.png')}
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(15000) -or $p.ExitCode) {throw 'Game failed to exit cleanly'}
 if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Renderer error'}
 Write-Output "Stationary residency evidence: $out"
} finally {
 if ($client) {$client.Dispose()}
 if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id -Force}
}
