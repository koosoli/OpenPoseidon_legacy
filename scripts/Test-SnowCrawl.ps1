param([switch]$ResetSettings, [switch]$Storm, [switch]$Refill,
      [ValidateRange(0.01,1.0)][double]$Depth=0.18, [switch]$ProneView, [switch]$Walking, [switch]$Rotor, [switch]$Vehicle, [switch]$PlayerProne,
      [ValidateSet('UAZ','M113')][string]$VehicleClass='UAZ',
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if ($Walking -and $ProneView) {throw 'Walking and ProneView are different fixtures'}
if ($Vehicle -and ($Rotor -or $Walking -or $ProneView)) {throw 'Vehicle is a separate fixture'}
if ($PlayerProne -and ($Vehicle -or $Rotor -or $Walking -or $ProneView)) {throw 'PlayerProne is a separate fixture'}
$posture=if ($Walking) {'UP'} else {'DOWN'}
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw
$output=Join-Path $root ('build/snow-crawl/acceptance/crawl-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
$stamp | Set-Content -LiteralPath (Join-Path $output 'DEPLOYED-FROM.txt')
[pscustomobject]@{installed=$stamp.Trim();depth=$Depth;vehicleClass=$VehicleClass;playerProne=[bool]$PlayerProne;vehicle=[bool]$Vehicle;rotor=[bool]$Rotor;walking=[bool]$Walking;proneView=[bool]$ProneView;refill=[bool]$Refill;storm=[bool]$Storm;resetSettings=[bool]$ResetSettings} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'test-context.json')
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $env:POSEIDON_USER_DIR -DlssMode 0
$env:POSEIDON_SNOW_TEST_DEPTH=$Depth.ToString([Globalization.CultureInfo]::InvariantCulture)
Remove-Item Env:POSEIDON_SNOW_TEST_RATE -ErrorAction SilentlyContinue
$env:WGR_AUTO_EXPOSURE='0'
$env:WGR_EXPOSURE=if ($Storm) {'1.0'} else {'0.15'}
if ($ResetSettings) { $env:WGR_SETTINGS_QUEUE_TRACE='1' }
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$mission=Join-Path $root 'dev-missions/snow-crawl.Intro'
$log=Join-Path $output 'crawl.log'
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
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
    function Send($command) {
        $writer.WriteLine(($command | ConvertTo-Json -Compress))
        do {
            $line=$reader.ReadLine()
            if ($null -eq $line) {throw 'Harness closed'}
            $r=$line | ConvertFrom-Json
        } while ($null -eq $r.ok)
        $line | Add-Content -LiteralPath (Join-Path $output 'responses.jsonl')
        if (!$r.ok) {throw $line}
        Write-Host $line
        return $r
    }
    Start-Sleep -Seconds 12
    if ($Depth -gt 0.5) {
        $null=Send @{cmd='dev_snow';action='depth-limit';metres=$Depth}
        $null=Send @{cmd='dev_snow';action='deposit';metres=$Depth}
    }
    $cover=Send @{cmd='dev_snow';action='state'}
    if ([Math]::Abs($cover.depth-$Depth) -gt 0.00001) {throw 'Actual snow depth differs from requested fixture'}
    if ($PlayerProne) {
        $null=Send @{cmd='exec';code='player setPos [9700,3630,0]; player setDir 0; player switchCamera "INTERNAL"'}
        # Down/up in one event batch is not a held action; two presses toggle back up.
        $null=Send @{cmd='key';sc=29;hold=$true}
        Start-Sleep -Milliseconds 150
        $null=Send @{cmd='key_up';sc=29}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'player-lower-1.png')}
        $null=Send @{cmd='exec';code='player switchCamera "EXTERNAL"'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'player-posture.png')}
        $null=Send @{cmd='exec';code='player switchCamera "INTERNAL"'}
        foreach ($step in 1..4) {
            $null=Send @{cmd='mouse_motion';dx=0;dy=100}
            Start-Sleep -Seconds 1
            $null=Send @{cmd='screenshot';path=(Join-Path $output "player-lookdown-$step.png")}
        }
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Player view test exit failed'}
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Player view renderer error'}
        Write-Host "Player view sequence captured; posture and look direction need visual verification: $output"
        return
    }
    if ($Vehicle) {
        if ($Depth -lt 0.8) {throw 'Vehicle hull fixture requires deep snow'}
        $null=Send @{cmd='exec';code=('doStop crawlWalker; snowCar="{0}" createVehicle [9700,3630,0]; snowCar setDir 0; crawlWalker moveInDriver snowCar; doStop crawlWalker; crawlView="camera" camCreate [9704,3626,2.5]; crawlView cameraEffect ["internal","back"]; crawlView camSetTarget [9700,3630,0]; crawlView camCommit 0' -f $VehicleClass)}
        Start-Sleep -Seconds 5
        $initialPosition=Send @{cmd='eval';code='getPos snowCar'}
        $initialCoords=($initialPosition.result.Trim('[',']') -split ',') | ForEach-Object { [double]::Parse($_,[Globalization.CultureInfo]::InvariantCulture) }
        if ($initialCoords.Count -ne 3) {throw 'Missing initial vehicle position'}
        $body=Send @{cmd='dev_snow';action='sample';x=$initialCoords[0];z=$initialCoords[1]}
        if ($body.deficit -lt 0.4 -or $body.deficit -gt 0.8) {throw 'No bounded underbody channel between wheel tracks'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'vehicle-stationary.png')}
        $null=Send @{cmd='exec';code='crawlWalker setBehaviour "CARELESS"; crawlWalker doMove [9700,3670,0]'}
        Start-Sleep -Seconds 12
        $position=Send @{cmd='eval';code='getPos snowCar'}
        $coords=($position.result.Trim('[',']') -split ',') | ForEach-Object { [double]::Parse($_,[Globalization.CultureInfo]::InvariantCulture) }
        if ($coords.Count -ne 3 -or $coords[1] -lt 3635) {throw 'Vehicle did not drive along the test path'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'vehicle-driving.png')}
        $null=Send @{cmd='exec';code='crawlWalker action ["EJECT",snowCar]'}
        Start-Sleep -Seconds 1
        $null=Send @{cmd='exec';code='crawlWalker setPos [9660,3600,0]; deleteVehicle snowCar'}
        $cx=($coords[0]+4).ToString([Globalization.CultureInfo]::InvariantCulture)
        $cz=($coords[1]-4).ToString([Globalization.CultureInfo]::InvariantCulture)
        $tx=$coords[0].ToString([Globalization.CultureInfo]::InvariantCulture)
        $tz=$coords[1].ToString([Globalization.CultureInfo]::InvariantCulture)
        $null=Send @{cmd='exec';code="crawlView camSetPos [$cx,$cz,2.5]; crawlView camSetTarget [$tx,$tz,0]; crawlView camCommit 0"}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'vehicle-trail.png')}
        $outside=Send @{cmd='dev_snow';action='sample';x=($initialCoords[0]+5);z=$initialCoords[1]}
        if ($outside.deficit -ne 0) {throw 'Vehicle cleared snow outside its footprint'}
        $null=Send @{cmd='dev_snow';action='deposit';metres=1}
        Start-Sleep -Seconds 2
        $filled=Send @{cmd='dev_snow';action='sample';x=$initialCoords[0];z=$initialCoords[1]}
        if ($filled.deficit -ne 0) {throw 'Snowfall failed to refill vehicle depression'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'vehicle-refilled.png')}
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Vehicle test exit failed'}
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Vehicle renderer error'}
        Write-Host "Vehicle contact passed; inspect captures in $output"
        return
    }
    if ($Rotor) {
        $null=Send @{cmd='exec';code='snowHeli="UH60" createVehicle [9700,3630,0]; snowHeli engineOn false; crawlView="camera" camCreate [9720,3605,15]; crawlView cameraEffect ["internal","back"]; crawlView camSetTarget [9700,3630,0]; crawlView camCommit 0'}
        Start-Sleep -Seconds 5
        $initial=Send @{cmd='dev_snow';action='sample';x=9700;z=3630}
        if ($initial.deficit -ne 0) {throw 'Stopped rotor changed snow'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'rotor-before.png')}
        $null=Send @{cmd='exec';code='player moveInDriver snowHeli; snowHeli engineOn true'}
        Start-Sleep -Seconds 30
        $null=Send @{cmd='eval';code='[getPos snowHeli,isEngineOn snowHeli]'}
        if ((Send @{cmd='eval';code='isEngineOn snowHeli'}).result -ne 'true') {throw 'Rotor fixture engine stopped; not a valid erosion test'}
        $eroded=Send @{cmd='dev_snow';action='sample';x=9700;z=3630}
        if ($eroded.deficit -le 0.02) {throw 'Running rotor did not erode snow'}
        $outside=Send @{cmd='dev_snow';action='sample';x=9720;z=3630}
        if ($outside.deficit -ne 0) {throw 'Rotor erosion escaped its bounded footprint'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'rotor-after.png')}
        $null=Send @{cmd='exec';code='player action ["EJECT",snowHeli]'}
        Start-Sleep -Seconds 1
        $null=Send @{cmd='exec';code='player setPos [9680,3590,0]; deleteVehicle snowHeli'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'rotor-ground.png')}
        $null=Send @{cmd='dev_snow';action='deposit';metres=1}
        Start-Sleep -Seconds 2
        $filled=Send @{cmd='dev_snow';action='sample';x=9700;z=3630}
        if ($filled.deficit -ne 0) {throw 'Snowfall failed to refill rotor depression'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'rotor-refilled.png')}
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Rotor test exit failed'}
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Rotor renderer error'}
        Write-Host "Rotor contact passed; inspect captures in $output"
        return
    }
    $null=Send @{cmd='exec';code='crawlWalker setPos [9700,3600,0]; doStop crawlWalker; crawlView="camera" camCreate [9703,3598,3]; crawlView cameraEffect ["internal","back"]; crawlView camSetTarget [9700,3604,0]; crawlView camCommit 0'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'before.png')}
    $null=Send @{cmd='exec';code=('crawlWalker setUnitPos "'+$posture+'"; crawlWalker setBehaviour "COMBAT"; crawlWalker doMove [9700,3606,0]')}
    Start-Sleep -Seconds 10
    $position=Send @{cmd='eval';code='getPos crawlWalker'}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'crawling.png')}
    $coords=($position.result.Trim('[',']') -split ',') | ForEach-Object { [double]::Parse($_,[Globalization.CultureInfo]::InvariantCulture) }
    if ($coords.Count -ne 3 -or $coords[1] -lt 3601) {throw 'Actor did not crawl along the test path'}
    if ($ProneView) {
        $null=Send @{cmd='exec';code='doStop crawlWalker; crawlView cameraEffect ["terminate","back"]; crawlWalker switchCamera "INTERNAL"'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'prone-internal.png')}
        $null=Send @{cmd='exec';code='crawlWalker switchCamera "EXTERNAL"'}
        Start-Sleep -Seconds 1
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'prone-external.png')}
        $null=Send @{cmd='exec';code='player switchCamera "INTERNAL"; crawlView cameraEffect ["internal","back"]'}
    }
    $null=Send @{cmd='exec';code='doStop crawlWalker; crawlWalker setPos [9660,3600,0]'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'trail.png')}
    if ($Refill) {
        $null=Send @{cmd='exec';code='crawlWalker setUnitPos "UP"'}
        Start-Sleep -Seconds 2
        $beforeFill=Send @{cmd='dev_snow';action='state'}
        if ($beforeFill.chunks -lt 2) {throw 'Not enough real track chunks for refill test'}
        $null=Send @{cmd='dev_snow';action='deposit';metres=1}
        Start-Sleep -Seconds 2
        $afterFill=Send @{cmd='dev_snow';action='state'}
        if ($afterFill.chunks -ge $beforeFill.chunks) {throw 'Filled tracks were not reclaimed'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'refilled.png')}
        $null=Send @{cmd='exec';code=('crawlWalker setPos [9700,3600,0]; crawlWalker setUnitPos "'+$posture+'"; crawlWalker doMove [9700,3606,0]')}
        Start-Sleep -Seconds 10
        $afterCrawl=Send @{cmd='dev_snow';action='state'}
        if ($afterCrawl.chunks -le $afterFill.chunks) {throw 'New crawl failed to create tracks after refill'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'refilled-new-trail.png')}
    }
    if ($ResetSettings) {
        $null=Send @{cmd='dev_reset_probe'}
        Start-Sleep -Seconds 2
        $before=Send @{cmd='dev_snow';action='state'}
        if (!$before.enabled -or $before.chunks -le 0 -or $before.depth -le 0) {throw 'No stored snow trail to reset'}
        $reset=Send @{cmd='dev_snow';action='reset-panel'}
        if ($reset.enabled -or !$reset.falling -or !$reset.geometry -or
            [Math]::Abs($reset.rate-0.01) -gt 0.00001 -or $reset.maxDepth -ne 0.5 -or $reset.flakes -ne 1 -or
            $reset.depth -ne $before.depth -or $reset.chunks -ne $before.chunks) {throw 'Master reset changed surface state or missed a snow setting'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'reset-disabled.png')}
        $again=Send @{cmd='dev_snow';action='enable'}
        if ($again.depth -ne $before.depth -or $again.chunks -ne $before.chunks) {throw 'Disabled interval erased snow'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'reset-restored.png')}
    }
    if ($Storm) {
        $null=Send @{cmd='dev_snow';action='reset-settings'}
        $null=Send @{cmd='dev_snow';action='enable'}
        Start-Sleep -Seconds 3
        $normal=Send @{cmd='dev_snow';action='state'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'snow-normal.png')}
        $null=Send @{cmd='dev_snow';action='storm'}
        Start-Sleep -Seconds 3
        $stormState=Send @{cmd='dev_snow';action='state'}
        if (!$stormState.enabled -or !$stormState.falling -or $stormState.flakes -ne 8 -or
            $stormState.liveFlakes -lt 1000 -or $stormState.drawnFlakes -le $normal.drawnFlakes -or
            $stormState.depth -le $normal.depth) {throw 'Snowstorm did not increase rendered precipitation and cover'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'snow-storm.png')}
    }
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Snow renderer error'}
    if ($ResetSettings) {
        foreach ($pattern in @('Settings queue applied: sky cirrus=false rays=false intensity=1 fogClose=0.4 softness=0.9',
                              'Settings queue applied: sky cirrus=true rays=true intensity=4 fogClose=0.85 softness=0.45',
                              'Settings queue applied: road lift=0.03',
                              'Settings queue applied: road lift=0.02')) {
            if (!(Select-String -LiteralPath $log -SimpleMatch $pattern -Quiet)) {throw "Renderer did not consume settings: $pattern"}
        }
    }
    Write-Host 'Inspect crawling.png and trail.png; position alone does not prove visible snow displacement.'
} finally {
    if ($client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill()}
}
