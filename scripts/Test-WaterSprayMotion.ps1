param([switch]$LegacyAnchor, [switch]$Dlss,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$label='spray-'+(Get-Date -Format 'yyyyMMdd-HHmmss')
if ($LegacyAnchor) {$label+='-legacy'}
if ($Dlss) {$label+='-dlss'}
$out=Join-Path $root 'build/water-spray-motion'
$user=Join-Path $out ('user-'+$label)
$water=Join-Path $user 'water-look'
New-Item -ItemType Directory -Force $water | Out-Null
$dlssMode=if ($Dlss) {-1} else {0}
[IO.File]::WriteAllText((Join-Path $user 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=$dlssMode;`nvsync=0;`nfpsCap=60;`n")
[IO.File]::WriteAllText((Join-Path $water 'noe_noe_wrp.cfg'), "v 5`nwave 2.2 1 1 0.65`nwhitecaps 0.62 1 1`ncoast 0.04 1.2 1.12 0 0.14 0.018 0.4 0.1`n")
$settings=@{POSEIDON_USER_DIR=$user;POSEIDON_WIND_OVERRIDE='18 90 0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1';WGR_BREAKER_CELLS='0';WGR_SPRAY_WORLD_ANCHOR='1'}
if ($LegacyAnchor) {$settings.WGR_SPRAY_WORLD_ANCHOR='0'}
& "$PSScriptRoot/farfield-movecam.ps1" -Label $label -Mission 'dev-missions/foam-ocean.noe' -Start 13000,3000 -StartY 8 -GroundY 0 -Azimuth 90 -Elevation -30 -LookDistance 50 -Speed 2 -SettleSeconds 12 -StaticSeconds 8 -MoveSeconds 30 -LoadSeconds 5 -CaptureAt 45 -Width 1280 -Height 720 -GameDir $GameDir -Out $out -Env $settings
$log=Join-Path (Join-Path $out $label) 'run-01.log'
if (!(Select-String -LiteralPath $log -Pattern 'amplitude=2.200' -Quiet)) {throw 'Wave profile was not applied'}
if (!(Select-String -LiteralPath $log -Pattern 'FT M' -Quiet)) {throw 'No moving-phase evidence'}
if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost' -Quiet) {throw 'Renderer error'}
Write-Output "Motion evidence: $log"
