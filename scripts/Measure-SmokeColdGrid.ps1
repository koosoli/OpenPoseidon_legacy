param([string]$Label='baseline', [switch]$LegacyRaster,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/smoke-cold-grid'
$user=Join-Path $out "user-$Label"
New-Item -ItemType Directory -Force $user | Out-Null
[IO.File]::WriteAllText((Join-Path $user 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$settings=@{POSEIDON_USER_DIR=$user;POSEIDON_TEST_SMOKE='1';POSEIDON_TEST_SMOKE_AT='7731.6 25.3 4421.4';
 POSEIDON_SMOKE_SYSTEM='2';POSEIDON_SMOKE_THICKNESS='8';POSEIDON_WIND_OVERRIDE='18 250 0.2';
 WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='0.25';POSEIDON_INTERIOR_COMPONENT_RASTER='1';POSEIDON_INTERIOR_RASTER_TRACE='1'}
if ($LegacyRaster) {$settings.POSEIDON_INTERIOR_COMPONENT_RASTER='0'}
& "$PSScriptRoot/farfield-bench.ps1" -Label $Label -Mission (Join-Path $root 'dev-missions/smoke-room.noe') `
 -GameDir $GameDir -Out $out -Env $settings -Repeats 1 -Width 1280 -Height 720 -Windowed `
 -ExtraArgs @('--test-world-freefly','7755','4405','29','310','-8') `
 -WarmupSeconds 30 -TimeoutSeconds 180 -RequireAll
$log=Join-Path (Join-Path $out $Label) 'run-01.log'
if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|Cannot load --test-world' -Quiet) {throw 'Invalid smoke run'}
if (!(Select-String -LiteralPath $log -Pattern "BuildingInterior 'ResHousehasic_zbroj'" -Quiet)) {throw 'Expected interior was not built'}
Select-String -LiteralPath $log -Pattern 'BuildingInterior.*build=|SmokeVolume: particles=' | Select-Object -Last 12 | ForEach-Object {$_.Line}
