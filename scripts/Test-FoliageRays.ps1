param([switch]$RaysOff, [switch]$LegacyOrder, [string]$Label='baseline', [string]$World='',
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
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output "$name.stdout.txt") -RedirectStandardError (Join-Path $output "$name.stderr.txt") -ArgumentList @('--render','wgpu','--window','--no-dev','--mod','@a3stratis','--width','1280','--height','720','--test-mission',('"'+$mission+'"'),'--test-world',('"'+$World+'"'),'--test-world-freefly','1747.43','5037.95','5.85','81.9','1.2','--auto-screenshot',('"35s:'+$capture+'"'),'--log-file',('"'+$log+'"'))
$null=$p.Handle
try {
    if (!$p.WaitForExit(180000) -or $p.ExitCode -ne 0) {throw 'Foliage capture failed'}
    if (!(Test-Path -LiteralPath $capture)) {throw 'Missing foliage capture'}
    if (Select-String -LiteralPath $log -Pattern 'StartAutoTest could not boot' -Quiet) {throw 'Mission did not load'}
    if (!(Select-String -LiteralPath $log -Pattern 'Terrain materials .*stratis.wrp:.*satellite' -Quiet)) {throw 'Stratis was not loaded'}
    Write-Host $capture
    Select-String -LiteralPath $log -Pattern 'late objects:|sun:|god.ray' | Select-Object -Last 12 | ForEach-Object {$_.Line}
} finally {if (!$p.HasExited) {$p.Kill()}}
