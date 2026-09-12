$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Join-Path (Split-Path $PSScriptRoot -Parent) ('build/mods-layout-fixtures/'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
function FixtureFile($relative,$text) {
    $path=Join-Path $root $relative
    New-Item -ItemType Directory -Force (Split-Path $path -Parent) | Out-Null
    [IO.File]::WriteAllText($path,$text)
}
FixtureFile 'valid/OpenPoseidon.exe' 'fixture, not executable'
FixtureFile 'valid/@OP_VehicleActions/mod.json' '{"defaultContentApi":"1","version":"1.0.0"}'
FixtureFile 'valid/@OP_VehicleActions/AddOns/op_jeep_actions.pbo' 'fixture archive'
FixtureFile 'valid/AddOns/fusion_ofp.pbo' 'fusion fixture'
FixtureFile 'valid/AddOns/stock.pbo' 'untouched stock'
FixtureFile 'valid/Missions/FusionTour.FusionOFP/custom.sqs' 'preserve user addition'
FixtureFile 'valid/@OP_VehicleActions/Missions/jeep-manual.Intro/mission.sqm' 'old jeep'
FixtureFile 'valid/Mods/@OP_Showcase/Missions/ShowcaseLab.Intro/mission.sqm' 'existing showcase'
& "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir "$root/valid"
if (Test-Path -LiteralPath "$root/valid/@OP_VehicleActions") {throw 'Old pack remains'}
if (Test-Path -LiteralPath "$root/valid/AddOns/fusion_ofp.pbo") {throw 'Old map archive remains'}
if ((Get-Content -LiteralPath "$root/valid/Mods/@OP_Fusion/Missions/OpenPoseidon/FusionTour.FusionOFP/custom.sqs") -ne 'preserve user addition') {throw 'Mission content lost'}
if ((Get-Content -LiteralPath "$root/valid/AddOns/stock.pbo") -ne 'untouched stock') {throw 'Stock content changed'}
if ((Get-Content -LiteralPath "$root/valid/Mods/@OP_Showcase/Missions/OpenPoseidon/ShowcaseLab.Intro/mission.sqm") -ne 'existing showcase') {throw 'Installed mission lost'}
if ((Get-Content -LiteralPath "$root/valid/Mods/@OP_VehicleActions/Missions/OpenPoseidon/jeep-manual.Intro/mission.sqm") -ne 'old jeep') {throw 'Legacy pack mission lost'}
& "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir "$root/valid"
FixtureFile 'conflict/OpenPoseidon.exe' 'fixture'
FixtureFile 'conflict/AddOns/fusion_ofp.pbo' 'original'
FixtureFile 'conflict/Mods/@OP_Fusion/AddOns/fusion_ofp.pbo' 'different'
$refused=$false
try {& "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir "$root/conflict"} catch {$refused=$true}
if (!$refused -or (Get-Content -LiteralPath "$root/conflict/AddOns/fusion_ofp.pbo") -ne 'original') {throw 'Conflict not preserved'}
FixtureFile 'linked/OpenPoseidon.exe' 'fixture'
FixtureFile 'mission-conflict/OpenPoseidon.exe' 'fixture'
FixtureFile 'mission-conflict/Missions/FusionTour.FusionOFP/mission.sqm' 'original'
FixtureFile 'mission-conflict/Mods/@OP_Fusion/Missions/FusionTour.FusionOFP/mission.sqm' 'modified'
$refused=$false
try {& "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir "$root/mission-conflict"} catch {$refused=$true}
if (!$refused -or !(Test-Path "$root/mission-conflict/Missions/FusionTour.FusionOFP/mission.sqm")) {throw 'Converging mission conflict not preserved'}
FixtureFile 'outside/fusion_ofp.pbo' 'external fixture'
New-Item -ItemType Junction -Path "$root/linked/AddOns" -Target "$root/outside" | Out-Null
$refused=$false
try {& "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir "$root/linked"} catch {$refused=$true}
if (!$refused -or (Get-Content -LiteralPath "$root/outside/fusion_ofp.pbo") -ne 'external fixture') {throw 'Linked source not protected'}
Write-Output "PASS: migration, idempotence, preservation, conflict and junction refusal. Fixtures: $root"
