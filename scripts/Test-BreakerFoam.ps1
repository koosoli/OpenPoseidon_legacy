param([switch]$Legacy, [switch]$LegacyCrest, [float]$WaveFoam=0.62, [float]$Wind=18, [float]$Exposure=1.0,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/breaker-foam/acceptance'
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
$water=Join-Path $env:POSEIDON_USER_DIR 'water-look'
New-Item -ItemType Directory -Force $water | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=-1;`nvsync=0;`nfpsCap=60;`n")
$gain=$WaveFoam.ToString([Globalization.CultureInfo]::InvariantCulture)
[IO.File]::WriteAllText((Join-Path $water 'noe_noe_wrp.cfg'), "v 5`nwave 2.2 1 1 0.65`nwhitecaps $gain 1 1`ncoast 0.04 1.2 1.12 0 0.14 0.018 0.4 0.1`n")
$env:WGR_BREAKER_HISTORY_LEGACY=if ($Legacy) {'1'} else {'0'}
$env:WGR_DEEP_BREAKER_RECOVERY=if ($LegacyCrest) {'0'} else {'1'}
$env:POSEIDON_WIND_OVERRIDE="$Wind 90 0"
$env:WGR_AUTO_EXPOSURE='0'
$env:WGR_EXPOSURE=$Exposure.ToString([Globalization.CultureInfo]::InvariantCulture)
$name=if ($Legacy) {'legacy'} else {'linear'}
if ($LegacyCrest) { $name += '-legacycrest' }
$name="$name-wind$Wind-gain$gain-exposure$env:WGR_EXPOSURE"
$capture=Join-Path $output "$name.png"
$log=Join-Path $output "$name.log"
$mission=Join-Path $root 'dev-missions/foam-ocean.noe'
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--test-mission',('"'+$mission+'"'),'--test-world-freefly','13000','3000','8','90','-30','--auto-screenshot',('"25s:'+$capture+'"'),'--log-file',('"'+$log+'"'))
$null=$p.Handle
try {
    if (!$p.WaitForExit(180000) -or $p.ExitCode -ne 0) {throw 'Breaker foam capture failed'}
    if (!(Test-Path -LiteralPath $capture)) {throw 'Missing foam capture'}
    if (!(Select-String -LiteralPath $log -Pattern 'amplitude=2.200' -Quiet)) {throw 'Water test profile was not applied'}
    Write-Host $capture
    Select-String -LiteralPath $log -Pattern 'Water GPU ms' | Select-Object -Last 8 | ForEach-Object { $_.Line }
} finally { if (!$p.HasExited) {$p.Kill()} }
