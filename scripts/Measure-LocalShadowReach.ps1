param([switch]$Matched, [switch]$ForceRefresh, [ValidateRange(1,4)][int]$Repeats=2,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$arm=if ($Matched) {'matched'} else {'control'}
if ($ForceRefresh) {$arm+='-refresh'}
$out=Join-Path $root 'build/local-shadow-reach'
$user=Join-Path $out "user-$arm"
New-Item -ItemType Directory -Force $user | Out-Null
[IO.File]::WriteAllText((Join-Path $user 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=0;`n")
[IO.File]::WriteAllText((Join-Path $user 'game.cfg'), "preferredViewDistance=50000;`n")
$settings=@{POSEIDON_USER_DIR=$user; WGR_AUTO_EXPOSURE='0'; WGR_EXPOSURE='2';
    WGR_LOCAL_REACH_TRACE='1'; WGR_STATIC_LIGHT_TRACE='1'; WGR_MATCH_LOCAL_SHADOW_REACH=($(if ($Matched) {'1'} else {'0'}))}
$settings.POSEIDON_LOCAL_SHADOW_FORCE_REFRESH=if ($ForceRefresh) {'1'} else {'0'}
& "$PSScriptRoot/farfield-bench.ps1" -Label $arm -Mission (Join-Path $root 'dev-missions/freefly-lights.eden') `
    -GameDir $GameDir -ExtraArgs @('--test-world-freefly','5092','3950','65','0','-25') `
    -Out $out -Env $settings -Repeats $Repeats -Width 1280 -Height 720 `
    -Windowed -WarmupSeconds 60 -TimeoutSeconds 180 -RequireAll
for ($r=1; $r -le $Repeats; ++$r) {
    $log=Join-Path (Join-Path $out $arm) ('run-{0:D2}.log' -f $r)
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|StartAutoTest could not boot' -Quiet) {throw 'Invalid shadow run'}
    $expected=if ($Matched) {'180'} else {'54'}
    if (!(Select-String -LiteralPath $log -Pattern "start=9 endScale=55\.555\d+ shadow=$expected " -Quiet)) {
        throw 'Required streetlamp did not exercise the reach difference'
    }
}
