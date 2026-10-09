param(
    [switch]$Large,
    [switch]$NoSatmap,
    [switch]$DumpSatmap,
    [string]$Pose='',
    [ValidateSet(-1,0,1)][int]$SatmapObjects=-1,
    [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    # an OpenPoseidon.exe outside the game folder (a branch build), run with the game folder as its working
    # directory; default: the installed one
    [string]$Exe=''
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root ('build/fusion/acceptance-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $env:POSEIDON_USER_DIR -DlssMode 0
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'game.cfg'), "preferredViewDistance=50000;`n")
$prefix=if ($Large) {'size-probe'} else {'fusion'}
$env:WGR_OFP_SATMAP=if ($NoSatmap) {'0'} else {'1'}
$env:WGR_OFP_SATMAP_OBJECTS=if ($SatmapObjects -lt 0) {$null} else {"$SatmapObjects"}
$env:WGR_OFP_SATMAP_DUMP=if ($DumpSatmap) {Join-Path $output 'satmap-albedo.png'} else {$null}
if ($SatmapObjects -ne 0) {$prefix += '-objects'}
if ($NoSatmap) { $prefix += '-no-satmap' }
if ($Pose) { $prefix += '-reported-high'; if (($Pose -split '\s+').Count -ne 5) {throw 'Pose requires x y z azimuth elevation'} }
Get-Content -LiteralPath "$GameDir/DEPLOYED-FROM.txt" | Set-Content -LiteralPath (Join-Path $output "$prefix.DEPLOYED-FROM.txt")
$mission=if ($Large) {'Mods\@OP_SizeProbe\Missions\OpenPoseidon\SizeProbe.FusionSizeProbe'} else {'Mods\@OP_Fusion\Missions\OpenPoseidon\FusionTour.FusionOFP'}
$log=Join-Path $output "$prefix.log"
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$exePath=if ($Exe) {$Exe} else {Join-Path $GameDir 'OpenPoseidon.exe'}
$p=Start-Process -FilePath $exePath -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
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
    if ($Pose) {
        $r=Send @{cmd='eval';code=('triFreeFlyPose "'+$Pose+'"')}
        if ($r.result.Trim('"') -ne 'OK') {throw 'Freefly pose refused'}
        Start-Sleep -Seconds 15
        $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix.png")}
        Start-Sleep -Seconds 2
    } else {
    $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-start.png")}
    $null=Send @{cmd='exec';code='hint ""; showCinemaBorder false; setViewDistance 150000; fusionCam="camera" camCreate [51200,26000,58000]; fusionCam cameraEffect ["internal","back"]; fusionCam camSetTarget [51200,44000,0]; fusionCam camCommit 0'}
    Start-Sleep -Seconds 4
    $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-overview.png")}
    $null=Send @{cmd='exec';code='fusionCam camSetPos [51200,44000,80000]; fusionCam camSetTarget [51200,44100,0]; fusionCam camCommit 0'}
    Start-Sleep -Seconds 4
    $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-high.png")}
    $sites=if ($Large) {@(@('south',10000,10000),@('centre',51200,51200),@('north',92000,92000))} else {@(@('everon',78660,64700),@('malden',28800,22400),@('kolgujev',60800,35200),@('nogova',17364,54008))}
    foreach ($site in $sites) {
        $name=$site[0]; $x=$site[1]; $z=$site[2]
        $height=[double](Send @{cmd='eval';code="triTerrainHeight [$x,$z]"}).result
        if ($height -le 0) {throw "Expected above-water height at $name, got $height"}
        $null=Send @{cmd='exec';code="setViewDistance 50000; vehicle player setPos [$x,$z,1200]; vehicle player setVelocity [0,0,0]; fusionCam camSetPos [$x,$($z-500),1200]; fusionCam camSetTarget [$x,$($z+800),0]; fusionCam camCommit 0"}
        Start-Sleep -Seconds 3
        $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-$name.png")}
        $null=Send @{cmd='eval';code='getPos vehicle player'}
        $p.Refresh(); Write-Host "$name height=$height workingSetMB=$([int]($p.WorkingSet64/1MB))"
    }
    if (!$Large) {
        # the map at Kolgujev: squares past 100 (Fusion is 800 across) need the grid's third label character
        $null=Send @{cmd='exec';code='fusionCam cameraEffect ["terminate","back"]; camDestroy fusionCam; vehicle player setPos [60800,35200,400]; vehicle player setVelocity [0,0,0]'}
        Start-Sleep -Seconds 2
        foreach ($c in @('triOpenMap','triShowMap 1')) { if ((Send @{cmd='eval';code=$c}).result.Trim('"') -ne 'OK') {throw "$c failed"} }
        foreach ($zoom in @(@('near',0.05),@('far',0.6))) {
            $null=Send @{cmd='eval';code="triMapSetScale $($zoom[1])"}
            Start-Sleep -Seconds 2
            $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-map-$($zoom[0]).png")}
        }
        $null=Send @{cmd='eval';code='triShowMap 0'}
        # save and load on the big world: the load must put the player back on Kolgujev, not where it moved after
        function PosOf { ((Send @{cmd='eval';code='getPos vehicle player'}).result -replace '[\[\]" ]','').Split(',') | ForEach-Object {[double]::Parse($_,[Globalization.CultureInfo]::InvariantCulture)} }
        $saved=PosOf
        if ((Send @{cmd='eval';code='triSaveGame "fusion-acceptance"'}).result.Trim('"') -ne 'OK') {throw 'Save failed'}
        $null=Send @{cmd='exec';code='vehicle player setPos [28800,22400,1200]'}
        Start-Sleep -Seconds 1
        if ((Send @{cmd='eval';code='triLoadGame "fusion-acceptance"'}).result.Trim('"') -ne 'OK') {throw 'Load failed'}
        Start-Sleep -Seconds 5
        $loaded=PosOf
        $moved=[Math]::Sqrt([Math]::Pow($loaded[0]-$saved[0],2)+[Math]::Pow($loaded[1]-$saved[1],2))
        Write-Host "save/load: saved $($saved[0..1] -join ',') loaded $($loaded[0..1] -join ',') ($([int]$moved) m apart)"
        if ($moved -gt 500) {throw "Load did not restore the position: $moved m from the save"}
        if ([double](Send @{cmd='eval';code='triTerrainHeight [60800,35200]'}).result -le 0) {throw 'Terrain lost after load'}
        $null=Send @{cmd='screenshot';path=(Join-Path $output "$prefix-loaded.png")}
    }
    }
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
    $text=Get-Content -LiteralPath $log -Raw
    if ($SatmapObjects -ne 0 -and !$NoSatmap -and $text -notmatch 'OFP satmap objects: [1-9][0-9]* models, [1-9][0-9]* textures, [1-9][0-9]* instances') {throw 'Object satellite bake was not exercised'}
    if ($text -match 'heightfield upload failed|UNHANDLED EXCEPTION' -or $text -notmatch 'heightfield ready=true') {throw 'Terrain upload acceptance failed'}
    if (!$Large -and !$Pose) {
        # each island's own ambient set played while the camera was there (EnvSoundRegions.cpp logs each crossing)
        foreach ($island in @('Everon','Malden','Kolgujev','Nogova')) {
            if ($text -notmatch "Ambient sound region: $island ") {throw "No ambient sound region change into $island"}
        }
        $text -split "`n" | Where-Object {$_ -match 'Ambient sound region:'} | ForEach-Object {Write-Host ($_ -replace '^.*Ambient','Ambient').Trim()}
    }
    Write-Host "Runtime checks passed; visually inspect $output/$prefix-*.png."
} finally {
    if ($null -ne $client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill(); $null=$p.WaitForExit(10000)}
}
