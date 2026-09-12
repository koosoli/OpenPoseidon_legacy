param([string]$Label='baseline', [switch]$LegacyRaster, [switch]$LegacyFaces, [switch]$Showcase,
      [switch]$LegacyBroadphase, [switch]$VerifyBatch,
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
 WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='0.25';POSEIDON_INTERIOR_COMPONENT_RASTER='1';POSEIDON_INTERIOR_RASTER_TRACE='1';POSEIDON_INTERIOR_FACE_INDEX='1'}
if ($LegacyRaster) {$settings.POSEIDON_INTERIOR_COMPONENT_RASTER='0'}
if ($LegacyFaces) {$settings.POSEIDON_INTERIOR_FACE_INDEX='0'}
if ($LegacyBroadphase) {$settings.POSEIDON_SMOKE_BROADPHASE='0'}
if ($VerifyBatch) {$settings.POSEIDON_SMOKE_BATCH_VERIFY='1'}
$mission=Join-Path $root 'dev-missions/smoke-room.noe'
$camera=@('--test-world-freefly','7755','4405','29','310','-8')
if ($Showcase) {
 $mission=Join-Path $GameDir 'Mods/@OP_Showcase/Missions/OpenPoseidon/ShowcaseLab.Intro'
 $settings.POSEIDON_TEST_SMOKE_AT='9547 31 3504'
 $camera=@('--test-world-freefly','9570','3495','40','290','-15')
}
& "$PSScriptRoot/farfield-bench.ps1" -Label $Label -Mission $mission `
 -GameDir $GameDir -Out $out -Env $settings -Repeats 1 -Width 1280 -Height 720 -Windowed `
 -ExtraArgs $camera `
 -WarmupSeconds 30 -TimeoutSeconds 180 -RequireAll
$log=Join-Path (Join-Path $out $Label) 'run-01.log'
if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|Cannot load --test-world|Smoke batch parity mismatch' -Quiet) {throw 'Invalid smoke run'}
if ($VerifyBatch -and !(Select-String -LiteralPath $log -SimpleMatch 'Smoke batch parity: 1000 exact ray comparisons completed' -Quiet)) {throw 'No positive batch parity evidence'}
$expected=if ($Showcase) {"BuildingInterior 'LabRoof'"} else {"BuildingInterior 'ResHousehasic_zbroj'"}
if (!(Select-String -LiteralPath $log -Pattern $expected -Quiet)) {throw 'Expected interior was not built'}
Select-String -LiteralPath $log -Pattern 'BuildingInterior.*build=|SmokeVolume: particles=' | Select-Object -Last 12 | ForEach-Object {$_.Line}
