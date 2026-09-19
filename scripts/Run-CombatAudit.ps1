param(
 [ValidateSet('Downloads','Helicopter','Stress','StressDistance','StressBehavior','LaneMove','Reposition','Cover','CoverProbe','Combat','Grenades','Burst','BurstAim','Crossfire','Shadows')][string]$Scenario='Helicopter',
 [int]$Seconds=90, [switch]$Reference, [switch]$Diagnostics, [switch]$LegacyTargetScan, [switch]$LegacyCombat, [switch]$VehicleCosts, [switch]$FixedCamera, [switch]$CenterPlayer,
 [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
 $shellExe=Join-Path $PSHOME $(if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'})
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$shellExe.Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Scenario',$Scenario,'-Seconds',"$Seconds",'-GameDir',$GameDir)
 if ($Reference) {$argv+='-Reference'}
 if ($Diagnostics) {$argv+='-Diagnostics'}
 if ($LegacyTargetScan) {$argv+='-LegacyTargetScan'}
 if ($LegacyCombat) {$argv+='-LegacyCombat'}
 if ($VehicleCosts) {$argv+='-VehicleCosts'}
 if ($FixedCamera) {$argv+='-FixedCamera'}
 if ($CenterPlayer) {$argv+='-CenterPlayer'}
 $env:LOCK_OWNER='isolated combat and performance audit'
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
$env:POSEIDON_COMBAT_AUDIT=$(if ($Diagnostics) {'1'} else {'0'})
$env:POSEIDON_AI_COMBAT_LEGACY=$(if ($LegacyCombat) {'1'} else {'0'})
$env:POSEIDON_TARGET_SCAN_LINEAR=$(if ($LegacyTargetScan) {'1'} else {'0'})
$env:POSEIDON_SIM_VEHICLE_COST=$(if ($VehicleCosts) {'1'} else {'0'})
$env:POSEIDON_SMOKE_SYSTEM='0'
$env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE='0.15'
$mission=Join-Path $root 'tests/perf/missions/combat_audit.Intro'
if ($Scenario -in @('LaneMove','Reposition','Cover','CoverProbe')) {$mission=Join-Path $root 'tests/perf/missions/combat_lane.Intro'}
if ($Scenario -eq 'Helicopter') {$mission=Join-Path $root 'tests/perf/missions/perf_heli_distance.Intro'}
if ($Scenario -in @('Stress','StressDistance','StressBehavior')) {$mission=Join-Path $GameDir 'Mods/@63units/63MAX.west_ls2'}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$log=Join-Path $out 'engine.log'
$gameArgs=@('-C',('"'+$GameDir+'"'),'--render=wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
if ($Scenario -in @('Stress','StressDistance','StressBehavior')) {$gameArgs+=@('--addon-root',('"'+(Join-Path $GameDir 'Mods/@63units')+'"'))}
@{head=(& git -C $root rev-parse HEAD);reference=[bool]$Reference;diagnostics=[bool]$Diagnostics;linearScan=[bool]$LegacyTargetScan;mission=$mission;gameArgs=$gameArgs;environment=@(Get-ChildItem Env: | Where-Object Name -Match '^(POSEIDON|WGR)' | Select-Object Name,Value);dataHashes=@(Get-FileHash (Join-Path $GameDir 'bin/CONFIG.BIN'),(Join-Path $GameDir 'bin/remaster.cpp'),(Join-Path $mission 'mission.sqm'));hashes=@(Get-FileHash (Join-Path $bin 'OpenPoseidon.exe'),(Join-Path $bin 'wgpu_renderer.dll'));deployment=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt') -ErrorAction SilentlyContinue)} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'provenance.json')
$http=$null
$p=$null
try {
if ($Scenario -eq 'Downloads') {
 $portFile=Join-Path $out 'http.port'
 $httpLog=Join-Path $out 'http.jsonl'
 $fixture=Join-Path $root 'tests/perf/http_download_fixture.py'
 $http=Start-Process -FilePath (Get-Command python).Source -ArgumentList @(('"'+$fixture+'"'),'--port-file',('"'+$portFile+'"'),'--log',('"'+$httpLog+'"')) -PassThru -WindowStyle Hidden
 $ready=[DateTime]::UtcNow.AddSeconds(10)
 while (!(Test-Path $portFile)) { if($http.HasExited -or [DateTime]::UtcNow -gt $ready){throw 'HTTP fixture failed'}; Start-Sleep -Milliseconds 100 }
}
$p=Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $bin -PassThru -WindowStyle Hidden
$null=$p.Handle
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
 if ($Scenario -eq 'Downloads') {
  $httpPort=(Get-Content $portFile).Trim()
  foreach ($route in @('resume','no-range','changed','repeated')) {
   $r=Send @{cmd='eval';code=('triDownloadFile "http://127.0.0.1:'+$httpPort+'/'+$route+'"')}
   if ($r.result -cne ('"fixture-download-resumed-after-disconnect'+[char]10+'"')) {throw "Download failed: $route"}
  }
  $requests=@(Get-Content $httpLog | ConvertFrom-Json)
  foreach ($route in @('resume','no-range','changed','repeated')) {
   $rows=@($requests | Where-Object route -eq $route)
   if ($rows.Count -lt 2 -or !($rows | Where-Object range -gt 0)) {throw "Resume not exercised: $route"}
  }
 } elseif ($Scenario -eq 'Helicopter') {
  $null=Send @{cmd='exec';code='hint ""; auditCamera="camera" camCreate [9700,3600,2]; auditCamera cameraEffect ["internal","back"]; auditCamera camSetTarget distanceHeli; auditCamera camSetFov 0.2; auditCamera camCommit 0; showCinemaBorder false'}
  if ($FixedCamera) {$null=Send @{cmd='exec';code='auditCamera camSetTarget [9700,4150,80]; auditCamera camSetFov 0.35; auditCamera camCommit 0'}}
  $null=Send @{cmd='screenshot';path=(Join-Path $out 'heli-start.png')}
  $null=Send @{cmd='eval';code='triPerfCapture 8192'}
  for ($i=0; $i -lt $Seconds; $i+=2) {
   $null=Send @{cmd='eval';code='[time,getPos distanceHeli,velocity distanceHeli,alive distancePilot]'}
   Start-Sleep -Seconds 2
  }
  $null=Send @{cmd='eval';code='triPerfCapture 0'}
 } elseif ($Scenario -eq 'StressDistance') {
  # Startup wall time varies with shader/resource warmup. Match mission time,
  # otherwise one arm has thousands fewer footprints when the camera moves.
  $readyDeadline=[DateTime]::UtcNow.AddSeconds(60)
  do {
   $ready=Send @{cmd='eval';code='time'}
   if ([double]$ready.result -ge 10.25) {break}
   if ([DateTime]::UtcNow -gt $readyDeadline) {throw 'Mission did not reach measurement time'}
   Start-Sleep -Milliseconds 200
  } while ($true)
  $null=Send @{cmd='exec';code='player allowDamage false; hint ""; auditCamera="camera" camCreate [10860,6380,40]; auditCamera cameraEffect ["internal","back"]; auditCamera camSetTarget [10860,6940,1]; auditCamera camSetFov 0.14; auditCamera camCommit 0; showCinemaBorder false'}
  $null=Send @{cmd='eval';code='triPerfCapture 8192'}
  foreach ($phase in @('far','near','far')) {
   $cameraCode=if ($phase -eq 'near') {'auditCamera camSetPos [10860,6820,8]; auditCamera camSetFov 0.7'} else {'auditCamera camSetPos [10860,6380,40]; auditCamera camSetFov 0.14'}
   $null=Send @{cmd='exec';code=($cameraCode+'; auditCamera camSetTarget [10860,6940,1]; auditCamera camCommit 0')}
   $null=Send @{cmd='eval';code=('["'+$phase+'",time]')}
   $null=Send @{cmd='ai_stress_state'}
   Start-Sleep -Seconds 8
  }
  $null=Send @{cmd='eval';code='triPerfCapture 0'}
  $null=Send @{cmd='ai_stress_state'}
  $null=Send @{cmd='screenshot';path=(Join-Path $out 'distance.png')}
 } elseif ($Scenario -in @('Stress','StressBehavior')) {
  if ($Scenario -eq 'StressBehavior') {
   $CenterPlayer=$true
   $null=Send @{cmd='ai_stress_state';recordShots=1}
  }
  if ($CenterPlayer) {$null=Send @{cmd='exec';code='player allowDamage false; player setPos [10860,6940,0]'}}
  for ($i=0;$i -lt $Seconds;$i+=5) {
   $null=Send @{cmd='eval';code='time'}
   if (!$Reference) {$null=Send @{cmd='ai_stress_state'}}
   Start-Sleep -Seconds 5
  }
  $null=Send @{cmd='screenshot';path=(Join-Path $out 'stress.png')}
 } elseif ($Scenario -in @('Combat','Grenades','Burst','BurstAim','Crossfire','LaneMove','Reposition','Cover','CoverProbe')) {
  $null=Send @{cmd='exec';code='auditTwist=false; auditWrongShots=0; auditCrossing=false; auditShots=0; auditShooter addEventHandler ["Fired",{auditShots=auditShots+1; if (auditTwist && auditShots>1 && getDir auditShooter>30 && getDir auditShooter<330) then {auditWrongShots=auditWrongShots+1}; if (auditTwist && auditShots==1) then {auditShooter setDir 90}; if (auditCrossing && auditShots==1) then {auditFriend setPos [9700,3610,0]}}]; auditTarget allowDamage false; auditFriend allowDamage false; hint ""; auditCamera="camera" camCreate [9695,3595,3]; auditCamera cameraEffect ["internal","back"]; auditCamera camSetTarget auditTarget; auditCamera camCommit 0; showCinemaBorder false'}
  if (!$Reference) {
   $null=Send @{cmd='ai_combat';improved=0}
   $r=Send @{cmd='ai_combat';improved=1}
   if (!$r.improved -or !$r.active) {throw 'AI combat setting did not activate'}
   $null=Send @{cmd='ai_combat';improved=$(if ($LegacyCombat) {0} else {1})}
  }
  if ($Scenario -eq 'BurstAim') {
   $modes=if ($Reference) {@(1)} else {@(0,1)}
   foreach ($mode in $modes) {
    $null=Send @{cmd='ai_combat';improved=$mode}
    $null=Send @{cmd='exec';code='auditTwist=false; auditShooter setCombatMode "BLUE"; auditShooter setDir 0; auditFriend setPos [9720,3620,0]; auditShooter setVehicleAmmo 1; auditShooter reveal auditTarget; auditShooter doTarget auditTarget'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='exec';code='auditShots=0; auditWrongShots=0; auditTwist=true; auditShooter setCombatMode "RED"'}
    Start-Sleep -Seconds 6
    $null=Send @{cmd='eval';code='[time,auditShots,auditWrongShots,getDir auditShooter]'}
    $null=Send @{cmd='ai_combat'}
    $null=Send @{cmd='exec';code='auditTwist=false; auditShooter setCombatMode "BLUE"'}
   }
  }
  if ($Scenario -eq 'CoverProbe') {
   $null=Send @{cmd='exec';code='auditFriend setPos [9780,3620,0]; auditTarget setPos [9700,3700,0]; auditShooter reveal auditTarget; auditShooter doTarget auditTarget; auditCamera camSetPos [9697,3694,2]; auditCamera camSetTarget auditTarget; auditCamera camCommit 0'}
   foreach ($type in @('FenceWood')) {
    $null=Send @{cmd='exec';code=('auditWall="'+$type+'" createVehicle [9700,3698,0]')}
    foreach ($rot in @(0,90)) {
     $null=Send @{cmd='exec';code=('auditWall setDir '+$rot)}
     foreach ($height in @(0.65,0.7,0.75,0.8,0.85,0.9,0.95)) {
      $null=Send @{cmd='exec';code=('auditWall setPos [9700,3698,'+$height+']')}
      Start-Sleep -Milliseconds 150
      $null=Send @{cmd='ai_cover_probe'}
     }
    }
    $null=Send @{cmd='screenshot';path=(Join-Path $out ($type+'.png'))}
    $null=Send @{cmd='exec';code='deleteVehicle auditWall'}
   }
  }
  if ($Scenario -eq 'Cover') {
   $null=Send @{cmd='exec';code='auditFriend setPos [9780,3620,0]; auditTarget setPos [9700,3700,0]; auditShooter reveal auditTarget; auditShooter doTarget auditTarget; auditWall="FenceWood" createVehicle [9750,3698,0]; auditWall setDir 0'}
   foreach ($pos in @('[9750,3698,0]','[9700,3698,0.7]','[9700,3698,1]','[9700,3698,0.7]','[9750,3698,0]')) {
    $null=Send @{cmd='exec';code=('auditShooter setCombatMode "BLUE"; auditWall setPos '+$pos+'; auditShooter setVehicleAmmo 1; auditShots=0')}
    Start-Sleep -Seconds 1
    $null=Send @{cmd='ai_cover_probe'}
    $null=Send @{cmd='screenshot';path=(Join-Path $out ('cover-'+[guid]::NewGuid().ToString('N')+'.png'))}
    $null=Send @{cmd='exec';code='auditShooter setCombatMode "RED"'}
    Start-Sleep -Seconds 6
    $null=Send @{cmd='eval';code='[time,auditShots,auditShooter knowsAbout auditTarget]'}
    $null=Send @{cmd='ai_combat'}
    $null=Send @{cmd='ai_cover_probe'}
   }
  }
  if ($Scenario -eq 'Reposition') {
   $null=Send @{cmd='exec';code='auditShooter setCombatMode "BLUE"; auditFriend setPos [9700,3750,0]; auditTarget setPos [9700,3700,0]; auditShooter reveal auditTarget; auditShooter doTarget auditTarget; auditShooter setVehicleAmmo 1'}
   Start-Sleep -Seconds 2
   $null=Send @{cmd='exec';code='auditShots=0; auditShooter setCombatMode "RED"'}
   for ($i=0;$i -lt $Seconds;$i+=2) {
    Start-Sleep -Seconds 2
    $null=Send @{cmd='ai_combat'}
    $null=Send @{cmd='ai_stress_state'}
    $null=Send @{cmd='eval';code='[time,getPos auditShooter,auditShots]'}
   }
  }
  if ($Scenario -eq 'LaneMove') {
   foreach ($mode in @(0,1)) {
    $null=Send @{cmd='ai_combat';improved=$mode}
    $null=Send @{cmd='exec';code='auditShooter setCombatMode "BLUE"; auditShooter setPos [9700,3600,0]; auditShooter setDir 0; auditFriend setPos [9700,3620,0]; auditShooter setVehicleAmmo 1; auditShooter reveal auditTarget; auditShooter doTarget auditTarget'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='exec';code='auditShots=0; auditShooter setCombatMode "RED"; auditShooter doMove [9700,3640,0]'}
    for ($i=0;$i -lt 12;$i++) {
     Start-Sleep -Seconds 1
     $null=Send @{cmd='ai_combat'}
     $null=Send @{cmd='ai_stress_state'}
     $null=Send @{cmd='eval';code='[time,getPos auditShooter,auditShots]'}
    }
    $null=Send @{cmd='exec';code='auditShooter setCombatMode "BLUE"; doStop auditShooter'}
   }
  }
  if ($Scenario -eq 'Crossfire') {
   # Keep the hostile target at 100 m; allies on the far side must still inhibit fire.
   foreach ($position in @('[9700,3620,0]','[9700,3750,0]','[9700,3900,0]','[9720,3900,0]')) {
    $null=Send @{cmd='exec';code=('auditShooter setCombatMode "BLUE"; auditFriend setPos '+$position+'; auditTarget setPos [9700,3700,0]; auditShooter setVehicleAmmo 1; auditShooter reveal auditTarget; auditShooter doTarget auditTarget')}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='exec';code='auditShots=0; auditShooter setCombatMode "RED"'}
    Start-Sleep -Seconds 8
    $null=Send @{cmd='eval';code='[time,auditShots,getPos auditFriend,getPos auditTarget]'}
    $null=Send @{cmd='exec';code='auditShooter setCombatMode "BLUE"'}
   }
  }
  if ($Scenario -eq 'Burst') {
   if ($Reference) {throw 'Burst A/B requires the new build with the AI combat setting'}
   foreach ($mode in @(0,1)) {
    $null=Send @{cmd='ai_combat';improved=$mode}
    $null=Send @{cmd='exec';code='auditCrossing=false; auditShooter setCombatMode "BLUE"; auditFriend setPos [9720,3620,0]; auditTarget setPos [9700,3700,0]; auditShooter setVehicleAmmo 1; auditShooter reveal auditTarget; auditShooter doTarget auditTarget'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='exec';code='auditShots=0; auditCrossing=true; auditShooter setCombatMode "RED"'}
    Start-Sleep -Seconds 6
    $null=Send @{cmd='eval';code='[time,auditShots,getPos auditFriend,magazines auditShooter]'}
    $null=Send @{cmd='exec';code='auditCrossing=false; auditShooter setCombatMode "BLUE"'}
   }
  }
  $ranges=if ($Scenario -eq 'Combat') {@(100,250,400)} else {@()}
  foreach ($range in $ranges) {
   $null=Send @{cmd='exec';code=('auditShots=0; auditShooter setCombatMode "BLUE"; auditShooter setVehicleAmmo 1; auditTarget setDammage 0; auditTarget setPos [9700,'+(3600+$range)+',0]; auditShooter reveal auditTarget; auditShooter doTarget auditTarget; auditShooter setCombatMode "RED"')}
   Start-Sleep -Seconds 12
   $null=Send @{cmd='eval';code='[time,auditShots,alive auditTarget,getDammage auditTarget,auditShooter auditShooter knowsAbout auditTarget,magazines auditShooter]'}
   $null=Send @{cmd='screenshot';path=(Join-Path $out ("open-"+$range+".png"))}
   $null=Send @{cmd='exec';code='auditShooter setCombatMode "BLUE"'}
  }
  $offsets=if ($Scenario -eq 'Combat') {@(0,20)} else {@()}
  foreach ($offset in $offsets) {
   $null=Send @{cmd='exec';code=('auditShots=0; auditTarget setDammage 0; auditTarget setPos [9700,3700,0]; auditFriend setDammage 0; auditFriend setPos ['+(9700+$offset)+',3620,0]; auditShooter setVehicleAmmo 1; auditShooter reveal auditTarget; auditShooter setCombatMode "RED"')}
   Start-Sleep -Seconds 12
   $null=Send @{cmd='eval';code='[time,auditShots,alive auditTarget,getDammage auditTarget,alive auditFriend,getDammage auditFriend]'}
   $null=Send @{cmd='exec';code='auditShooter setCombatMode "BLUE"'}
  }
  if ($Scenario -notin @('Burst','BurstAim','Crossfire','LaneMove','Reposition','Cover','CoverProbe')) {
  # The installed HandGrenade AI envelope is 40..60 m. A 30 m control
  # never reaches grenade selection and cannot validate the safety gate.
  $null=Send @{cmd='exec';code='auditShooter setCombatMode "BLUE"; removeAllWeapons auditShooter; {auditShooter removeMagazine _x} forEach magazines auditShooter; auditShooter addMagazine "HandGrenade"; auditShooter addMagazine "HandGrenade"; auditShooter addMagazine "HandGrenade"; auditShooter addWeapon "Throw"; auditTarget setPos [9700,3645,0]'}
  foreach ($offset in @(50,0,50)) {
   $null=Send @{cmd='exec';code=('auditShots=0; auditTarget setPos [9700,3645,0]; auditFriend setPos ['+(9700+$offset)+',3645,0]; auditShooter reveal auditTarget; auditShooter doTarget auditTarget; auditShooter setCombatMode "RED"')}
   Start-Sleep -Seconds 18
   if (!$Reference) {$null=Send @{cmd='ai_combat'}}
   $null=Send @{cmd='eval';code='[time,auditShots,magazines auditShooter,getPos auditFriend,getPos auditTarget]'}
   $null=Send @{cmd='exec';code='auditShooter setCombatMode "BLUE"; auditShooter setVehicleAmmo 1'}
  }
  }
 } else {throw 'Use renderer-specific capture procedure for Shadows'}
 $null=Send @{cmd='exit'}
 if (!$p.WaitForExit(30000)) {throw 'Exit timed out'}
 if ($p.ExitCode -ne 0) {throw "Game exit $($p.ExitCode)"}
 if (Select-String -LiteralPath $log -Pattern 'Script error|Validation Error|panicked at|UNHANDLED EXCEPTION') {throw 'Runtime error; inspect engine.log'}
 Write-Output "Audit complete: $out"
} finally {
 if ($http -and !$http.HasExited) {Stop-Process -Id $http.Id; $http.WaitForExit()}
 if ($client) {$client.Dispose()}
 if ($p -and !$p.HasExited) {Stop-Process -Id $p.Id; $p.WaitForExit()}
}
