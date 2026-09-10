param([string]$Label='acceptance',
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root "build/save-version/$Label"
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
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
    $stream=$client.GetStream(); $stream.ReadTimeout=120000
    $reader=[IO.StreamReader]::new($stream)
    $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false)); $writer.AutoFlush=$true
    function Send($command) {
        if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Renderer error'}
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
    if ((Send @{cmd='eval';code='alive player'}).result -ne 'true') {throw 'No living player'}
    $null=Send @{cmd='eval';code='player setDamage 0.25'}
    if ((Send @{cmd='eval';code='triSaveGame "version-current"'}).result.Trim('"') -ne 'OK') {throw 'Save failed'}
    $null=Send @{cmd='eval';code='player setDamage 0.5'}
    if ((Send @{cmd='eval';code='triLoadGame "version-current"'}).result.Trim('"') -ne 'OK') {throw 'Load failed'}
    Start-Sleep -Seconds 3
    if ((Send @{cmd='eval';code='alive player'}).result -ne 'true') {throw 'Player lost during load'}
    $damage=[double]::Parse((Send @{cmd='eval';code='damage player'}).result,[Globalization.CultureInfo]::InvariantCulture)
    if ([Math]::Abs($damage-0.25) -gt 0.01) {throw "State did not restore: damage=$damage"}
    $null=Send @{cmd='screenshot';path=(Join-Path $output 'restored.png')}
    $null=Send @{cmd='exit'}
    if (!$p.WaitForExit(20000) -or $p.ExitCode -ne 0) {throw 'Game exit failed'}
    Write-Host 'Current-version installed save/load passed. Future-version rejection is covered by the native test.'
} finally {
    if ($client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill()}
}
