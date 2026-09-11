param([switch]$RaysOff, [switch]$LegacyOrder, [switch]$LateSurface, [switch]$EarlyOpaque, [switch]$Zeus, [switch]$Everon, [string]$Label='baseline', [string]$World='',
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
      [string]$Pose='', [ValidateRange(-1,23.999)][double]$Hour=-1,
      [switch]$EarlyCutout, [ValidateRange(0.01,64)][double]$Exposure=1)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($EarlyCutout -and ($EarlyOpaque -or $LegacyOrder -or $LateSurface)) {throw 'Select only one comparison path'}
if (($EarlyOpaque -and ($LegacyOrder -or $LateSurface)) -or ($LegacyOrder -and $LateSurface)) {
    throw 'Select only one comparison path'
}
$root=Split-Path $PSScriptRoot -Parent
if ($Everon -and $World) {throw 'Everon uses its installed classic world, not the modern WRP override'}
if (!$Everon) {
    if (!$World) {$World=Join-Path $root '../../../packages/a3-compat/world/stratis/stratis.wrp'}
    if (!(Test-Path -LiteralPath $World)) {throw 'Specify the test WRP with -World'}
    $World=(Resolve-Path -LiteralPath $World).Path
}
$output=Join-Path $root 'build/foliage-rays/acceptance'
$env:POSEIDON_USER_DIR=Join-Path $output ('user-'+$Label)
New-Item -ItemType Directory -Force $env:POSEIDON_USER_DIR | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $env:POSEIDON_USER_DIR -DlssMode 0
$env:WGR_EARLY_LATE_OPAQUE=if ($EarlyCutout) {'2'} elseif ($EarlyOpaque) {'1'} else {'0'}
$env:WGR_LATE_SURFACE_RAYS=if ($LateSurface) {'1'} else {'0'}
$env:WGR_LATE_OBJECT_TRACE='1'
$env:WGR_AUTO_EXPOSURE='0'
$env:WGR_EXPOSURE=$Exposure.ToString([Globalization.CultureInfo]::InvariantCulture)
$env:WGR_LOD_GOVERNOR_RANGE='1'
$env:WGR_GODRAYS=if ($RaysOff) {'0'} else {'1'}
$name="$Label-rays$env:WGR_GODRAYS"
$capture=Join-Path $output "$name.png"
$log=Join-Path $output "$name.log"
$metrics=Join-Path $output "$name.json"
Get-Content -LiteralPath "$GameDir/DEPLOYED-FROM.txt" | Set-Content -LiteralPath "$output/$name.DEPLOYED-FROM.txt"
$mission=Join-Path $root $(if ($Everon) {'tests/perf/missions/perf_field.eden'} else {'dev-missions/foliage-rays.Stratis'})
if (!$Pose) {$Pose=if ($Everon) {'4320.38 4270.16 109.82 306.4 -0.1'} else {'1747.43 5037.95 5.85 81.9 1.2'}}
if (($Pose -split '\s+').Count -ne 5) {throw 'Pose requires x y z azimuth elevation'}
if ($Hour -lt 0) {$Hour=if ($Everon) {17.62} else {6.0}}
$hourText=$Hour.ToString([Globalization.CultureInfo]::InvariantCulture)
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
$listener.Start(); $port=$listener.LocalEndpoint.Port; $listener.Stop()
# Keep the panel from replacing the environment's A/B ray settings.
$cameraArgs=if ($Zeus) {@('--no-dev','--harness',"$port",'--auto-screenshot',('"60s:'+$capture+'"'))} else {@('--no-dev','--test-world-freefly') + ($pose -split ' ') + @('--auto-screenshot',('"35s:'+$capture+'"'))}
$worldArgs=@('--test-world-hour',$hourText)
if (!$Everon) {$worldArgs+=@('--mod','@a3stratis','--test-world',('"'+$World+'"'))}
$argsGame=@('--render','wgpu','--window','--width','1280','--height','720','--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'),'--capture-metrics',('"'+$metrics+'"')) + $cameraArgs + $worldArgs
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
        # CLI world-hour is tied to the CLI camera; the player/freefly path needs its own clock.
        $h=[int][Math]::Floor($Hour); $m=[int][Math]::Round(($Hour-$h)*60)
        $null=Send @{cmd='exec';code=("setDate [1985,6,21,$h,$m]")}
        $null=Send @{cmd='exec';code='player setPos [3000,3000,0]'}
        $r=Send @{cmd='eval';code=('triFreeFlyPose "'+$pose+'"')}
        if ($r.result.Trim('"') -ne 'OK') {throw 'Zeus camera refused'}
        Start-Sleep -Seconds 35
        if ((Send @{cmd='eval';code='alive player'}).result -ne 'true') {throw 'Player died'}
        # The application screenshot event also writes metrics and exits cleanly.
    }
    if (!$p.WaitForExit(180000) -or $p.ExitCode -ne 0) {throw 'Foliage capture failed'}
    if (!(Test-Path -LiteralPath $capture)) {throw 'Missing foliage capture'}
    if (Select-String -LiteralPath $log -Pattern 'StartAutoTest could not boot' -Quiet) {throw 'Mission did not load'}
    if (Select-String -LiteralPath $log -Pattern 'Cannot load --test-world' -Quiet) {throw 'World override failed'}
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Renderer error; reject foliage capture'}
    if ($LateSurface -and !$RaysOff) {
        $trace=Select-String -LiteralPath $log -Pattern 'late surface rays: ([0-9]+) material slots, intensity=([0-9.eE+-]+)' | Select-Object -Last 1
        if (!$trace -or [int]$trace.Matches[0].Groups[1].Value -le 0 -or
            [double]::Parse($trace.Matches[0].Groups[2].Value,[Globalization.CultureInfo]::InvariantCulture) -le 0) {
            throw 'Late surface scattering not exercised; reject comparison'
        }
    }
    if (!$Everon -and !(Select-String -LiteralPath $log -Pattern 'Terrain materials .*stratis.wrp:.*satellite' -Quiet)) {throw 'Stratis was not loaded'}
    $j=Get-Content -LiteralPath $metrics -Raw | ConvertFrom-Json
    if ($j.build.render_width -ne 1280 -or $j.build.render_height -ne 720 -or
        $j.build.msaa_samples -ne 4 -or $j.build.dlss_active) {throw 'Unexpected graphics profile'}
    if ($j.objects.main_instances -le 0) {throw 'Empty object scene'}
    if ($j.objects.lod_governor.lod_min -ne $j.objects.lod_governor.lod_max) {throw 'LOD governor not fixed'}
    Write-Host $capture
    Select-String -LiteralPath $log -Pattern 'late objects:|sun:|god.ray' | Select-Object -Last 12 | ForEach-Object {$_.Line}
} finally {
    if ($null -ne $client) {$client.Dispose()}
    if (!$p.HasExited) {$p.Kill()}
}
