param([switch]$LightsOff, [switch]$LocalReachControl, [switch]$LocalReachMatched,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($LocalReachControl -and $LocalReachMatched) {throw 'Select one reach comparison arm'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/freefly-lights/acceptance'
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=-1;`nvsync=0;`nfpsCap=60;`n")
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'game.cfg'), "preferredViewDistance=50000;`n")
$env:WGR_AUTO_EXPOSURE='0'
$env:WGR_EXPOSURE='2'
$env:WGR_STATIC_LIGHT_TRACE='1'
$env:POSEIDON_BULB_GLOW_DIAG='1'
$name=if ($LightsOff) {'truck-off'} else {'truck-on'}
if ($LocalReachControl -or $LocalReachMatched) {
    $env:WGR_LOCAL_REACH_TRACE='1'
    $env:WGR_MATCH_LOCAL_SHADOW_REACH=if ($LocalReachMatched) {'1'} else {'0'}
    $prefix=if ($LocalReachMatched) {'reach-matched-'} else {'reach-control-'}
    $name=$prefix+$name
}
$log=Join-Path $output "$name.log"
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$mission=Join-Path $root 'dev-missions/freefly-lights.eden'
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
$null=$p.Handle
$client=$null
try {
    $client=[Net.Sockets.TcpClient]::new(); $deadline=[DateTime]::UtcNow.AddSeconds(120)
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
        if (!$r.ok) {throw $line}
        Write-Host $line
        return $r
    }
    Start-Sleep -Seconds 15
    if ((Select-String -LiteralPath $log -Pattern 'StartAutoTest could not boot' -Quiet) -or
        (Send @{cmd='eval';code='typeOf lightTruck'}).result.Trim('"') -ne 'Truck5t') {
        throw 'Required truck mission did not load'
    }
    $action=if ($LightsOff) {'LIGHT OFF'} else {'LIGHT ON'}
    # AI drivers reapply night lights every tick unless in cautious/danger mode.
    $behaviour=if ($LightsOff) {'COMBAT'} else {'SAFE'}
    $null=Send @{cmd='exec';code="driver lightTruck setBehaviour `"$behaviour`""}
    $null=Send @{cmd='exec';code="lightTruck action [`"$action`",lightTruck]"}
    Start-Sleep -Seconds 3
    $null=Send @{cmd='screenshot';path=(Join-Path $output "$name-player.png")}
    foreach ($pose in @(@('near',3950,65,-25),@('far',3050,500,-25),@('distant',1000,1500,-26),@('return',3950,65,-25))) {
        $coords="5092 $($pose[1]) $($pose[2]) 0 $($pose[3])"
        $r=Send @{cmd='eval';code="triFreeFlyPose `"$coords`""}
        if ($r.result.Trim('"') -ne 'OK') {throw 'Zeus freefly entry refused'}
        Start-Sleep -Seconds 5
        $null=Send @{cmd='screenshot';path=(Join-Path $output "$name-$($pose[0]).png")}
        if ((Send @{cmd='eval';code='alive player'}).result -ne 'true') {throw 'Player must remain alive'}
    }
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Light renderer error'}
    if (($LocalReachControl -or $LocalReachMatched) -and
        !(Select-String -LiteralPath $log -SimpleMatch 'Local reach: slot=' -Quiet)) {throw 'No shadow light exercised'}
    $selection=(Select-String -LiteralPath $log -Pattern 'local-light selection' | Select-Object -Last 1).Line
    $expected=if ($LightsOff) {40} else {42}
    if ($selection -notmatch "candidates=$expected selected=$expected ") {throw "Light control did not hold: $selection"}
    Write-Host "Inspect light pools and truck bulbs in $output; this capture does not itself prove the reported regression fixed."
} finally {
    if ($null -ne $client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill(); $null=$p.WaitForExit(10000)}
}
