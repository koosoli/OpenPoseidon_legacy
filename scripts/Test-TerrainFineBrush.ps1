param([switch]$LargeNogova, [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root ('build/terrain-brush/acceptance/brush-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $env:POSEIDON_USER_DIR -DlssMode 0
$env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE='0.15'
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$mission=Join-Path $root 'dev-missions/terrain-brush.Intro'
if ($LargeNogova) {$mission=Join-Path $root 'dev-missions/smoke-room.noe'}
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--log-file',('"'+(Join-Path $output 'fine.log')+'"'))
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
    Start-Sleep -Seconds 12
    if ($LargeNogova) {
        $null=Send @{cmd='exec';code='player allowDamage false; player switchCamera "EXTERNAL"'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'before-external.png')}
        $null=Send @{cmd='eval';code='getPos player'}
        $null=Send @{cmd='eval';code='triTerrainHeight [7760,4405]'}
        # Keep the actor outside all three footprints; burial is not invisibility.
        foreach ($stroke in @('[7900,4405,40,-1]','[7900,4510,40,-1]','[7790,4540,40,1]')) {
            for ($i=0; $i -lt 20; ++$i) {
                $r=Send @{cmd='eval';code=('triTerrainPaint '+$stroke)}
                if ($r.result -notmatch 'OK') {throw "Large edit refused: $stroke"}
            }
        }
        foreach ($view in @('EXTERNAL','INTERNAL')) {
            $null=Send @{cmd='exec';code=('player switchCamera "'+$view+'"')}
            Start-Sleep -Seconds 2
            $null=Send @{cmd='screenshot';path=(Join-Path $output ('after-'+$view+'.png'))}
        }
        $null=Send @{cmd='eval';code='getPos player'}
        $alive=Send @{cmd='eval';code='alive player'}
        if ($alive.result -notmatch 'true') {throw 'Player died during edit test'}
        $null=Send @{cmd='eval';code='triTerrainHeight [7760,4405]'}
        $null=Send @{cmd='eval';code='triTerrainBrushMode "restore"'}
        $null=Send @{cmd='exec';code='player switchCamera "EXTERNAL"'}
        Start-Sleep -Seconds 2
        $null=Send @{cmd='screenshot';path=(Join-Path $output 'restored-external.png')}
        $null=Send @{cmd='exit'}
        if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
        Write-Host "Large Nogova edits completed. Inspect actor visibility in $output; state alone is not visual acceptance."
        return
    }
    $null=Send @{cmd='eval';code='triFreeFlyPose "9700 3570 55 0 -45"'}
    $a=[double](Send @{cmd='eval';code='triTerrainHeight [9700,3600]'}).result
    $b=[double](Send @{cmd='eval';code='triTerrainHeight [9706.25,3600]'}).result
    $r=Send @{cmd='eval';code='triTerrainPaint [9700,3600,0.5,-3]'}
    if ($r.result -notmatch 'OK') {throw 'Fine edit refused'}
    $cut=[double](Send @{cmd='eval';code='triTerrainHeight [9700,3600]'}).result
    $outside=[double](Send @{cmd='eval';code='triTerrainHeight [9706.25,3600]'}).result
    if ([Math]::Abs($cut-($a-3)) -gt 0.01 -or [Math]::Abs($outside-$b) -gt 0.001) {throw 'Fine brush exceeded its footprint'}
    Start-Sleep -Seconds 2
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'fine-crater.png')}
    $null=Send @{cmd='eval';code='triTerrainBrushMode "restore"'}
    $r=Send @{cmd='eval';code='triTerrainPaint [9703.125,3603.125,0.5,-3]'}
    if ($r.result -notmatch 'OK') {throw 'Cell-centre stroke missed every vertex'}
    $centre=[double](Send @{cmd='eval';code='triTerrainHeight [9700,3600]'}).result
    if ($centre -ge $a-0.001) {throw 'No cell-centre influence'}
    $null=Send @{cmd='eval';code='triTerrainBrushMode "restore"'}
    $restored=[double](Send @{cmd='eval';code='triTerrainHeight [9700,3600]'}).result
    if ([Math]::Abs($restored-$a) -gt 0.001) {throw 'Restore failed'}
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
    Write-Host "Fine radius: centre $a -> $cut; neighbour $b -> $outside; cell centre $centre; restored $restored"
} finally {
    if ($null -ne $client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill()}
}
