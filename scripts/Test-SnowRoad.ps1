param([ValidateRange(0,1)][double]$Depth=0.3,
      [switch]$LowLift,
      [string]$Label='baseline',
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root "build/snow-road/$Label"
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$env:POSEIDON_SNOW_TEST_DEPTH=$Depth.ToString([Globalization.CultureInfo]::InvariantCulture)
$env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE='0.15'
if ($LowLift) {
    $env:WGR_ROAD_PIXEL_LIFT_PER_M='0.00001'
    $env:WGR_ROAD_PIXEL_LIFT='0.001'
}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$mission=Join-Path $root 'dev-missions/snow-road.eden'
$log=Join-Path $output 'game.log'
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
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Renderer error; reject this run'}
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
    if (Select-String -LiteralPath $log -Pattern 'StartAutoTest could not boot' -Quiet) {throw 'Mission did not load'}
    foreach ($pose in @(@('near','5083.67 3988.25 17.63 145.4 -18.5'),
                        @('far','5020 4080 55 145.4 -18.5'),
                        @('return','5083.67 3988.25 17.63 145.4 -18.5'))) {
        if ((Send @{cmd='eval';code="triFreeFlyPose `"$($pose[1])`""}).result.Trim('"') -ne 'OK') {throw 'Zeus entry failed'}
        Start-Sleep -Seconds 5
        if ((Send @{cmd='eval';code='alive player'}).result -ne 'true') {throw 'Player died; camera comparison invalid'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output "$($pose[0]).png")}
    }
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
    Write-Host 'Inspect matched road coverage; LowLift is a diagnostic, not a shipping fix.'
} finally {
    if ($client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill()}
}
