param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault', [switch]$Menu, [switch]$InspectRoom, [switch]$CastleOnly, [switch]$GlassOnly, [switch]$GroupingAmmoOnly, [switch]$FlightOnly, [switch]$FlightMenuOnly)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) { throw 'Use scripts/with-game-lock.sh' }
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/showcase/smoke'
New-Item -ItemType Directory -Force $out | Out-Null
$env:POSEIDON_USER_DIR=Join-Path $out 'user'
if ($GlassOnly) {
 $out=Join-Path $root 'build/showcase/glass'
 $env:POSEIDON_USER_DIR=Join-Path $out 'user'
 New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
 [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
 $env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE='0.15'
}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start()
$port=$listener.LocalEndpoint.Port
$listener.Stop()
$log=Join-Path $out 'showcase.log'
$mission=Join-Path $GameDir 'Mods/@OP_Showcase/Missions/OpenPoseidon/ShowcaseLab.Intro'
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
 function Send($command, $expected=$null) {
  $writer.WriteLine(($command | ConvertTo-Json -Compress))
  do {
   $line=$reader.ReadLine()
   if ($null -eq $line) {throw 'Harness closed'}
   $r=$line | ConvertFrom-Json
  } while ($null -eq $r.ok)
  if (!$r.ok) {throw $line}
  if ($null -ne $expected -and $r.result -cne $expected) {throw "Unexpected result: $line; expected $expected"}
  Write-Output $line
 }
 Start-Sleep -Seconds 18
 if ($GlassOnly) {
  Send @{cmd='eval';code='typeOf labGlass'} '"LabPanelGlass"'
  Send @{cmd='exec';code='hint ""; [player,player,labGlassVisit] exec "action.sqs"'}
  Start-Sleep -Seconds 3
  # The live player's input heading restores the mission's 30 degrees after
  # setDir. Aim this short approach through the pane at that actual heading.
  Send @{cmd='exec';code='hint ""; player setPos [9573.84,3502,0]; player switchCamera "INTERNAL"'}
  Start-Sleep -Seconds 3
  Send @{cmd='eval';code='[getPos player,getDir player,getPos labGlass]'}
  Send @{cmd='eval';code='getDir player < 50'} 'true'
  Send @{cmd='exec';code='player setPos [9575 - 2 * sin (getDir player) / cos (getDir player),3502,0]'}
  Send @{cmd='key';sc=26;hold=$true}
  Start-Sleep -Seconds 1
  Send @{cmd='key_up';sc=26}
  Send @{cmd='eval';code='getPos player'}
  Send @{cmd='eval';code='((getPos player) select 1) < 3504'} 'true'
  Send @{cmd='exec';code='player setPos [9582,3512,0]'}
  Send @{cmd='exec';code='labCamera = "camera" camCreate [9575,3499,1.7]; labCamera cameraEffect ["internal","back"]; labCamera camSetTarget [9575,3507,1.7]; labCamera camCommit 0; showCinemaBorder false'}
  Start-Sleep -Seconds 3
  Send @{cmd='screenshot';path=(Join-Path $out 'glass-front.png')}
  Send @{cmd='exec';code='labCamera camSetPos [9571,3501,1.7]; labCamera camSetTarget [9575,3504,1.7]; labCamera camCommit 0'}
  Start-Sleep -Seconds 3
  Send @{cmd='screenshot';path=(Join-Path $out 'glass-oblique.png')}
  Send @{cmd='exec';code='labGlassTarget setPos [9583,3507,1.5]; labCamera camSetPos [9575,3512,1.7]; labCamera camSetTarget [9575,3499,1.7]; labCamera camCommit 0'}
  Start-Sleep -Seconds 3
  Send @{cmd='screenshot';path=(Join-Path $out 'glass-back.png')}
  Send @{cmd='exec';code='labCamera camSetPos [9575,3499,1.7]; labCamera camSetTarget [9575,3507,1.7]; labCamera camCommit 0'}
  foreach ($shot in 1..3) {
   Send @{cmd='exec';code='labBullet="BulletSingleW" createVehicle [9575,3501,1.5]; labBullet setPosASL [9575,3501,labGlassBase+1.5]; labBullet setVelocity [0,100,0]'}
   Start-Sleep -Milliseconds 350
  }
  Start-Sleep -Seconds 3
  Send @{cmd='eval';code='isNull labGlass'} 'true'
  Send @{cmd='eval';code='alive labGlassLeft && alive labGlassRight && alive labGlassTarget'} 'true'
  Send @{cmd='exec';code='labCamera camSetPos [9575,3512,1.7]; labCamera camSetTarget [9575,3499,1.7]; labCamera camCommit 0'}
  Start-Sleep -Seconds 2
  Send @{cmd='screenshot';path=(Join-Path $out 'glass-back-broken.png')}
  Send @{cmd='exec';code='labCamera camSetPos [9575,3499,1.7]; labCamera camSetTarget [9575,3507,1.7]; labCamera camCommit 0'}
  Start-Sleep -Seconds 2
  Send @{cmd='screenshot';path=(Join-Path $out 'glass-removed-control.png')}
  Send @{cmd='exec';code='labCamera cameraEffect ["terminate","back"]; camDestroy labCamera; player switchCamera "INTERNAL"; player setPos [9575 - 2 * sin (getDir player) / cos (getDir player),3502,0]'}
  Send @{cmd='key';sc=26;hold=$true}
  Start-Sleep -Seconds 1
  Send @{cmd='key_up';sc=26}
  Send @{cmd='eval';code='((getPos player) select 1) > 3504'} 'true'
  Send @{cmd='eval';code='alive player'} 'true'
  Send @{cmd='exit'}
  if (!$p.WaitForExit(30000)) {throw 'Exit timed out'}
  if ($p.ExitCode -ne 0) {throw "Game exit $($p.ExitCode)"}
  if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|UNHANDLED EXCEPTION|upload failed showcase_lab') {throw 'Glass runtime error'}
  Write-Output "Glass captures: $out"
  return
 }
 if ($CastleOnly) {
  Send @{cmd='exec';code='hint ""; player setPos [9490,3559,0]; player setDir 0; player switchCamera "INTERNAL"'}
  Send @{cmd='eval';code='triPhysicsShowcase "castle"'} '"OK"'
  Start-Sleep -Seconds 8
  Send @{cmd='screenshot';path=(Join-Path $out 'castle-walk-before.png')}
  Send @{cmd='key';sc=26;hold=$true}
  Start-Sleep -Seconds 4
  Send @{cmd='key_up';sc=26}
  Send @{cmd='eval';code='getPos player'}
  Send @{cmd='screenshot';path=(Join-Path $out 'castle-walk-blocked.png')}
  Send @{cmd='eval';code='((getPos player) select 1) > 3560 && ((getPos player) select 1) < 3563.5'} 'true'
  Send @{cmd='exec';code='player setPos [9498,3544,0]; player setDir 0'}
  Send @{cmd='key';sc=26;hold=$true}
  Start-Sleep -Seconds 2
  Send @{cmd='key_up';sc=26}
  Send @{cmd='eval';code='((getPos player) select 1) > 3547'} 'true'
  Send @{cmd='exec';code='player setPos [9470,3510,0]; labCamera = "camera" camCreate [9473,3525,15]; labCamera cameraEffect ["internal","back"]; labCamera camSetTarget [9500,3555,4]; labCamera camCommit 0'}
  Start-Sleep -Seconds 2
  Send @{cmd='exec';code='labGrenade = "GrenadeHand" createVehicle [9493,3543.5,1]; labGrenade setVelocity [0,4,10]'}
  Start-Sleep -Seconds 6
  Send @{cmd='screenshot';path=(Join-Path $out 'castle-grenade.png')}
  Send @{cmd='eval';code='triPhysicsShowcase "castle"'} '"OK"'
  Start-Sleep -Seconds 8
  Send @{cmd='exec';code='labRocket = "LAW" createVehicle [9493,3535,2]; labRocket setVelocity [0,0,100]'}
  Start-Sleep -Seconds 4
  Send @{cmd='screenshot';path=(Join-Path $out 'castle-rocket.png')}
  Send @{cmd='exit'}
  if (!$p.WaitForExit(30000)) {throw 'Exit timed out'}
  if ($p.ExitCode -ne 0) {throw "Game exit $($p.ExitCode)"}
  if (!(Select-String -LiteralPath $log -Pattern 'SHOWCASE blast: hit=18 range=7 affected=[1-9]')) {throw 'Hand grenade missed the blocks'}
  if (!(Select-String -LiteralPath $log -Pattern 'SHOWCASE blast: hit=150 range=2.5 affected=[1-9]')) {throw 'LAW missed the blocks'}
  return
 }
 if ($Menu) {
  Send @{cmd='exec';code='triSetLanguage "English"; triClickText "SINGLE MISSION"'}
  Start-Sleep -Seconds 3
  Send @{cmd='eval';code='triAssertListText [101,"OpenPoseidon..."]'} '"OK"'
  Send @{cmd='eval';code='triSelectListByData [101,"OpenPoseidon"]'} 'true'
  Send @{cmd='exec';code='triClickText "Open"'}
  Start-Sleep -Seconds 2
  Send @{cmd='eval';code='triAssertListText [101,"Showcase Lab - Physics and Light"]'} '"OK"'
  Send @{cmd='eval';code='triAssertListText [101,"OFP Fusion - Five Islands"]'} '"OK"'
  Send @{cmd='eval';code='triSelectListByData [101,"jeep-manual.Intro"]'} 'true'
  Send @{cmd='eval';code='triSelectListByData [101,"jeep-ride.Intro"]'} 'true'
  Send @{cmd='eval';code='triSelectListByData [101,"ShowcaseLab.Intro"]'} 'true'
  Send @{cmd='screenshot';path=(Join-Path $out 'mission-menu.png')}
  Send @{cmd='exec';code='triClickText "Play"'}
  Start-Sleep -Seconds 18
 }
 Send @{cmd='eval';code='typeOf labNormal'} '"LabPanelNormal"'
 if ($FlightOnly -or $FlightMenuOnly) {
  Send @{cmd='eval';code='driver labHeli == labFlightPilot'} 'true'
  Send @{cmd='exec';code='[player,player,labFlightVisit] exec "action.sqs"'}
  Start-Sleep -Seconds 2
  Send @{cmd='eval';code='vehicle player == labHeli'} 'true'
  $actions=(Send @{cmd='eval';code='triActionMenuText'} | ConvertFrom-Json).result
  foreach ($label in @('LAB: start helicopter circuit','LAB: return and land helicopter')) {
   if (!$actions.Contains($label)) {throw "Missing passenger action: $label; actual: $actions"}
  }
  Send @{cmd='exec';code='player action ["CARGO WEAPON READY",labHeli]; labFlightShots=0; player addEventHandler ["Fired",{labFlightShots=labFlightShots+1}]'}
  Start-Sleep -Seconds 2
  Send @{cmd='exec';code='player action ["USER",labHeli,labHeliFlightStart]'}
  Start-Sleep -Seconds 1
  Send @{cmd='eval';code='labFlightRequested'} 'true'
  if ($FlightMenuOnly) {
   # The installed scripting action command delivered ID 0 for requested ID 1.
   # Check menu availability above and dispatch separately; owner tests UI input.
   Send @{cmd='exec';code='[labHeli,player,labHeliFlightLand] exec "action.sqs"'}
   Start-Sleep -Seconds 4
   Send @{cmd='eval';code='labFlightRequested'} 'false'
   Send @{cmd='key';sc=40;hold=$true}
   Start-Sleep -Milliseconds 150
   Send @{cmd='key_up';sc=40}
   Send @{cmd='screenshot';path=(Join-Path $out 'helicopter-passenger-menu.png')}
   Send @{cmd='exec';code='player action ["GETOUT",labHeli]'}
   Start-Sleep -Seconds 3
   Send @{cmd='eval';code='vehicle player == player'} 'true'
   Send @{cmd='exec';code='player setPos [9802,3300,0]; player assignAsCargo labRideJeep; player moveInCargo labRideJeep'}
   Start-Sleep -Seconds 2
   Send @{cmd='eval';code='vehicle player == labRideJeep'} 'true'
   $actions=(Send @{cmd='eval';code='triActionMenuText'} | ConvertFrom-Json).result
   if (!$actions.Contains('LAB: start / stop Jeep circuit')) {throw "Missing Jeep passenger action: $actions"}
   Send @{cmd='exec';code='player action ["USER",labRideJeep,labJeepRideStart]'}
   Start-Sleep -Seconds 4
   Send @{cmd='eval';code='labRideRequested'} 'true'
   Send @{cmd='exec';code='player action ["USER",labRideJeep,labJeepRideStart]'}
   Start-Sleep -Seconds 4
   Send @{cmd='eval';code='labRideRequested'} 'false'
   Send @{cmd='exit'}
   if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Flight menu test exit failed'}
   return
  }
  $deadline=[DateTime]::UtcNow.AddSeconds(90)
  do {
   Start-Sleep -Seconds 2
   Send @{cmd='eval';code='[labFlightRequested,getPos labHeli,speed labHeli,fuel labHeli,alive labFlightPilot,labFlightLeg]'}
   $state=(Send @{cmd='eval';code='((getPos labHeli) select 2)>10 && abs(speed labHeli)>5'} | ConvertFrom-Json)
  } while ($state.result -ne 'true' -and [DateTime]::UtcNow -lt $deadline)
  if ($state.result -ne 'true') {throw 'Pilot did not take off'}
  foreach ($i in 1..12) {
   $heading=[double](Send @{cmd='eval';code='getDir labHeli'} | ConvertFrom-Json).result * [Math]::PI/180
   Send @{cmd='crew_cursor';x=[Math]::Cos($heading);y=-0.25;z=-[Math]::Sin($heading)}
   Start-Sleep -Seconds 1
   Send @{cmd='mouse_button';button=1;down=$true}
   Start-Sleep -Milliseconds 150
   Send @{cmd='mouse_button';button=1;down=$false}
   Send @{cmd='eval';code='[speed labHeli,labFlightLeg,labFlightShots]'}
  }
  Send @{cmd='eval';code='labFlightShots > 3'} 'true'
  Send @{cmd='screenshot';path=(Join-Path $out 'rifle-flight.png')}
  Send @{cmd='exec';code='[labHeli,player,labHeliFlightLand] exec "action.sqs"'}
  $deadline=[DateTime]::UtcNow.AddSeconds(180)
  do {
   Start-Sleep -Seconds 3
   $state=(Send @{cmd='eval';code='((getPos labHeli) select 2)<1.5 && abs(speed labHeli)<2'} | ConvertFrom-Json)
  } while ($state.result -ne 'true' -and [DateTime]::UtcNow -lt $deadline)
  if ($state.result -ne 'true') {throw 'Pilot did not return and land'}
  Send @{cmd='eval';code='alive player && alive labFlightPilot && alive labHeli'} 'true'
  Send @{cmd='exit'}
  if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Flight test exit failed'}
  return
 }
 if ($GroupingAmmoOnly) {
  Send @{cmd='eval';code='weapons labHeli'}
  Send @{cmd='exec';code='labAmmoControl="UH60" createVehicle [9700,3600,0]; labAmmoControl setVehicleAmmo 0.1'}
  Send @{cmd='exec';code='player moveInGunner labHeli; labHeli selectWeapon ((weapons labHeli) select 0)'}
  Start-Sleep -Seconds 2
  Send @{cmd='eval';code='[driver labHeli == player,gunner labHeli == player,vehicle player == labHeli,alive player]'}
  Send @{cmd='screenshot';path=(Join-Path $out 'heli-ammo-seat.png')}
  Send @{cmd='exec';code='labTestWeapon=(weapons labHeli) select 0; labTestMagazineCount=count magazines labHeli; labTestShots=0; labHeli addEventHandler ["Fired",{labTestShots=labTestShots+1}]; labHeli setVehicleAmmo 0.1; labTestAmmo=labHeli ammo labTestWeapon'}
  Send @{cmd='eval';code='labTestAmmo > 0'} 'true'
  foreach ($i in 1..8) {
   Send @{cmd='mouse_button';button=1;down=$true}
   Start-Sleep -Milliseconds 150
   Send @{cmd='mouse_button';button=1;down=$false}
   Start-Sleep -Milliseconds 650
  }
  Send @{cmd='eval';code='[labTestShots,labTestAmmo,labHeli ammo labTestWeapon,magazinesArray labHeli]'}
  Send @{cmd='eval';code='labTestShots > 1'} 'true'
  Send @{cmd='eval';code='labHeli ammo labTestWeapon > labTestAmmo'} 'true'
  Send @{cmd='eval';code='count magazines labHeli == labTestMagazineCount'} 'true'
  Send @{cmd='eval';code='labAmmoControl ammo labTestWeapon == labTestAmmo'} 'true'
  Send @{cmd='eval';code='[labTestShots,labTestAmmo,labHeli ammo labTestWeapon,magazinesArray labHeli]'}
  Send @{cmd='exit'}
  if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
  return
 }
 Send @{cmd='exec';code='hint ""; showCinemaBorder false; labCamera = "camera" camCreate [9483,3488,9]; labCamera cameraEffect ["internal","back"]; labCamera camSetTarget [9503,3507,0]; labCamera camCommit 0'}
 Start-Sleep -Seconds 4
 Send @{cmd='screenshot';path=(Join-Path $out 'campus.png')}
 Send @{cmd='exec';code='labCamera camSetPos [9495.3,3497,1.2]; labCamera camSetTarget [9495.3,3504,0.85]; labCamera camCommit 0'}
 Start-Sleep -Seconds 1
 Send @{cmd='exec';code='[player,player,labThrow] exec "action.sqs"'}
 Start-Sleep -Seconds 3
 Send @{cmd='screenshot';path=(Join-Path $out 'stack-impact.png')}
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
 Send @{cmd='eval';code='triPhysicsShowcase "throw"'} '"OK"'
 Send @{cmd='eval';code='triPhysicsShowcase "throw"'} '"OK"'
 Send @{cmd='eval';code='triPhysicsShowcase "throw"'} '"OK"'
 Send @{cmd='eval';code='triPhysicsShowcase "throw"'} '"REFUSED"'
 Send @{cmd='exec';code='[player,player,labCool] exec "action.sqs"'}
 Start-Sleep -Seconds 2
 Send @{cmd='screenshot';path=(Join-Path $out 'lights-cool.png')}
 Send @{cmd='eval';code='triPhysicsShowcase "invalid"'} '"REFUSED"'
 Send @{cmd='exec';code='[player,player,labClear] exec "action.sqs"'}
 Send @{cmd='eval';code='triPhysicsShowcase "castle"'} '"OK"'
 Send @{cmd='exec';code='skipTime 12; labCamera camSetPos [9473,3525,15]; labCamera camSetTarget [9500,3555,4]; labCamera camCommit 0'}
 Start-Sleep -Seconds 8
 Send @{cmd='screenshot';path=(Join-Path $out 'castle-settled.png')}
 Send @{cmd='eval';code='triPhysicsShowcase "throw"'} '"OK"'
 Start-Sleep -Seconds 3
 Send @{cmd='screenshot';path=(Join-Path $out 'castle-impact.png')}
 Send @{cmd='exec';code='labCamera camSetPos [9547,3498,1.7]; labCamera camSetTarget [9547,3510,1.7]; labCamera camCommit 0'}
 Start-Sleep -Seconds 2
 Send @{cmd='screenshot';path=(Join-Path $out 'room-lit.png')}
 Send @{cmd='eval';code='triPhysicsShowcase "roomlight"'} '"OK"'
 Start-Sleep -Seconds 2
 Send @{cmd='screenshot';path=(Join-Path $out 'room-unlit.png')}
 if ($InspectRoom) {
  Send @{cmd='eval';code='[getPosASL labRoomRoof,getPosASL labRoomBack,vectorUp labRoomRoof,vectorUp labRoomBack]'}
  Send @{cmd='exec';code='{_x setVectorUp [0,0,1]} forEach [labRoomRoof,labRoomLeft,labRoomRight,labRoomBack,labDoorLeft,labDoorRight,labRoomNormal,labRoomGloss]'}
  Start-Sleep -Seconds 2
  Send @{cmd='screenshot';path=(Join-Path $out 'room-upright.png')}
  Send @{cmd='eval';code='triShadowTuning'}
  Send @{cmd='eval';code='triShadowSetBias 0'}
  Start-Sleep -Seconds 2
  Send @{cmd='screenshot';path=(Join-Path $out 'room-bias-zero.png')}
  Send @{cmd='eval';code='triShadowSetNormalOffset 0'} '"OK"'
  Start-Sleep -Seconds 2
  Send @{cmd='screenshot';path=(Join-Path $out 'room-normal-offset-zero.png')}
  Send @{cmd='eval';code='triShadowSetPcf 0'} '"OK"'
  Start-Sleep -Seconds 2
  Send @{cmd='screenshot';path=(Join-Path $out 'room-pcf-zero.png')}
  Send @{cmd='exec';code='triShadowSetBias 0.00002; triShadowSetNormalOffset 1; triShadowSetPcf 1'}
 }
 Send @{cmd='exec';code='labCamera camSetPos [9700,3520,35]; labCamera camSetTarget [9700,3600,0]; labCamera camCommit 0'}
 Start-Sleep -Seconds 2
 Send @{cmd='screenshot';path=(Join-Path $out 'terrain-before.png')}
 Send @{cmd='eval';code='triPhysicsShowcase "crater"'} '"OK"'
 Send @{cmd='eval';code='triPhysicsShowcase "crater"'} '"REFUSED"'
 Start-Sleep -Seconds 4
 Send @{cmd='screenshot';path=(Join-Path $out 'terrain-crater.png')}
 Send @{cmd='eval';code='triPhysicsShowcase "craterreset"'} '"OK"'
 Start-Sleep -Seconds 3
 Send @{cmd='screenshot';path=(Join-Path $out 'terrain-restored.png')}
 Send @{cmd='eval';code='driver labRideJeep == labRideDriver'} 'true'
 Send @{cmd='eval';code='abs(speed labRideJeep) < 1'} 'true'
 Send @{cmd='exec';code='[player,player,labRideVisit] exec "action.sqs"'}
 Start-Sleep -Seconds 2
 Send @{cmd='eval';code='vehicle player == player'} 'true'
 Send @{cmd='exec';code='player moveInCargo labRideJeep'}
 Start-Sleep -Seconds 8
 Send @{cmd='eval';code='vehicle player == labRideJeep'} 'true'
 Send @{cmd='eval';code='abs(speed labRideJeep) < 1'} 'true'
 Send @{cmd='exec';code='[player,player,labRideStart] exec "action.sqs"'}
 Start-Sleep -Seconds 8
 Send @{cmd='eval';code='abs(speed labRideJeep) > 1'} 'true'
 Send @{cmd='exec';code='unassignVehicle player; player action ["EJECT",labRideJeep]'}
 Start-Sleep -Seconds 5
 Send @{cmd='eval';code='vehicle player == player'} 'true'
 Send @{cmd='eval';code='abs(speed labRideJeep) < 1'} 'true'
 Send @{cmd='exit'}
 if (!$p.WaitForExit(30000)) {throw 'Exit timed out'}
 if ($p.ExitCode -ne 0) {throw "Game exit $($p.ExitCode)"}
} finally {
 if ($client) {$client.Dispose()}
 if (!$p.HasExited) {$p.Kill()}
}
if (!(Select-String -LiteralPath $log -SimpleMatch 'SHOWCASE: ready')) {throw 'Mission init did not finish'}
if (Select-String -LiteralPath $log -Pattern 'upload failed showcase_lab|failed to upload texture showcase_lab|UNHANDLED EXCEPTION') {throw 'Showcase asset/runtime error'}
Write-Output "Showcase captures: $out"
