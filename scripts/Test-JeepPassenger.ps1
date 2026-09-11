param([switch]$ProjectileFrame, [switch]$AnimationPreview, [switch]$AnimationVideo, [switch]$NpcFire,
      [ValidateSet('clear','hold','outside','blocked')][string]$NpcScenario='clear',
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($AnimationVideo -and !$AnimationPreview) {throw '-AnimationVideo requires -AnimationPreview'}
if ($NpcFire -and ($AnimationPreview -or $ProjectileFrame)) {throw 'Select one integration mode per launch'}
if (!$NpcFire -and $NpcScenario -ne 'clear') {throw '-NpcScenario requires -NpcFire'}
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
$mission=Join-Path $root 'dev-missions/jeep-ffv.Intro'
$log=Join-Path $output 'jeep.log'
$gameArgs=@('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
if ($AnimationPreview -or $NpcFire) {
    $preview=Join-Path $root 'build/jeep-ffv/animation-preview'
    if (!(Test-Path -LiteralPath "$preview/AddOns/op_jeep_animation_preview.pbo")) {throw 'Build animation preview first'}
    $gameArgs+=@('--mod',('"'+$preview+'"'))
}
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $gameArgs
$null=$p.Handle
$client=$null
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
    Start-Sleep -Seconds 12
    $null=Send @{cmd='eval';code='typeOf ffvJeep'} '"Jeep"'
    $null=Send @{cmd='exec';code='removeAllWeapons player; ffvShots=0; ffvPassenger addEventHandler ["Fired",{ffvShots=ffvShots+1}]; ffvPassenger setCombatMode "BLUE"; ffvPassenger assignAsCargo ffvJeep; ffvPassenger moveInCargo ffvJeep; ffvView="camera" camCreate [9704,3604,2.2]; ffvView cameraEffect ["internal","back"]; ffvView camSetTarget [9700,3600,1]; ffvView camCommit 0; showCinemaBorder false'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='eval';code='vehicle ffvPassenger == ffvJeep'} 'true'
    $null=Send @{cmd='eval';code='weapons ffvPassenger'}
    $before=Send @{cmd='eval';code='ffvPassenger ammo "M16"'}
    $magazinesBefore=Send @{cmd='eval';code='magazinesArray ffvPassenger'}
    $null=Send @{cmd='eval';code='alive ffvTarget && getDammage ffvTarget == 0'} 'true'
    if ($before.result -ne '30') {throw 'Passenger fired before baseline capture'}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'seated.png')}
    if ($NpcFire) {
        $null=Send @{cmd='eval';code='local ffvPassenger'}
        if ($NpcScenario -eq 'outside') {
            $null=Send @{cmd='exec';code='ffvTarget setPos [9700,3660,0]'}
        }
        if ($NpcScenario -eq 'blocked') {
            $null=Send @{cmd='exec';code='ffvBlock="Ural" createVehicle [9704,3600,0]; ffvBlock setDir 0'}
        }
        $null=Send @{cmd='exec';code='ffvPassenger switchMove "OPJeepAim"; ffvPassenger reveal ffvTarget; ffvPassenger doTarget ffvTarget'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='eval';code='ffvShots'} '0'
        $activation=Send @{cmd='crew_pose_probe'}
        if (!$activation.cargoWeaponActive) {throw 'NPC fixture did not activate the explicitly configured seat'}
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
    if (!$NpcFire -or $NpcScenario -ne 'hold') {
        $null=Send @{cmd='exec';code='ffvPassenger reveal ffvTarget; ffvPassenger doTarget ffvTarget; ffvPassenger setCombatMode "YELLOW"; ffvPassenger doFire ffvTarget'}
    }
    Start-Sleep -Seconds 10
    $after=Send @{cmd='eval';code='ffvPassenger ammo "M16"'}
    $seated=Send @{cmd='eval';code='vehicle ffvPassenger == ffvJeep'}
    $damage=Send @{cmd='eval';code='getDammage ffvTarget'}
    $cargoShots=Send @{cmd='eval';code='ffvShots'}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'ordered-fire.png')}
    if ($NpcFire) {
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
        [pscustomobject]@{kind='stationary-npc-integration';scenario=$NpcScenario;installed=$stamp.Trim();ammo=$after.result;magazines=$remaining.result;shots=$cargoShots.result;targetDamage=$damage.result;probe=$probe} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'npc-observation.json')
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(30000) -or $p.ExitCode -ne 0) {throw 'NPC fire exit failed'}
        if (Select-String -LiteralPath $log -Pattern 'Bad animation|Broken animation|Animation .*not found|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet) {throw 'NPC integration load/runtime error'}
        Write-Host "Stationary NPC $NpcScenario integration passed, NOT moving/player/reload acceptance: $output"
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
    function RifleRounds($response) {
        $items=ConvertFrom-Json -InputObject $response.result
        $rounds=0
        for ($i=0; $i -lt $items.Count; $i+=2) {
            if ($items[$i] -ceq 'M16') {$rounds += [int]$items[$i+1]}
        }
        return $rounds
    }
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
