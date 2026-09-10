param([switch]$ResetSettings, [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/snow-crawl/acceptance'
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=-1;`n")
$env:POSEIDON_SNOW_TEST_DEPTH='0.18'
Remove-Item Env:POSEIDON_SNOW_TEST_RATE -ErrorAction SilentlyContinue
$env:WGR_AUTO_EXPOSURE='0'
$env:WGR_EXPOSURE='0.15'
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
        if (!$r.ok) {throw $line}
        Write-Host $line
        return $r
    }
    Start-Sleep -Seconds 12
    $null=Send @{cmd='exec';code='crawlWalker setPos [9700,3600,0]; doStop crawlWalker; crawlView="camera" camCreate [9703,3598,3]; crawlView cameraEffect ["internal","back"]; crawlView camSetTarget [9700,3604,0]; crawlView camCommit 0'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'before.png')}
    $null=Send @{cmd='exec';code='crawlWalker setUnitPos "DOWN"; crawlWalker setBehaviour "COMBAT"; crawlWalker doMove [9700,3606,0]'}
    Start-Sleep -Seconds 10
    $position=Send @{cmd='eval';code='getPos crawlWalker'}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'crawling.png')}
    $coords=($position.result.Trim('[',']') -split ',') | ForEach-Object { [double]::Parse($_,[Globalization.CultureInfo]::InvariantCulture) }
    if ($coords.Count -ne 3 -or $coords[1] -lt 3601) {throw 'Actor did not crawl along the test path'}
    $null=Send @{cmd='exec';code='doStop crawlWalker; crawlWalker setPos [9660,3600,0]'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'trail.png')}
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
