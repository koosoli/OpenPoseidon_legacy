param([ValidateSet('M16','AK74')][string]$Rifle='M16',
      [ValidateSet('','Jeep','UAZ','Truck5tOpen','UH60','UH60MG')][string]$InspectVehicle='',
      [ValidateRange(0,11)][int]$InspectCargoIndex=0,
      [switch]$InspectAllSeats,
      [ValidateSet('Jeep','UAZ','Truck5tOpen','UH60','UH60MG')][string]$TestVehicle='Jeep',
      [switch]$ProjectileFrame, [switch]$AnimationPreview, [switch]$AnimationVideo, [switch]$NpcFire,
      [ValidateSet('clear','hold','outside','blocked','friendly')][string]$NpcScenario='clear',
      [ValidateSet(0,90,180)][int]$NpcHeading=0, [switch]$NpcDriver,
      [switch]$NpcTransitions, [switch]$NpcMoving, [switch]$NpcTurning, [switch]$SecondJeep,
      [ValidateSet('near','far')][string]$TurnTarget='near',
      [ValidateSet('individual','group','waypoint')][string]$DriverCommand='individual',
      [switch]$LimitedDriver,
      [switch]$PlayerFire, [switch]$Lifecycle, [switch]$SaveLoad, [switch]$MenuReentry, [switch]$MovingPlayer, [switch]$CarrierDamage,
      [switch]$DefaultPackage, [string]$UserMod='',
      [ValidateSet('','missing','incompatible')][string]$AutomaticOriginal='', [string]$MissionPath='',
      [string]$LoadOriginalSave='',
      [string]$StreamingWorld='', [switch]$SimulationResidency,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($StreamingWorld -and !(Test-Path -LiteralPath $StreamingWorld -PathType Leaf)) {throw 'Streaming world must exist'}
if ($SimulationResidency -and (!$StreamingWorld -or !$NpcFire -or $NpcScenario -ne 'clear' -or $PlayerFire -or !$DefaultPackage)) {throw 'Streaming consumer fixture requires a modern world, clear AI cargo, and default package'}
if ($StreamingWorld) {
    $env:WGR_SIMULATION_RESIDENCY=if ($SimulationResidency) {'1'} else {'0'}
    $env:WGR_SIMULATION_RESIDENCY_TEST=if ($SimulationResidency) {'1'} else {'0'}
    $env:WGR_SIMULATION_POSITIVE_COLD='0'
    $env:WGR_OBJECT_STREAM_GPU_BUDGET='0'
    $env:WGR_OBJECT_STREAM_PBO='0'
    $env:WGR_OBJECT_STREAM_WINDOW_GROWTH='0'
}
if ($InspectVehicle -and ($PlayerFire -or $NpcFire -or $DefaultPackage -or $AnimationPreview -or $ProjectileFrame)) {
    throw 'Original seat inspection requires a separate baseline launch without FFV content'
}
if ($NpcTurning) {$NpcMoving=$true}
if ($MenuReentry -and !$PlayerFire) {throw 'Menu reentry requires the player test'}
if ($DriverCommand -ne 'individual' -and !$NpcMoving) {throw 'Driver command requires a moving test'}
if ($LimitedDriver -and (!$NpcMoving -or $DriverCommand -ne 'individual')) {throw 'Limited subgroup hint requires the individual moving fixture'}
if ($PlayerFire) {
    if ($NpcFire -or $NpcMoving -or $NpcTransitions -or $NpcScenario -ne 'clear') {throw 'Player fire is a separate case'}
    $NpcFire=$true
}
if ($CarrierDamage -and (!$PlayerFire -or $Lifecycle -or $MenuReentry -or $MovingPlayer)) {throw 'Carrier damage requires stationary player fire without lifecycle/menu modes'}
if ($AnimationVideo -and !$AnimationPreview) {throw '-AnimationVideo requires -AnimationPreview'}
if ($NpcFire -and ($AnimationPreview -or $ProjectileFrame)) {throw 'Select one integration mode per launch'}
if (!$NpcFire -and $NpcScenario -ne 'clear') {throw '-NpcScenario requires -NpcFire'}
if (!$NpcFire -and ($NpcHeading -ne 0 -or $NpcDriver)) {throw 'NPC heading/driver requires -NpcFire'}
if ($NpcTransitions -and (!$NpcFire -or $NpcScenario -ne 'clear')) {throw 'Transition/reload requires the clear NPC case'}
if ($NpcMoving -and (!$NpcFire -or $NpcScenario -ne 'clear' -or $NpcHeading -ne 0 -or $NpcTransitions)) {throw 'Moving test requires clear heading-zero NPC case without reload'}
if ($NpcMoving) {$NpcDriver=$true}
if ($SecondJeep -and (!$NpcFire -or $NpcScenario -ne 'clear' -or $NpcMoving -or $PlayerFire)) {throw 'Second Jeep requires stationary clear NPC case'}
if ($Lifecycle -and (!$NpcFire -or $NpcScenario -ne 'clear' -or $NpcMoving)) {throw 'Lifecycle requires stationary clear cargo test'}
if ($SaveLoad -and (!$NpcTransitions -and !$PlayerFire)) {throw 'Save/load requires a reload test'}
if ($DefaultPackage -and ($AnimationPreview -or $ProjectileFrame)) {throw 'Default package is separate from preview/constructor tests'}
if ($AutomaticOriginal -and (!$DefaultPackage -or $NpcFire -or $UserMod)) {throw 'Automatic fallback requires the original baseline and automatic package mode'}
if ($LoadOriginalSave -and ($DefaultPackage -or $NpcFire -or $AnimationPreview -or $ProjectileFrame -or $UserMod -or !(Test-Path -LiteralPath $LoadOriginalSave -PathType Leaf))) {throw 'Saved-pose fallback requires a real fixture save and disabled VehicleActions'}
$root=Split-Path $PSScriptRoot -Parent
$stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw
$output=Join-Path $root ('build/jeep-ffv/baseline-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $env:POSEIDON_USER_DIR -DlssMode 0
$env:WGR_AUTO_EXPOSURE='0'
$env:WGR_EXPOSURE='0.15'
$stamp | Set-Content -LiteralPath (Join-Path $output 'DEPLOYED-FROM.txt')
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$mission=if ($MissionPath) {$MissionPath} else {Join-Path $root 'dev-missions/jeep-ffv.Intro'}
$log=Join-Path $output 'jeep.log'
$gameArgs=@('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
if ($StreamingWorld) {$gameArgs+=@('--test-world',('"'+$StreamingWorld+'"'))}
if (!$DefaultPackage) {$gameArgs+='--no-vehicle-actions'}
if (!$DefaultPackage -and ($AnimationPreview -or $NpcFire)) {
    $preview=Join-Path $root 'build/jeep-ffv/animation-preview'
    if (!(Test-Path -LiteralPath "$preview/AddOns/op_jeep_animation_preview.pbo")) {throw 'Build animation preview first'}
    $gameArgs+=@('--mod',('"'+$preview+'"'))
}
if ($UserMod) {
    if (!$DefaultPackage -or !(Test-Path -LiteralPath $UserMod -PathType Container)) {throw 'User override needs default-package mode and an existing mod folder'}
    $gameArgs+=@('--mod',('"'+$UserMod+'"'))
}
if ($DefaultPackage) {
    $packArchive=Join-Path $GameDir 'Mods/@OP_VehicleActions/AddOns/op_jeep_actions.pbo'
    if (!(Test-Path -LiteralPath $packArchive)) {
        if (!$AutomaticOriginal) {throw 'Default VehicleActions is not installed'}
        $packArchive=$null
    }
} elseif ($AnimationPreview -or $NpcFire) {
    $packArchive=Join-Path $preview 'AddOns/op_jeep_animation_preview.pbo'
} else {$packArchive=$null}
$packHash=$null
if ($packArchive) {
    $packStream=[IO.File]::OpenRead($packArchive)
    $sha=[Security.Cryptography.SHA256]::Create()
    try {$packHash=[BitConverter]::ToString($sha.ComputeHash($packStream)).Replace('-','')}
    finally {$packStream.Dispose(); $sha.Dispose()}
}
[pscustomobject]@{rifle=$Rifle;inspectVehicle=$InspectVehicle;inspectCargoIndex=$InspectCargoIndex;installed=$stamp.Trim();args=$gameArgs;source=(& git -C $root rev-parse HEAD);pack=$packArchive;packSha256=$packHash;driverCommand=$DriverCommand;limitedDriver=[bool]$LimitedDriver;turning=[bool]$NpcTurning;turnTarget=$TurnTarget} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'test-context.json')
if ($StreamingWorld) {
    @{scriptSha256=(Get-FileHash $PSCommandPath).Hash;world=(Get-FileHash $StreamingWorld);simulationResidency=[bool]$SimulationResidency;
      binaries=@(Get-FileHash (Join-Path $GameDir 'OpenPoseidon.exe'),(Join-Path $GameDir 'wgpu_renderer.dll'))} |
        ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output 'streaming-provenance.json')
}
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $gameArgs
$null=$p.Handle
$client=$null
function RifleRounds($response) {
    $items=ConvertFrom-Json -InputObject $response.result
    $rounds=0
    for ($i=0; $i -lt $items.Count; $i+=2) {
        if ($items[$i] -ceq $Rifle) {$rounds += [int]$items[$i+1]}
    }
    return $rounds
}
try {
    $client=[Net.Sockets.TcpClient]::new(); $deadline=[DateTime]::UtcNow.AddSeconds(90)
    while (!$client.Connected) {
        try {$client.Connect('127.0.0.1',$port)} catch {
            if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw}
            Start-Sleep -Milliseconds 250
        }
    }
    $stream=$client.GetStream(); $stream.ReadTimeout=30000
    $reader=[IO.StreamReader]::new($stream)
    $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush=$true
    function Send($command, $expected=$null) {
        $writer.WriteLine(($command | ConvertTo-Json -Compress))
        do {
            $line=$reader.ReadLine()
            if ($null -eq $line) {throw 'Harness closed'}
            $r=$line | ConvertFrom-Json
        } while ($null -eq $r.ok)
        $line | Add-Content -LiteralPath (Join-Path $output 'responses.jsonl')
        if (!$r.ok -or ($null -ne $expected -and $r.result -cne $expected)) {throw "Unexpected harness response: $line"}
        Write-Host $line
        return $r
    }
    function ResumeReviewCamera {
        if (!$PlayerFire) {
            $null=Send @{cmd='exec';code='deleteVehicle ffvView; ffvView="camera" camCreate [9701.8,3599.6,1.6]; ffvView cameraEffect ["internal","back"]; ffvView camSetTarget [9700.4,3600,1.35]; ffvView camCommit 0; showCinemaBorder false'}
        }
    }
    Start-Sleep -Seconds 12
    if ($Rifle -ne 'M16') {
        if ($LoadOriginalSave) {throw 'Original-save fixture owns its inventory; do not replace it'}
        $null=Send @{cmd='exec';code=('removeAllWeapons ffvPassenger; {ffvPassenger removeMagazine _x} forEach magazines ffvPassenger; ffvPassenger addMagazine "'+$Rifle+'"; ffvPassenger addMagazine "'+$Rifle+'"; ffvPassenger addWeapon "'+$Rifle+'"')}
    }
    if ($LoadOriginalSave) {
        # Discover this launch's test-save directory without assuming a profile name.
        $null=Send @{cmd='eval';code='triSaveGame "path-probe"'} '"OK"'
        $marker=@(Get-ChildItem -LiteralPath $env:POSEIDON_USER_DIR -Recurse -Filter 'path-probe.fps' -File)
        if ($marker.Count -ne 1) {throw 'Test save directory is ambiguous'}
        Copy-Item -LiteralPath $LoadOriginalSave -Destination (Join-Path $marker[0].DirectoryName 'original-cargo.fps')
        $null=Send @{cmd='eval';code='triLoadGame "original-cargo"'} '"OK"'
        Start-Sleep -Seconds 2
        $loadedAmmo=Send @{cmd='eval';code='magazinesArray ffvPassenger'}
        $loadedShots=Send @{cmd='eval';code='ffvShots'}
        ResumeReviewCamera
        Start-Sleep -Seconds 5
        $pose=Send @{cmd='crew_pose_probe'}
        $state=Send @{cmd='crew_seat_status'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'saved-pose-original.png')}
        $null=Send @{cmd='eval';code='magazinesArray ffvPassenger'} $loadedAmmo.result
        $null=Send @{cmd='eval';code='ffvShots'} $loadedShots.result
        if (!$state.alive -or !$state.inCargo -or $state.active -or $pose.primaryMove -notlike 'jeepcodriver*' -or $pose.reloadInProgress) {throw 'Disabling package did not restore the original Jeep co-driver pose'}
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Saved-pose original fallback exit failed'}
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'Saved-pose fallback runtime error'}
        Write-Host "Saved cargo pose with package disabled passed: $output"
        return
    }
    if ($DefaultPackage) {
        $diagnostic=switch ($AutomaticOriginal) {
            'missing' {'required archive missing or empty'}
            'incompatible' {'missing or incompatible defaultContentApi'}
            default {'compatible package found'}
        }
        if (!(Select-String -LiteralPath $log -SimpleMatch -Pattern "Default content: @OP_VehicleActions: $diagnostic" -Quiet)) {
            throw "Expected automatic-package diagnostic: $diagnostic"
        }
    }
    if ($InspectVehicle) {
        $null=Send @{cmd='exec';code=('deleteVehicle ffvJeep; ffvJeep="'+$InspectVehicle+'" createVehicle [9700,3600,0]; ffvJeep setDir 0; ffvSeatGroup=createGroup west')}
        for ($seatIndex=0; $seatIndex -lt $InspectCargoIndex; $seatIndex++) {
            $null=Send @{cmd='exec';code='"SoldierWB" createUnit [[9700,3600,0],ffvSeatGroup,"this setCombatMode ""BLUE""; this moveInCargo ffvJeep"]'}
        }
        $null=Send @{cmd='exec';code='ffvPassenger moveInCargo ffvJeep; ffvView="camera" camCreate [9707,3607,4]; ffvView cameraEffect ["internal","back"]; ffvView camSetTarget [9700,3600,1.8]; ffvView camCommit 0; showCinemaBorder false'}
        Start-Sleep -Seconds 3
        $original=Send @{cmd='crew_pose_probe'}
        if ($original.cargoIndex -ne $InspectCargoIndex -or $original.cargoWeaponActive) {throw 'Original seat inspection resolved the wrong seat or enabled FFV'}
        if ($InspectAllSeats) {
            for ($i=1;$i -lt 12;$i++) {
                $null=Send @{cmd='exec';code='unassignVehicle ffvPassenger; ffvPassenger action ["EJECT",ffvJeep]'}
                Start-Sleep -Milliseconds 500
                $null=Send @{cmd='exec';code='"SoldierWB" createUnit [[9700,3600,0],ffvSeatGroup,"this setCombatMode ""BLUE""; this moveInCargo ffvJeep"]; ffvPassenger moveInCargo ffvJeep'}
                Start-Sleep -Milliseconds 400
                $null=Send @{cmd='crew_pose_probe'}
            }
        }
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'original-seat.png')}
        $null=Send @{cmd='exec';code='ffvView camSetPos [(getPos ffvJeep select 0)+5,(getPos ffvJeep select 1)-6,4]; ffvView camSetTarget [(getPos ffvJeep select 0),(getPos ffvJeep select 1),2]; ffvView camCommit 0'}
        Start-Sleep -Seconds 1
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'original-seat-rear.png')}
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Original seat inspection did not shut down cleanly'}
        Write-Host "Original seat recorded: $InspectVehicle cargo $InspectCargoIndex; $output"
        return
    }
    if ($TestVehicle -ne 'Jeep') {
        $null=Send @{cmd='exec';code=('deleteVehicle ffvJeep; ffvJeep="'+$TestVehicle+'" createVehicle [9700,3600,0]; ffvJeep setDir 0')}
        $null=Send @{cmd='exec';code='ffvSeatGroup=createGroup west'}
        for ($i=0;$i -lt $InspectCargoIndex;$i++) {
            $null=Send @{cmd='exec';code='"SoldierWB" createUnit [[9700,3600,0],ffvSeatGroup,"removeAllWeapons this; this setCombatMode ""BLUE""; this moveInCargo ffvJeep"]'}
        }
    }
    $null=Send @{cmd='eval';code='typeOf ffvJeep'} ('"'+$TestVehicle+'"')
    if ($NpcFire) {
        $angle=$NpcHeading * [math]::PI / 180
        $rightX=[int][math]::Round([math]::Cos($angle)); $rightZ=-[int][math]::Round([math]::Sin($angle))
        $frontX=-$rightZ; $frontZ=$rightX
        $null=Send @{cmd='exec';code=('ffvJeep setDir {0}; ffvTarget setPos [{1},{2},0]' -f $NpcHeading,(9700+60*$rightX),(3600+60*$rightZ))}
        # Place moving-test targets before reveal; moving a known target later
        # tests stale perception rather than the intended seat/steering case.
        if ($NpcTurning) {
            $position=if ($TurnTarget -eq 'far') {'[9780,3640,0]'} else {'[9730,3620,0]'}
            # A newly placed target has no pre-mission memory at the old position.
            $null=Send @{cmd='exec';code=('ffvTurnTargetGroup=createGroup east; deleteVehicle ffvTarget; "SoldierEB" createUnit ['+$position+',ffvTurnTargetGroup,"ffvTarget=this; removeAllWeapons this; this disableAI ""MOVE"""]')}
        }
        elseif ($NpcMoving) {$null=Send @{cmd='exec';code='ffvTarget setPos [9760,3610,0]'}}
        if ($NpcDriver) {
            $driverInit=if ($NpcMoving) {'ffvDriver=this; removeAllWeapons this'} else {'ffvDriver=this; this disableAI ""MOVE""; removeAllWeapons this'}
            $driverGroup='group player'
            if ($NpcMoving) {
                $null=Send @{cmd='exec';code='ffvJeep setFuel 0; ffvDriverGroup=createGroup west'}
                $driverGroup='ffvDriverGroup'
            }
            $null=Send @{cmd='exec';code=('"SoldierWB" createUnit [[9690,3590,0],'+$driverGroup+',"'+$driverInit+'"]; ffvDriver moveInDriver ffvJeep')}
        }
    }
    $null=Send @{cmd='exec';code='removeAllWeapons player; ffvShots=0; ffvPassenger addEventHandler ["Fired",{ffvShots=ffvShots+1}]; ffvPassenger setCombatMode "BLUE"; ffvPassenger assignAsCargo ffvJeep; ffvPassenger moveInCargo ffvJeep; ffvView="camera" camCreate [9704,3604,2.2]; ffvView cameraEffect ["internal","back"]; ffvView camSetTarget [9700,3600,1]; ffvView camCommit 0; showCinemaBorder false'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='eval';code='vehicle ffvPassenger == ffvJeep'} 'true'
    $null=Send @{cmd='eval';code='weapons ffvPassenger'}
    $before=Send @{cmd='eval';code=('ffvPassenger ammo "'+$Rifle+'"')}
    $magazinesBefore=Send @{cmd='eval';code='magazinesArray ffvPassenger'}
    $null=Send @{cmd='eval';code='alive ffvTarget && getDammage ffvTarget == 0'} 'true'
    if ($before.result -ne '30') {throw 'Passenger fired before baseline capture'}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'seated.png')}
    if (!$NpcFire -and !$AnimationPreview) {
        $originalSeat=Send @{cmd='crew_pose_probe'}
        if ($originalSeat.cargoWeaponActive -or $originalSeat.primaryMove -like 'opjeep*') {
            throw 'Original cargo fallback retained the upgraded seat/animation'
        }
    }
    if ($NpcFire) {
        $null=Send @{cmd='eval';code='local ffvPassenger'}
        $null=Send @{cmd='eval';code='getPos ffvTarget'}
        if ($NpcScenario -eq 'outside') {
            $null=Send @{cmd='exec';code=('ffvTarget setPos [{0},{1},0]' -f (9700+60*$frontX),(3600+60*$frontZ))}
        }
        if ($NpcScenario -eq 'blocked') {
            $null=Send @{cmd='exec';code=('ffvBlock="Ural" createVehicle [{0},{1},0]; ffvBlock setDir {2}' -f (9700+4*$rightX),(3600+4*$rightZ),$NpcHeading)}
        }
        if ($NpcScenario -eq 'friendly') {
            $null=Send @{cmd='exec';code=('"SoldierWB" createUnit [[{0},{1},0],group player,"ffvFriendly=this; this disableAI ""MOVE""; removeAllWeapons this; this setUnitPos ""UP"""]' -f (9700+4*$rightX),(3600+4*$rightZ))}
        }
        $entry=Send @{cmd='crew_pose_probe'}
        if (!$PlayerFire -and !$entry.cargoWeaponActive) {throw 'Automatic cargo animation entry failed'}
        $null=Send @{cmd='exec';code='ffvPassenger reveal ffvTarget; ffvPassenger doTarget ffvTarget'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='eval';code='ffvShots'} '0'
        $activation=Send @{cmd='crew_pose_probe'}
        if (!$PlayerFire -and !$activation.cargoWeaponActive) {throw 'NPC fixture did not activate the explicitly configured seat'}
    }
    if ($SecondJeep) {
        $null=Send @{cmd='exec';code='ffvSecondJeep="Jeep" createVehicle [9700,3700,0]; ffvSecondJeep setDir 0; ffvSecondGroup=createGroup west; ffvSecondShots=0; "SoldierWB" createUnit [[9700,3700,0],ffvSecondGroup,"ffvSecond=this; this setCombatMode ""BLUE""; removeAllWeapons this"]; {ffvSecond removeMagazine _x} forEach magazines ffvSecond; ffvSecond addMagazine "M16"; ffvSecond addWeapon "M16"; ffvSecond moveInCargo ffvSecondJeep; "SoldierEB" createUnit [[9760,3700,0],group ffvTarget,"ffvSecondTarget=this; removeAllWeapons this; this disableAI ""MOVE"""]; ffvSecond addEventHandler ["Fired",{ffvSecondShots=ffvSecondShots+1}]; ffvSecond reveal ffvSecondTarget; ffvSecond doTarget ffvSecondTarget'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='eval';code='alive ffvSecondTarget && getDammage ffvSecondTarget == 0'} 'true'
        $null=Send @{cmd='exec';code='ffvSecond setCombatMode "YELLOW"; ffvSecond doFire ffvSecondTarget'}
        Start-Sleep -Seconds 12
        $secondAmmo=Send @{cmd='eval';code='ffvSecond ammo "M16"'}
        $secondShots=Send @{cmd='eval';code='ffvSecondShots'}
        $secondDamage=Send @{cmd='eval';code='getDammage ffvSecondTarget'}
        $null=Send @{cmd='eval';code='vehicle ffvSecond == ffvSecondJeep'} 'true'
        if ([int]$secondShots.result -le 0 -or [int]$secondAmmo.result -ge 30 -or [double]$secondDamage.result -le 0) {throw 'Second independent cargo passenger did not hit'}
        $null=Send @{cmd='eval';code='ffvShots'} '0'
        $null=Send @{cmd='eval';code=('ffvPassenger ammo "'+$Rifle+'"')} '30'
        $null=Send @{cmd='exec';code='ffvSecond setCombatMode "BLUE"; deleteVehicle ffvSecondTarget; ffvSecondJeep setDammage 1'}
        Start-Sleep -Seconds 2
        $primary=Send @{cmd='crew_seat_status'}
        if (!$primary.active -or !$primary.alive) {throw 'Second carrier destruction affected primary cargo state'}
        [pscustomobject]@{ammo=$secondAmmo.result;shots=$secondShots.result;damage=$secondDamage.result;primary=$primary} |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'second-jeep.json')
    }
    if ($AnimationPreview) {
        $null=Send @{cmd='crew_pose_probe'}
        $null=Send @{cmd='exec';code='ffvView camSetPos [9701.8,3599.6,1.6]; ffvView camSetTarget [9700.4,3600,1.35]; ffvView camCommit 0'}
        Start-Sleep -Seconds 1
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'pose-original-side.png')}
        $null=Send @{cmd='exec';code='deleteVehicle ffvTarget; ffvPassenger setBehaviour "SAFE"; ffvView camSetPos [9703,3602,1.8]; ffvView camSetTarget [9700.4,3600,1.0]; ffvView camCommit 0'}
        foreach ($pose in @('Idle','Raise','Aim','Recoil','Reload','Lower')) {
            $null=Send @{cmd='exec';code=('ffvPassenger switchMove "OPJeep'+$pose+'"')}
            Start-Sleep -Milliseconds 250
            $null=Send @{cmd='eval';code='vehicle ffvPassenger == ffvJeep'} 'true'
            $probe=Send @{cmd='crew_pose_probe'}
            if ($probe.move -ine ('OPJeep'+$pose)) {throw 'Requested animation state is not active'}
            $null=Send @{cmd='screenshot';path=(Join-Path $output ("pose-$pose.png"))}
        }
        $null=Send @{cmd='exec';code='ffvPassenger switchMove "OPJeepAim"; ffvView camSetPos [9701.8,3599.6,1.6]; ffvView camSetTarget [9700.4,3600,1.35]; ffvView camCommit 0'}
        Start-Sleep -Seconds 1
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'pose-aim-side.png')}
        if ($AnimationVideo) {
            $samples=@()
            $clock=[Diagnostics.Stopwatch]::StartNew()
            foreach ($pose in @('Idle','Raise','Aim','Recoil','Reload','Lower')) {
                $null=Send @{cmd='exec';code=('ffvPassenger switchMove "OPJeep'+$pose+'"')}
                $end=$clock.Elapsed.TotalSeconds + $(if ($pose -eq 'Reload') {3.0} else {1.5})
                do {
                    $name='motion-{0:D4}.png' -f $samples.Count
                    $beforeCapture=$clock.Elapsed.TotalSeconds
                    $null=Send @{cmd='screenshot';path=(Join-Path $output $name)}
                    $afterCapture=$clock.Elapsed.TotalSeconds
                    $samples += [pscustomobject]@{file=$name;pose=$pose;time=($beforeCapture+$afterCapture)*0.5;captureSeconds=$afterCapture-$beforeCapture}
                    Start-Sleep -Milliseconds 60
                } while ($clock.Elapsed.TotalSeconds -lt $end)
            }
            $samples | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'motion-samples.json')
        }
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Animation preview exit failed'}
        if (Select-String -LiteralPath $log -Pattern 'Bad animation|Broken animation|Animation .*not found|Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Animation preview load/render error'}
        if ($AnimationVideo) {
            & python "$PSScriptRoot/default-content/make_jeep_pose_video.py" $output
            if ($LASTEXITCODE -ne 0) {throw 'Pose video encoding failed'}
        }
        Write-Host "Animation frames require visual inspection: $output"
        return
    }
    if ($PlayerFire) {
        $null=Send @{cmd='exec';code='ffvView cameraEffect ["terminate","back"]'}
        $null=Send @{cmd='crew_take_control'}
        $null=Send @{cmd='eval';code='player == ffvPassenger'} 'true'
        if ($MenuReentry) {
            $entry=Send @{cmd='crew_pose_probe'}
            if ($null -eq $entry.entryX -or $entry.entryRadius -le 0) {throw 'Fixture has no valid assigned entry point'}
            Start-Sleep -Seconds 2
            $null=Send @{cmd='screenshot';path=(Join-Path $output 'menu-before-exit.png')}
            $null=Send @{cmd='key';sc=40;hold=$true}
            Start-Sleep -Milliseconds 250
            $null=Send @{cmd='key_up';sc=40}
            Start-Sleep -Seconds 2
            $exitState=Send @{cmd='crew_seat_status'}
            if ($exitState.inCargo -or $exitState.active) {throw 'Action-menu exit did not release cargo capability'}
            $heading=[math]::Atan2(9700-$entry.entryX,3600-$entry.entryZ)*180/[math]::PI
            $placement=[string]::Format([Globalization.CultureInfo]::InvariantCulture,
                'ffvPassenger assignAsCargo ffvJeep; ffvPassenger setPos [{0},{1},0]; ffvPassenger setDir {2}', $entry.entryX,$entry.entryZ,$heading)
            $null=Send @{cmd='exec';code=$placement}
            Start-Sleep -Seconds 2
            $null=Send @{cmd='screenshot';path=(Join-Path $output 'menu-before-entry.png')}
            $null=Send @{cmd='key';sc=40;hold=$true}
            Start-Sleep -Milliseconds 250
            $null=Send @{cmd='key_up';sc=40}
            Start-Sleep -Seconds 8
            $null=Send @{cmd='screenshot';path=(Join-Path $output 'menu-after-entry.png')}
            $state=Send @{cmd='crew_seat_status'}
            if (!$state.inCargo -or $state.active) {throw 'Entry must leave the optional weapon stance disabled'}
            $null=Send @{cmd='key';sc=48;hold=$true}
            Start-Sleep -Milliseconds 150
            $null=Send @{cmd='key_up';sc=48}
            $null=Send @{cmd='screenshot';path=(Join-Path $output 'menu-ready-selected.png')}
            $null=Send @{cmd='key';sc=40;hold=$true}
            Start-Sleep -Milliseconds 150
            $null=Send @{cmd='key_up';sc=40}
            Start-Sleep -Seconds 3
            $state=Send @{cmd='crew_seat_status'}
            if (!$state.inCargo -or !$state.active) {throw 'Ready action did not enable the optional weapon stance'}
            & "$PSScriptRoot/Test-JeepMouse.ps1" -Reader $reader -Writer $writer -LogPath (Join-Path $output 'mouse-responses.jsonl')
            if (!$?) {throw 'Relative mouse acceptance failed'}
            $null=Send @{cmd='key';sc=48;hold=$true}
            Start-Sleep -Milliseconds 150
            $null=Send @{cmd='key_up';sc=48}
            $null=Send @{cmd='screenshot';path=(Join-Path $output 'menu-stow-selected.png')}
            $null=Send @{cmd='key';sc=40;hold=$true}
            Start-Sleep -Milliseconds 150
            $null=Send @{cmd='key_up';sc=40}
            Start-Sleep -Seconds 2
            $state=Send @{cmd='crew_seat_status'}
            if (!$state.inCargo -or $state.active) {throw 'Stow action did not restore ordinary cargo'}
            $shotCount=Send @{cmd='eval';code='ffvShots'}
            $null=Send @{cmd='mouse_button';button=1;down=$true}
            Start-Sleep -Milliseconds 200
            $null=Send @{cmd='mouse_button';button=1;down=$false}
            $null=Send @{cmd='eval';code='ffvShots'} $shotCount.result
            $null=Send @{cmd='screenshot';path=(Join-Path $output 'menu-stowed.png')}
            $null=Send @{cmd='key';sc=48;hold=$true}
            Start-Sleep -Milliseconds 150
            $null=Send @{cmd='key_up';sc=48}
            $null=Send @{cmd='key';sc=40;hold=$true}
            Start-Sleep -Milliseconds 150
            $null=Send @{cmd='key_up';sc=40}
            Start-Sleep -Seconds 2
            $state=Send @{cmd='crew_seat_status'}
            if (!$state.active -or !$state.inCargo) {throw 'Ready after stowing failed'}
            & "$PSScriptRoot/Test-JeepMouse.ps1" -Reader $reader -Writer $writer -LogPath (Join-Path $output 'mouse-reactivation.jsonl')
            if (!$?) {throw 'Firing after stowing and readying again failed'}
            $null=Send @{cmd='exit'}
            if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Menu/mouse fixture exit failed'}
            Write-Host "PASS: menu reentry and relative mouse with $Rifle; $output"
            return
        }
        $null=Send @{cmd='exec';code='ffvPassenger action ["CARGO WEAPON READY",ffvJeep]'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='crew_cursor';x=60;y=0.01;z=-0.07}
        Start-Sleep -Seconds 3
        $aimPose=Send @{cmd='crew_pose_probe'}
        $targetPosition=(Send @{cmd='eval';code='getPosASL ffvTarget'}).result | ConvertFrom-Json
        $null=Send @{cmd='crew_cursor';x=([double]$targetPosition[0]-$aimPose.muzzleX);y=([double]$targetPosition[2]+1.3-$aimPose.muzzleY);z=([double]$targetPosition[1]-$aimPose.muzzleZ)}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='eval';code='ffvShots'} '0'
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'player-before-fire.png')}
        if ($MovingPlayer) {
            $null=Send @{cmd='exec';code='deleteVehicle ffvTarget; ffvDriveGroup=createGroup west; "SoldierWB" createUnit [[9695,3595,0],ffvDriveGroup,"ffvTestDriver=this; this setBehaviour ""CARELESS""; this setCombatMode ""BLUE"""]; ffvTestDriver moveInDriver ffvJeep; ffvDriveGroup move [9700,4100,0]'}
            $null=Send @{cmd='exec';code='ffvJeep setFuel 1; ffvTestDriver assignAsDriver ffvJeep; ffvTestDriver moveInDriver ffvJeep; ffvTestDriver doMove [9700,4100,0]'}
            Start-Sleep -Seconds 6
            $null=Send @{cmd='eval';code='abs(speed ffvJeep)>5'} 'true'
            for ($i=0; $i -lt 24; ++$i) {
                $null=Send @{cmd='eval';code='[speed ffvJeep,getDir ffvJeep,ffvShots]'}
                $null=Send @{cmd='crew_pose_probe'}
                $null=Send @{cmd='mouse_button';button=1;down=$true}
                Start-Sleep -Milliseconds 150
                $null=Send @{cmd='mouse_button';button=1;down=$false}
                Start-Sleep -Milliseconds 200
            }
            $null=Send @{cmd='screenshot';path=(Join-Path $output 'moving-player.png')}
            $null=Send @{cmd='exit'}
            if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Moving player exit failed'}
            Write-Host "Moving player diagnostics: $output"
            return
        }
        for ($attempt=0; $attempt -lt 12; $attempt++) {
            $aimPose=Send @{cmd='crew_pose_probe'}
            $targetPosition=(Send @{cmd='eval';code='getPosASL ffvTarget'}).result | ConvertFrom-Json
            $null=Send @{cmd='crew_cursor';x=([double]$targetPosition[0]-$aimPose.muzzleX);y=([double]$targetPosition[2]+1.3-$aimPose.muzzleY);z=([double]$targetPosition[1]-$aimPose.muzzleZ)}
            $null=Send @{cmd='mouse_button';button=1;down=$true}
            Start-Sleep -Milliseconds 180
            $null=Send @{cmd='mouse_button';button=1;down=$false}
            Start-Sleep -Milliseconds 180
            $hit=Send @{cmd='eval';code='getDammage ffvTarget'}
            $null=Send @{cmd='eval';code='[alive ffvPassenger,getDammage ffvPassenger,ffvShots,vehicle ffvPassenger == ffvJeep]'}
            $null=Send @{cmd='crew_seat_status'}
            $null=Send @{cmd='crew_pose_probe'}
            $null=Send @{cmd='eval';code='alive ffvPassenger && getDammage ffvPassenger == 0'} 'true'
            if ([double]$hit.result -gt 0) {break}
        }
    }
    if ($NpcMoving) {
        # doMove creates a separate subgroup with SpeedNormal. Use the existing
        # group move for an actual LIMITED run; retain individual as the stress case.
        $driverMove=if ($DriverCommand -eq 'group') {'ffvDriverGroup move'} else {'ffvDriver doMove'}
        if ($DriverCommand -ne 'individual') {
            # The fuel-zero setup can make group AI unassign an immobile vehicle.
            $null=Send @{cmd='exec';code='ffvJeep setFuel 1; ffvDriver assignAsDriver ffvJeep; [ffvDriver] orderGetIn true; ffvDriver moveInDriver ffvJeep'}
            $null=Send @{cmd='eval';code='driver ffvJeep == ffvDriver'} 'true'
        }
        $initialMove=if ($DriverCommand -eq 'waypoint') {
            'ffvDriverWaypoint=ffvDriverGroup addWaypoint [[9700,3680,0],0]; ffvDriverWaypoint setWaypointType "MOVE"; ffvDriverWaypoint setWaypointSpeed "LIMITED"; ffvDriverWaypoint setWaypointBehaviour "CARELESS"'
        } else {$driverMove+' [9700,3680,0]'}
        $null=Send @{cmd='exec';code=('ffvMovingShots=0; ffvPassenger addEventHandler ["Fired",{if (abs(speed ffvJeep)>5) then {ffvMovingShots=ffvMovingShots+1}}]; ffvJeep setFuel 1; ffvDriver setBehaviour "CARELESS"; ffvDriver setSpeedMode "LIMITED"; '+$initialMove+'; ffvView camSetPos [9706,3615,3]; ffvView camSetTarget ffvJeep; ffvView camCommit 0')}
        if ($NpcTurning) {
            $turnMove=if ($DriverCommand -eq 'waypoint') {'ffvDriverWaypoint setWaypointPosition [[9660,3680,0],0]'} else {$driverMove+' [9660,3680,0]'}
            $null=Send @{cmd='exec';code=('ffvTurnShots=0; ffvShotHeadings=[]; ffvPassenger addEventHandler ["Fired",{ffvShotHeadings=ffvShotHeadings+[getDir ffvJeep]; if ((abs(speed ffvJeep)>5) && (abs(sin(getDir ffvJeep))>0.087)) then {ffvTurnShots=ffvTurnShots+1}}]; '+$turnMove)}
        }
        if ($LimitedDriver) {
            $hint=Send @{cmd='crew_driver_limited'}
            if (!$hint.limited) {throw 'Actual driver subgroup did not accept LIMITED'}
        }
        $driveDeadline=[DateTime]::UtcNow.AddSeconds(20)
        do {
            Start-Sleep -Milliseconds 250
            $driveSpeed=Send @{cmd='eval';code='abs(speed ffvJeep)'}
            $turnReady=$true
            if ($NpcTurning) {$turnReady=(Send @{cmd='eval';code='abs(sin(getDir ffvJeep))>0.087'}).result -ceq 'true'}
        } while (([double]$driveSpeed.result -le 5 -or !$turnReady) -and [DateTime]::UtcNow -lt $driveDeadline)
        $null=Send @{cmd='eval';code='[driver ffvJeep == ffvDriver,vehicle ffvDriver == ffvJeep,unitReady ffvDriver,speedMode ffvDriverGroup,behaviour ffvDriver,getPos ffvDriver]'}
        if ([double]$driveSpeed.result -le 5 -or !$turnReady) {throw 'AI driver never reached the required motion/heading'}
        $null=Send @{cmd='eval';code='ffvShots'} '0'
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'moving-before-fire.png')}
    }
    if ($SimulationResidency) {
        $beforeNaturalFire=Send @{cmd='stream_simulation_residency'}
        if ($beforeNaturalFire.queryRegions -ne 0) {throw 'Read-only cargo diagnostics created Fire interest'}
    }
    if (!$PlayerFire -and (!$NpcFire -or $NpcScenario -ne 'hold')) {
        $null=Send @{cmd='exec';code='ffvPassenger reveal ffvTarget; ffvPassenger doTarget ffvTarget; ffvPassenger setCombatMode "YELLOW"; ffvPassenger doFire ffvTarget'}
    }
    if ($NpcMoving) {
        $samples=@(); $clock=[Diagnostics.Stopwatch]::StartNew()
        do {
            $motion=Send @{cmd='crew_pose_probe'}
            $drive=Send @{cmd='eval';code='[getPos ffvJeep,getDir ffvJeep,speed ffvJeep]'}
            $name='motion-{0:D4}.png' -f $samples.Count
            $beforeCapture=$clock.Elapsed.TotalSeconds
            $null=Send @{cmd='screenshot';path=(Join-Path $output $name)}
            $afterCapture=$clock.Elapsed.TotalSeconds
            $samples += [pscustomobject]@{file=$name;pose=$motion.primaryMove;phase=$motion.phase;time=($beforeCapture+$afterCapture)*.5;captureSeconds=$afterCapture-$beforeCapture;vehicle=$drive.result;probe=$motion}
            Start-Sleep -Milliseconds 100
        } while ($clock.Elapsed.TotalSeconds -lt 10)
        $samples | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $output 'motion-samples.json')
    } elseif ($SimulationResidency) {
        # Only observe natural AI firing. Never queue from the harness or call
        # the pose evaluator during this interval to establish consumer proof.
        $consumerSamples=@();$consumerDeadline=[DateTime]::UtcNow.AddSeconds(10)
        do {
            Start-Sleep -Milliseconds 250
            $consumerState=Send @{cmd='stream_simulation_residency'}
            $naturalShots=Send @{cmd='eval';code='ffvShots'}
            $consumerSamples+=@{state=$consumerState;shots=[int]$naturalShots.result}
        } while ([DateTime]::UtcNow -lt $consumerDeadline)
        $consumerSamples | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'streaming-consumer.json')
        if (!@($consumerSamples | Where-Object {$_.state.readyQueries -gt 0 -and $_.state.fireCollisionCalls -gt 0 -and $_.shots -gt 0}).Count) {throw 'Natural AI cargo firing did not exercise a Ready streaming query'}
    } else {Start-Sleep -Seconds 10}
    $after=Send @{cmd='eval';code=('ffvPassenger ammo "'+$Rifle+'"')}
    $seated=Send @{cmd='eval';code='vehicle ffvPassenger == ffvJeep'}
    $damage=Send @{cmd='eval';code='getDammage ffvTarget'}
    $cargoShots=Send @{cmd='eval';code='ffvShots'}
    if ($NpcMoving) {
        $movingShots=Send @{cmd='eval';code='ffvMovingShots'}
        $driveEnd=Send @{cmd='eval';code='getPos ffvJeep'}
        $null=Send @{cmd='crew_pose_probe'}
        if ([int]$movingShots.result -le 0) {throw 'No actual passenger shot during AI-driven movement'}
        if ($NpcTurning) {
            $turnShots=Send @{cmd='eval';code='ffvTurnShots'}
            $null=Send @{cmd='eval';code='ffvShotHeadings'}
            if ([int]$turnShots.result -le 0) {throw 'No actual passenger shot after AI steering changed heading by five degrees'}
        }
    }
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'ordered-fire.png')}
    if ($NpcFire) {
        $null=Send @{cmd='eval';code='getPos ffvTarget'}
        $probe=Send @{cmd='crew_pose_probe'}
        $remaining=Send @{cmd='eval';code='magazinesArray ffvPassenger'}
        if ($seated.result -ne 'true') {throw 'NPC no longer occupies the real cargo seat'}
        if ($NpcScenario -eq 'clear') {
            if ([double]$after.result -ge 30 -or [double]$damage.result -le 0 -or [int]$cargoShots.result -le 0) {
                throw "NPC passenger failed to hit with real $Rifle ammunition"
            }
        } elseif ($after.result -ne '30' -or $damage.result -ne '0' -or $cargoShots.result -ne '0') {
            throw "Unsafe NPC shot in negative case: $NpcScenario"
        }
        [pscustomobject]@{kind='cargo-integration';player=[bool]$PlayerFire;moving=[bool]$NpcMoving;movingShots=$movingShots.result;scenario=$NpcScenario;heading=$NpcHeading;driver=[bool]$NpcDriver;installed=$stamp.Trim();ammo=$after.result;magazines=$remaining.result;shots=$cargoShots.result;targetDamage=$damage.result;probe=$probe} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'npc-observation.json')
        if ($SaveLoad) {
            $null=Send @{cmd='exec';code='ffvPassenger setCombatMode "BLUE"'}
            $savedDamage=Send @{cmd='eval';code='getDammage ffvPassenger'}
            $savedMagazines=Send @{cmd='eval';code='magazinesArray ffvPassenger'}
            $null=Send @{cmd='eval';code='triSaveGame "jeep-seated"'} '"OK"'
            $null=Send @{cmd='exec';code='ffvPassenger setDammage 0.4'}
            $null=Send @{cmd='eval';code='triLoadGame "jeep-seated"'} '"OK"'
            Start-Sleep -Seconds 2
            $null=Send @{cmd='eval';code='getDammage ffvPassenger'} $savedDamage.result
            $null=Send @{cmd='eval';code='magazinesArray ffvPassenger'} $savedMagazines.result
            $restored=Send @{cmd='crew_seat_status'}
            if (!$restored.active -or !$restored.inCargo) {throw 'Seated save/load lost active passenger'}
            if ($PlayerFire) {$null=Send @{cmd='eval';code='player == ffvPassenger'} 'true'}
            ResumeReviewCamera
        }
        if ($NpcTransitions -or $PlayerFire) {
            $null=Send @{cmd='exec';code='ffvPassenger setCombatMode "BLUE"'}
            if (!$PlayerFire) {
                $null=Send @{cmd='exec';code='ffvView camSetPos [9701.8,3599.6,1.6]; ffvView camSetTarget [9700.4,3600,1.35]; ffvView camCommit 0'}
            }
            Start-Sleep -Seconds 1
            $reloadBefore=Send @{cmd='eval';code='magazinesArray ffvPassenger'}
            $shotsBeforeReload=Send @{cmd='eval';code='ffvShots'}
            if ($PlayerFire) {
                $null=Send @{cmd='key';sc=21;hold=$true}
                $inputDeadline=[DateTime]::UtcNow.AddSeconds(2)
                do {
                    Start-Sleep -Milliseconds 50
                    $inputState=Send @{cmd='crew_pose_probe'}
                } while (!$inputState.reloadInProgress -and [DateTime]::UtcNow -lt $inputDeadline)
                $null=Send @{cmd='key_up';sc=21}
                if (!$inputState.reloadInProgress -and $inputState.reloadInput -le 0) {throw 'Reload key was not observed by the gameplay input path'}
            } else {$null=Send @{cmd='crew_request_reload'}}
            $samples=@(); $seenReload=$false; $reloadSaved=$false; $clock=[Diagnostics.Stopwatch]::StartNew()
            do {
                $motion=Send @{cmd='crew_pose_probe'}
                $inReload = $motion.primaryMove -imatch '^OP(Jeep|Heli)Reload$'
                $seenReload=$seenReload -or $inReload
                if ($inReload -and !$motion.weaponsDisabled) {throw 'Reload did not disable firing'}
                if ($SaveLoad -and !$reloadSaved -and $inReload -and $motion.phase -gt .2) {
                    $clock.Stop()
                    $null=Send @{cmd='eval';code='triSaveGame "jeep-reloading"'} '"OK"'
                    $null=Send @{cmd='eval';code='triLoadGame "jeep-reloading"'} '"OK"'
                    ResumeReviewCamera
                    $clock.Start()
                    $reloadSaved=$true
                }
                $name='motion-{0:D4}.png' -f $samples.Count
                $beforeCapture=$clock.Elapsed.TotalSeconds
                $null=Send @{cmd='screenshot';path=(Join-Path $output $name)}
                $afterCapture=$clock.Elapsed.TotalSeconds
                $samples += [pscustomobject]@{file=$name;pose=$motion.primaryMove;phase=$motion.phase;time=($beforeCapture+$afterCapture)*0.5;captureSeconds=$afterCapture-$beforeCapture}
                Start-Sleep -Milliseconds 80
            } while ($clock.Elapsed.TotalSeconds -lt 8)
            $samples | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'motion-samples.json')
            $reloadAfter=Send @{cmd='eval';code='magazinesArray ffvPassenger'}
            $null=Send @{cmd='eval';code=('ffvPassenger ammo "'+$Rifle+'"')} '30'
            $null=Send @{cmd='eval';code='ffvShots'} $shotsBeforeReload.result
            $null=Send @{cmd='eval';code='vehicle ffvPassenger == ffvJeep'} 'true'
            $reloadEnd=Send @{cmd='crew_pose_probe'}
            if (!$seenReload -or ($SaveLoad -and !$reloadSaved) -or $reloadEnd.reloadInProgress -or !$reloadEnd.cargoWeaponActive -or !$reloadEnd.loaded -or
                (RifleRounds $reloadBefore) -ne (RifleRounds $reloadAfter)) {throw 'Real cargo reload/animation/magazine conservation failed'}
        }
        if ($Lifecycle) {
            $null=Send @{cmd='exec';code='ffvPassenger setCombatMode "BLUE"; unassignVehicle ffvPassenger; ffvPassenger action ["EJECT",ffvJeep]'}
            Start-Sleep -Seconds 2
            $null=Send @{cmd='eval';code='vehicle ffvPassenger == ffvPassenger'} 'true'
            $exitState=Send @{cmd='crew_seat_status'}
            if (!$exitState.present -or $exitState.active -or $exitState.inCargo) {throw 'Cargo mode survived exit'}
            $null=Send @{cmd='exec';code='ffvPassenger assignAsCargo ffvJeep; ffvPassenger moveInCargo ffvJeep'}
            Start-Sleep -Seconds 2
            $reentry=Send @{cmd='crew_seat_status'}
            if ($reentry.active -eq [bool]$PlayerFire -or !$reentry.inCargo) {throw 'Reentry did not restore the correct player/NPC mode default'}
            $null=Send @{cmd='exec';code='unassignVehicle ffvPassenger; ffvPassenger action ["EJECT",ffvJeep]'}
            Start-Sleep -Seconds 1
            $null=Send @{cmd='exec';code='"SoldierWB" createUnit [[9690,3590,0],group player,"ffvSeatBlocker=this; removeAllWeapons this; this disableAI ""MOVE"""]; ffvSeatBlocker moveInCargo ffvJeep; ffvPassenger assignAsCargo ffvJeep; ffvPassenger moveInCargo ffvJeep'}
            Start-Sleep -Seconds 2
            $ordinary=Send @{cmd='crew_pose_probe'}
            if ($ordinary.cargoWeaponActive -or $ordinary.cargoIndex -eq 0) {throw 'Unsupported cargo seat entered weapon mode'}
            $null=Send @{cmd='exec';code='unassignVehicle ffvPassenger; ffvPassenger action ["EJECT",ffvJeep]; unassignVehicle ffvSeatBlocker; ffvSeatBlocker action ["EJECT",ffvJeep]'}
            Start-Sleep -Seconds 1
            $null=Send @{cmd='exec';code='deleteVehicle ffvSeatBlocker; ffvPassenger assignAsCargo ffvJeep; ffvPassenger moveInCargo ffvJeep'}
            Start-Sleep -Seconds 2
            $reentry=Send @{cmd='crew_seat_status'}
            if ($reentry.active -eq [bool]$PlayerFire) {throw 'Return from ordinary seat kept a stale weapon mode'}
            if ($PlayerFire) {
                $null=Send @{cmd='exec';code='ffvPassenger action ["CARGO WEAPON READY",ffvJeep]'}
                Start-Sleep -Seconds 2
                $reentry=Send @{cmd='crew_seat_status'}
                if (!$reentry.active) {throw 'Explicit reactivation after seat change failed'}
            }
            $null=Send @{cmd='exec';code='ffvJeep setDammage 1'}
            Start-Sleep -Seconds 1
            $destroyed=Send @{cmd='crew_seat_status'}
            if ($destroyed.active) {throw 'Destroyed carrier retained cargo weapon mode'}
        }
        if ($CarrierDamage) {
            $damageBefore=[double](Send @{cmd='eval';code='getDammage ffvJeep'}).result
            $null=Send @{cmd='crew_cursor';x=0.1;y=-1;z=0}
            Start-Sleep -Seconds 2
            foreach ($shot in 1..6) {
                $null=Send @{cmd='mouse_button';button=1;down=$true}
                Start-Sleep -Milliseconds 150
                $null=Send @{cmd='mouse_button';button=1;down=$false}
                Start-Sleep -Milliseconds 250
            }
            $damageAfter=[double](Send @{cmd='eval';code='getDammage ffvJeep'}).result
            [pscustomobject]@{before=$damageBefore;after=$damageAfter;vehicle=$TestVehicle} |
                ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'carrier-damage.json')
            if ($damageAfter -le $damageBefore) {throw 'Personal bullets did not damage the carrier floor'}
        }
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'NPC fire exit failed'}
        if (Select-String -LiteralPath $log -Pattern 'Bad animation|Broken animation|Animation .*not found|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'NPC integration load/runtime error'}
        if ($NpcTransitions -or $PlayerFire -or $NpcMoving) {
            & python "$PSScriptRoot/default-content/make_jeep_pose_video.py" $output
            if ($LASTEXITCODE -ne 0) {throw 'Reload video encoding failed'}
        }
        Write-Host "Cargo $NpcScenario passed (player=$PlayerFire, moving=$NpcMoving, reload=$($NpcTransitions -or $PlayerFire), lifecycle=$Lifecycle, save=$SaveLoad, default=$DefaultPackage); NOT full visual/movement acceptance: $output"
        return
    }
    $null=Send @{cmd='exec';code='ffvPassenger setCombatMode "BLUE"; unassignVehicle ffvPassenger; ffvPassenger action ["EJECT",ffvJeep]'}
    Start-Sleep -Seconds 3
    $null=Send @{cmd='eval';code='vehicle ffvPassenger == ffvPassenger'} 'true'
    $null=Send @{cmd='exec';code='ffvPassenger setPos [9704,3600,0]; ffvPassenger setDir 90; ffvPassenger setUnitPos "UP"; ffvPassenger setBehaviour "COMBAT"; ffvPassenger setCombatMode "YELLOW"; ffvPassenger doTarget ffvTarget; ffvPassenger doFire ffvTarget'}
    Start-Sleep -Seconds 12
    $footAmmo=Send @{cmd='eval';code=('ffvPassenger ammo "'+$Rifle+'"')}
    $footDamage=Send @{cmd='eval';code='getDammage ffvTarget'}
    $footShots=Send @{cmd='eval';code='ffvShots'}
    $magazinesAfter=Send @{cmd='eval';code='magazinesArray ffvPassenger'}
    $roundsBefore=RifleRounds $magazinesBefore
    $roundsAfter=RifleRounds $magazinesAfter
    if ([int]$footShots.result -le [int]$cargoShots.result -or [double]::Parse($footDamage.result,[Globalization.CultureInfo]::InvariantCulture) -le 0) {throw 'On-foot firing control failed'}
    if ($roundsBefore -le 0 -or $roundsAfter -ge $roundsBefore) {throw 'No verified rifle ammunition consumption'}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'on-foot-control.png')}
    if ($ProjectileFrame) {
        $null=Send @{cmd='exec';code='deleteVehicle ffvTarget; ffvPassenger setCombatMode "BLUE"; ffvPassenger setBehaviour "SAFE"; ffvPassenger assignAsCargo ffvJeep; ffvPassenger moveInCargo ffvJeep; ffvJeep setPos [9740,3670,0]'}
        Start-Sleep -Seconds 1
        foreach ($heading in @(0,90,180)) {
            $null=Send @{cmd='exec';code=("ffvJeep setDir $heading; ffvJeep setVelocity [8,0,3]")}
            $probe=Send @{cmd='crew_projectile_probe'}
            if ($null -eq $probe.weaponDirectionError -or $probe.weaponDirectionError -gt 0.001 -or
                $probe.positionError -gt 0.001 -or $probe.velocityError -gt 0.001 -or $probe.directionDot -lt 0.99 -or
                $probe.worldX -lt 9700 -or $probe.worldZ -lt 3650 -or $probe.inheritedSpeed -lt 1) {
                throw 'Attached projectile frame/translation inheritance mismatch'
            }
        }
        $null=Send @{cmd='eval';code='vehicle ffvPassenger == ffvJeep'} 'true'
    }
    # Observation only: identical ammunition is expected on the unmodified path,
    # not success criteria for future FFV. Never label this a firing acceptance.
    [pscustomobject]@{kind='baseline-observation';installed=$stamp.Trim();ammoBefore=$before.result;ammoAfter=$after.result;stillCargo=$seated.result;targetDamage=$damage.result;cargoShots=$cargoShots.result;onFootAmmo=$footAmmo.result;onFootDamage=$footDamage.result;totalShots=$footShots.result;rifleRoundsBefore=$roundsBefore;rifleRoundsAfter=$roundsAfter} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'observation.json')
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'Game did not exit cleanly'}
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Renderer error'}
    Write-Host "Jeep baseline observation: $output"
} finally {
    if ($client) {$client.Dispose()}
    if (!$p.HasExited) {Stop-Process -Id $p.Id -Force}
    $p.Dispose()
}
