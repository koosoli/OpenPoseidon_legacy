param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
      [string]$OutputDir='', [ValidateSet('LAW','Shell105','Shell125','Heat')][string]$Ammo='LAW')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$output=if ($OutputDir) {[IO.Path]::GetFullPath($OutputDir)} else {
    Join-Path $root ('build/rocket-craters/acceptance-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=-1;`n")
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$mission=Join-Path $root 'dev-missions/snow-crawl.Intro'
$log=Join-Path $output 'rocket.log'
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--dev','--width','1280','--height','720','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-freefly','9700','3550','65','0','-45','--log-file',('"'+$log+'"'))
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
    function Height {return [double](Send @{cmd='eval';code='triTerrainHeight [9700,3600]'}).result}
    function Fire {
        $height=((Height)+15).ToString([Globalization.CultureInfo]::InvariantCulture)
        $null=Send @{cmd='exec';code=('craterRocket="'+$Ammo+'" createVehicle [9700,3600,'+$height+']; craterRocket setPos [9700,3600,'+$height+']; craterRocket setVectorDirAndUp [[0,0,-1],[0,1,0]]; craterRocket setVelocity [0,0,-100]')}
        Start-Sleep -Seconds 5
    }
    Start-Sleep -Seconds 15
    $before=Height
    Fire
    $default=Height
    # Classic script runtime has no getNumber/configFile command. Use the actual
    # impact's logged ammo property, then independently calculate expected depth.
    $impact=Select-String -LiteralPath $log -Pattern 'Rocket crater:.*blast=([0-9.eE+-]+) changed=true' | Select-Object -Last 1
    if (!$impact) {throw 'Missing successful explosive impact'}
    $blast=[double]::Parse($impact.Matches[0].Groups[1].Value,[Globalization.CultureInfo]::InvariantCulture)
    $expected=[Math]::Min(2.0,[Math]::Max(0.25,[Math]::Pow($blast/150.0,1.0/3.0)))
    if ($default -ge $before-0.1) {throw 'Default did not crater'}
    if ([Math]::Abs(($before-$default)-$expected) -gt 0.05) {throw "Expected ammo-scaled depth $expected at the grid vertex"}
    $null=Send @{cmd='eval';code='triRocketCraters "off"'}
    Fire
    $off=Height
    if ([Math]::Abs($default-$off) -gt 0.001) {throw 'Disabled changed terrain'}
    $null=Send @{cmd='eval';code='triRocketCraters "reset"'}
    if ([Math]::Abs((Height)-$off) -gt 0.001) {throw 'Settings reset undid crater'}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'default-crater.png')}
    Start-Sleep -Seconds 1
    $null=Send @{cmd='eval';code='triTerrainBrushMode "restore"'}
    if ([Math]::Abs((Height)-$before) -gt 0.001) {throw 'Restore before reset impact failed'}
    Fire
    $reset=Height
    if ([Math]::Abs(($before-$reset)-$expected) -gt 0.05) {throw 'Reset did not restore ammo-scaled default'}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'craters.png')}
    $null=Send @{cmd='eval';code='triTerrainBrushMode "restore"'}
    $restored=Height
    if ([Math]::Abs($restored-$before) -gt 0.001) {throw 'Terrain reset did not restore original height'}
    Write-Host "$Ammo HEIGHTS $before -> $default -> $off -> $reset -> $restored"
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Renderer error'}
} finally {
    if ($client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill()}
}
