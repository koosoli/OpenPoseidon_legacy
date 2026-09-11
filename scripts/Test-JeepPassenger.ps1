param([switch]$ProjectileFrame, [switch]$AnimationPreview, [switch]$AnimationVideo, [switch]$NpcFire,
      [ValidateSet('clear','hold','outside','blocked','friendly')][string]$NpcScenario='clear',
      [ValidateSet(0,90,180)][int]$NpcHeading=0, [switch]$NpcDriver,
      [switch]$NpcTransitions, [switch]$NpcMoving, [switch]$NpcTurning, [switch]$SecondJeep,
      [switch]$PlayerFire, [switch]$Lifecycle, [switch]$SaveLoad,
      [switch]$DefaultPackage, [string]$UserMod='',
      [ValidateSet('','missing','incompatible')][string]$AutomaticOriginal='', [string]$MissionPath='',
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($NpcTurning) {$NpcMoving=$true}
if ($PlayerFire) {
    if ($NpcFire -or $NpcMoving -or $NpcTransitions -or $NpcScenario -ne 'clear') {throw 'Player fire is a separate case'}
    $NpcFire=$true
}
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
    $packArchive=Join-Path $GameDir '@OP_VehicleActions/AddOns/op_jeep_actions.pbo'
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
[pscustomobject]@{installed=$stamp.Trim();args=$gameArgs;source=(& git -C $root rev-parse HEAD);pack=$packArchive;packSha256=$packHash} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'test-context.json')
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $gameArgs
$null=$p.Handle
$client=$null
function RifleRounds($response) {
    $items=ConvertFrom-Json -InputObject $response.result
    $rounds=0
    for ($i=0; $i -lt $items.Count; $i+=2) {
        if ($items[$i] -ceq 'M16') {$rounds += [int]$items[$i+1]}
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
    $null=Send @{cmd='eval';code='typeOf ffvJeep'} '"Jeep"'
    if ($NpcFire) {
        $angle=$NpcHeading * [math]::PI / 180
        $rightX=[int][math]::Round([math]::Cos($angle)); $rightZ=-[int][math]::Round([math]::Sin($angle))
        $frontX=-$rightZ; $frontZ=$rightX
        $null=Send @{cmd='exec';code=('ffvJeep setDir {0}; ffvTarget setPos [{1},{2},0]' -f $NpcHeading,(9700+60*$rightX),(3600+60*$rightZ))}
        # Place moving-test targets before reveal; moving a known target later
        # tests stale perception rather than the intended seat/steering case.
        if ($NpcTurning) {$null=Send @{cmd='exec';code='ffvTarget setPos [9780,3640,0]'}}
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
    $before=Send @{cmd='eval';code='ffvPassenger ammo "M16"'}
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
        if (!$entry.cargoWeaponActive) {throw 'Automatic cargo animation entry failed'}
        $null=Send @{cmd='exec';code='ffvPassenger reveal ffvTarget; ffvPassenger doTarget ffvTarget'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='eval';code='ffvShots'} '0'
        $activation=Send @{cmd='crew_pose_probe'}
        if (!$activation.cargoWeaponActive) {throw 'NPC fixture did not activate the explicitly configured seat'}
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
        $null=Send @{cmd='eval';code='ffvPassenger ammo "M16"'} '30'
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
        $null=Send @{cmd='crew_cursor';x=60;y=0.01;z=-0.07}
        Start-Sleep -Seconds 3
        $null=Send @{cmd='crew_pose_probe'}
        $null=Send @{cmd='eval';code='ffvShots'} '0'
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'player-before-fire.png')}
        for ($attempt=0; $attempt -lt 12; $attempt++) {
            $null=Send @{cmd='crew_cursor';x=60;y=0.01;z=-0.07}
            $null=Send @{cmd='mouse_button';button=1;down=$true}
            Start-Sleep -Milliseconds 180
            $null=Send @{cmd='mouse_button';button=1;down=$false}
            Start-Sleep -Milliseconds 180
            $hit=Send @{cmd='eval';code='getDammage ffvTarget'}
            if ([double]$hit.result -gt 0) {break}
        }
    }
    if ($NpcMoving) {
        $null=Send @{cmd='exec';code='ffvMovingShots=0; ffvPassenger addEventHandler ["Fired",{if (abs(speed ffvJeep)>5) then {ffvMovingShots=ffvMovingShots+1}}]; ffvJeep setFuel 1; ffvDriver setBehaviour "CARELESS"; ffvDriver setSpeedMode "LIMITED"; ffvDriver doMove [9700,3680,0]; ffvView camSetPos [9706,3615,3]; ffvView camSetTarget ffvJeep; ffvView camCommit 0'}
        if ($NpcTurning) {
            $null=Send @{cmd='exec';code='ffvTurnShots=0; ffvShotHeadings=[]; ffvPassenger addEventHandler ["Fired",{ffvShotHeadings=ffvShotHeadings+[getDir ffvJeep]; if ((abs(speed ffvJeep)>5) && (abs(sin(getDir ffvJeep))>0.087)) then {ffvTurnShots=ffvTurnShots+1}}]; ffvDriver doMove [9660,3680,0]'}
        }
        $driveDeadline=[DateTime]::UtcNow.AddSeconds(20)
        do {
            Start-Sleep -Milliseconds 250
            $driveSpeed=Send @{cmd='eval';code='abs(speed ffvJeep)'}
            $turnReady=$true
            if ($NpcTurning) {$turnReady=(Send @{cmd='eval';code='abs(sin(getDir ffvJeep))>0.087'}).result -ceq 'true'}
        } while (([double]$driveSpeed.result -le 5 -or !$turnReady) -and [DateTime]::UtcNow -lt $driveDeadline)
        if ([double]$driveSpeed.result -le 5 -or !$turnReady) {throw 'AI driver never reached the required motion/heading'}
        $null=Send @{cmd='eval';code='ffvShots'} '0'
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'moving-before-fire.png')}
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
    } else {Start-Sleep -Seconds 10}
    $after=Send @{cmd='eval';code='ffvPassenger ammo "M16"'}
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
                throw 'NPC passenger failed to hit with real M16 ammunition'
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
                Start-Sleep -Milliseconds 200
                $null=Send @{cmd='key_up';sc=21}
            } else {$null=Send @{cmd='crew_request_reload'}}
            $samples=@(); $seenReload=$false; $reloadSaved=$false; $clock=[Diagnostics.Stopwatch]::StartNew()
            do {
                $motion=Send @{cmd='crew_pose_probe'}
                $seenReload=$seenReload -or ($motion.primaryMove -ieq 'OPJeepReload')
                if ($motion.primaryMove -ieq 'OPJeepReload' -and !$motion.weaponsDisabled) {throw 'Reload did not disable firing'}
                if ($SaveLoad -and !$reloadSaved -and $motion.primaryMove -ieq 'OPJeepReload' -and $motion.phase -gt .2) {
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
            $null=Send @{cmd='eval';code='ffvPassenger ammo "M16"'} '30'
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
            if (!$reentry.active -or !$reentry.inCargo) {throw 'Supported reentry did not restore cargo mode'}
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
            if (!$reentry.active) {throw 'Return from ordinary seat failed'}
            $null=Send @{cmd='exec';code='ffvJeep setDammage 1'}
            Start-Sleep -Seconds 1
            $destroyed=Send @{cmd='crew_seat_status'}
            if ($destroyed.active) {throw 'Destroyed carrier retained cargo weapon mode'}
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
    $footAmmo=Send @{cmd='eval';code='ffvPassenger ammo "M16"'}
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
