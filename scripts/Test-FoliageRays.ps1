param([switch]$RaysOff, [switch]$LegacyOrder, [switch]$Zeus, [string]$Label='baseline', [string]$World='',
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
if (!$World) {$World=Join-Path $root '../../../packages/a3-compat/world/stratis/stratis.wrp'}
if (!(Test-Path -LiteralPath $World)) {throw 'Specify the original Stratis WRP with -World'}
$World=(Resolve-Path -LiteralPath $World).Path
$output=Join-Path $root 'build/foliage-rays/acceptance'
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$env:WGR_EARLY_LATE_OPAQUE=if ($LegacyOrder) {'0'} else {'1'}
$env:WGR_LATE_OBJECT_TRACE='1'
$env:WGR_AUTO_EXPOSURE='0'
$env:WGR_EXPOSURE='1'
$env:WGR_GODRAYS=if ($RaysOff) {'0'} else {'1'}
$name="$Label-rays$env:WGR_GODRAYS"
$capture=Join-Path $output "$name.png"
$log=Join-Path $output "$name.log"
$mission=Join-Path $root 'dev-missions/foliage-rays.Stratis'
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
$cameraArgs=if ($Zeus) {@('--dev','--harness',"$port")} else {@('--no-dev','--test-world-freefly','1747.43','5037.95','5.85','81.9','1.2','--auto-screenshot',('"35s:'+$capture+'"'))}
$argsGame=@('--render','wgpu','--window','--mod','@a3stratis','--width','1280','--height','720','--test-mission',('"'+$mission+'"'),'--test-world',('"'+$World+'"'),'--log-file',('"'+$log+'"')) + $cameraArgs
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output "$name.stdout.txt") -RedirectStandardError (Join-Path $output "$name.stderr.txt") -ArgumentList $argsGame
$null=$p.Handle
$client=$null
try {
    if ($Zeus) {
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
            return $r
        }
        Start-Sleep -Seconds 5
        $null=Send @{cmd='exec';code='player setPos [3000,3000,0]'}
        $r=Send @{cmd='eval';code='triFreeFlyPose "1747.43 5037.95 5.85 81.9 1.2"'}
        if ($r.result.Trim('"') -ne 'OK') {throw 'Zeus camera refused'}
        Start-Sleep -Seconds 35
        if ((Send @{cmd='eval';code='alive player'}).result -ne 'true') {throw 'Player died'}
        $null=Send @{cmd='screenshot';path=$capture}
        $null=Send @{cmd='exit'}
    }
    if (!$p.WaitForExit(180000) -or $p.ExitCode -ne 0) {throw 'Foliage capture failed'}
    if (!(Test-Path -LiteralPath $capture)) {throw 'Missing foliage capture'}
    if (Select-String -LiteralPath $log -Pattern 'StartAutoTest could not boot' -Quiet) {throw 'Mission did not load'}
    if (!(Select-String -LiteralPath $log -Pattern 'Terrain materials .*stratis.wrp:.*satellite' -Quiet)) {throw 'Stratis was not loaded'}
    Write-Host $capture
    Select-String -LiteralPath $log -Pattern 'late objects:|sun:|god.ray' | Select-Object -Last 12 | ForEach-Object {$_.Line}
} finally {
    if ($null -ne $client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill()}
}
