param(
    [switch]$Large,
    [switch]$NoSatmap,
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/fusion/acceptance'
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),
    "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=-1;`n")
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'game.cfg'), "preferredViewDistance=50000;`n")
$prefix=if ($Large) {'size-probe'} else {'fusion'}
$env:WGR_OFP_SATMAP=if ($NoSatmap) {'0'} else {'1'}
if ($NoSatmap) { $prefix += '-no-satmap' }
$mission=if ($Large) {'Missions\SizeProbe.FusionSizeProbe'} else {'Missions\FusionTour.FusionOFP'}
$log=Join-Path $output "$prefix.log"
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
$null=$p.Handle
$client=$null
try {
    $client=[Net.Sockets.TcpClient]::new(); $deadline=[DateTime]::UtcNow.AddSeconds(180)
    while (!$client.Connected) {
        $p.Refresh()
        if ($p.WorkingSet64 -gt 7GB) {throw 'Working set exceeds 7 GB test safety limit'}
        try {$client.Connect('127.0.0.1',$port)} catch {
            if ($p.HasExited -or [DateTime]::UtcNow -gt $deadline) {throw}
            Start-Sleep -Milliseconds 500
        }
    }
    $stream=$client.GetStream(); $stream.ReadTimeout=90000
    $reader=[IO.StreamReader]::new($stream)
    $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush=$true
    function Send($command) {
        $p.Refresh()
        if ($p.WorkingSet64 -gt 7GB) {throw 'Working set exceeds 7 GB test safety limit'}
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
    $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-start.png")}
    $null=Send @{cmd='exec';code='hint ""; showCinemaBorder false; fusionCam="camera" camCreate [25600,13000,29000]; fusionCam cameraEffect ["internal","back"]; fusionCam camSetTarget [25600,22000,0]; fusionCam camCommit 0'}
    Start-Sleep -Seconds 4
    $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-overview.png")}
    $null=Send @{cmd='exec';code='fusionCam camSetPos [25600,22000,40000]; fusionCam camSetTarget [25600,22100,0]; fusionCam camCommit 0'}
    Start-Sleep -Seconds 4
    $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-high.png")}
    $sites=if ($Large) {@(@('south',10000,10000),@('centre',51200,51200),@('north',92000,92000))} else {@(@('everon',8260,11900),@('malden',25600,14400),@('kolgujev',41600,14400),@('nogova',18964,28408),@('desert',36700,27600))}
    foreach ($site in $sites) {
        $name=$site[0]; $x=$site[1]; $z=$site[2]
        $height=[double](Send @{cmd='eval';code="triTerrainHeight [$x,$z]"}).result
        if ($height -le 0) {throw "Expected above-water height at $name, got $height"}
        $null=Send @{cmd='exec';code="vehicle player setPos [$x,$z,1200]; vehicle player setVelocity [0,0,0]; fusionCam camSetPos [$x,$($z-500),1200]; fusionCam camSetTarget [$x,$($z+800),0]; fusionCam camCommit 0"}
        Start-Sleep -Seconds 3
        $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-$name.png")}
        $null=Send @{cmd='eval';code='getPos vehicle player'}
        $p.Refresh(); Write-Host "$name height=$height workingSetMB=$([int]($p.WorkingSet64/1MB))"
    }
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
    $text=Get-Content -LiteralPath $log -Raw
    if ($text -match 'heightfield upload failed|UNHANDLED EXCEPTION' -or $text -notmatch 'heightfield ready=true') {throw 'Terrain upload acceptance failed'}
    Write-Host "Runtime checks passed; visually inspect $output/$prefix-*.png."
} finally {
    if ($null -ne $client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill(); $null=$p.WaitForExit(10000)}
}
