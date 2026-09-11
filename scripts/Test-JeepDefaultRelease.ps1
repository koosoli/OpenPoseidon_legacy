param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
      [string]$OutputRoot='D:\OpenPoseidon-tests')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh for this whole serial campaign'}
$root=Split-Path $PSScriptRoot -Parent
$stage=Join-Path $OutputRoot ('jeep-default-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
if (Test-Path -LiteralPath $stage) {throw 'Fresh release destination must not exist'}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
# Stock data are still required. Only these installed data directories are linked;
# own packs, executable, DLL and mission are independent copies in the fresh folder.
foreach ($dir in @('BIN','DTA','AddOns','dtaExt','fonts','Worlds')) {
    $stock=Join-Path $GameDir $dir
    if (!(Test-Path -LiteralPath $stock -PathType Container)) {throw "Required installed data missing: $stock"}
    New-Item -ItemType Junction -Path (Join-Path $stage $dir) -Target $stock | Out-Null
}
& "$PSScriptRoot/Deploy.ps1" -GameDir $stage
$mission=Join-Path $stage 'Missions/JeepFFV.Intro'
New-Item -ItemType Directory -Force $mission | Out-Null
Copy-Item -LiteralPath "$root/dev-missions/jeep-ffv.Intro/mission.sqm" -Destination "$mission/mission.sqm"
function RunCase([string]$Name, [string[]]$Extra) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/Test-JeepPassenger.ps1" -GameDir $stage -MissionPath $mission @Extra *> "$stage/$Name.log"
    if ($LASTEXITCODE -ne 0) {throw "Fresh release case failed: $stage/$Name.log"}
    Write-Host "Fresh release $Name passed"
}
RunCase 'default-npc' @('-DefaultPackage','-NpcFire','-NpcTransitions','-SaveLoad')
RunCase 'default-player' @('-DefaultPackage','-PlayerFire','-SaveLoad','-Lifecycle')
RunCase 'disabled' @()
$archive=Join-Path $stage 'Mods/@OP_VehicleActions/AddOns/op_jeep_actions.pbo'
Move-Item -LiteralPath $archive -Destination "$archive.held"
try {RunCase 'missing' @('-DefaultPackage','-AutomaticOriginal','missing')}
finally {Move-Item -LiteralPath "$archive.held" -Destination $archive}
$metadata=Join-Path $stage 'Mods/@OP_VehicleActions/mod.json'
Move-Item -LiteralPath $metadata -Destination "$metadata.held"
try {
    Copy-Item -LiteralPath "$root/tests/fixtures/default-content/incompatible-vehicle-mod.json" -Destination $metadata
    RunCase 'incompatible' @('-DefaultPackage','-AutomaticOriginal','incompatible')
} finally {
    # These are single owned files, never a recursive operation on stock junctions.
    Remove-Item -LiteralPath $metadata -Force
    Move-Item -LiteralPath "$metadata.held" -Destination $metadata
}
$userMod=Join-Path $stage '@UserPriority'
New-Item -ItemType Directory -Force "$userMod/AddOns" | Out-Null
& "$root/dist/x64-win-rwdi/PoseidonTools.exe" pbo pack "$root/tests/fixtures/default-content/jeep-user-priority" "$userMod/AddOns/op_jeep_user_priority.pbo" --prefix op_jeep_user_priority
if ($LASTEXITCODE -ne 0) {throw 'Could not build user-priority fixture'}
RunCase 'user-priority' @('-DefaultPackage','-UserMod',$userMod)
Write-Host "Fresh release campaign passed: $stage. Original data/install untouched."
