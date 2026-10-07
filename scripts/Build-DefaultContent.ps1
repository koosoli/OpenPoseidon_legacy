param([string]$Preset = 'win-x64-clang-rwdi', [string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') { throw 'Unsupported content staging preset' }
$suffix = ($Preset -split '-')[-1]
$dist = Join-Path $root "dist/x64-win-$suffix"
$tools = Join-Path $dist 'PoseidonTools.exe'
if (!(Test-Path -LiteralPath $tools)) { throw 'Build PoseidonTools before packaging default content' }
$source = Join-Path $root 'content/default-packs/visual'
$generated = Join-Path $root 'build/default-content/generated'
$stage = Join-Path $root 'build/default-content/pbo-source'
$pack = Join-Path $dist 'Mods/@OP_VisualUpgrade'
New-Item -ItemType Directory -Force $stage, (Join-Path $pack 'AddOns') | Out-Null
& $Python "$PSScriptRoot/default-content/generate_materials.py" --spec "$source/materials.json" --out $generated --size 1024
if ($LASTEXITCODE -ne 0) { throw 'Material generation failed' }
# Explicitly verified source identities only; no filename heuristic.
$expected = @('config.cpp','materials.json')
foreach ($material in @('road_asphalt','sand','gravel')) {
    foreach ($kind in @('co','nohq')) {
        $name = "${material}_$kind"
        & $tools image convert "$generated/$name.png" "$stage/$name.paa" --format DXT5
        if ($LASTEXITCODE -ne 0) { throw "PAA encoding failed: $name" }
        $expected += "$name.paa"
    }
}
& $tools image convert "$generated/sand_detail_nohq.png" "$stage/sand_detail_nohq.paa" --format DXT5
if ($LASTEXITCODE -ne 0) { throw 'Sand detail normal encoding failed' }
$expected += 'sand_detail_nohq.paa'
Copy-Item -LiteralPath "$source/config.cpp" -Destination "$stage/config.cpp" -Force
Copy-Item -LiteralPath "$source/material-overrides.json" -Destination "$stage/materials.json" -Force
foreach ($file in Get-ChildItem -LiteralPath $stage -Force) {
    if ($file.PSIsContainer -or $file.Name -notin $expected) { throw "Unexpected staging content: $($file.FullName)" }
}
& $tools pbo pack $stage "$pack/AddOns/op_ground_materials.pbo" --prefix op_ground_materials
if ($LASTEXITCODE -ne 0) { throw 'Ground material PBO packaging failed' }
Copy-Item -LiteralPath "$source/mod.json" -Destination "$pack/mod.json" -Force
Write-Host "Staged road and terrain integration package: $pack"
& "$PSScriptRoot/Build-VehicleActions.ps1" -Preset $Preset -Python $Python
& "$PSScriptRoot/Build-InventoryIcons.ps1" -Preset $Preset
$menuStage = Join-Path $dist 'assets/menu/RoughSea.Intro'
New-Item -ItemType Directory -Force $menuStage | Out-Null
foreach ($name in @('mission.sqm','init.sqs')) {
    Copy-Item -LiteralPath (Join-Path $root "content/menu/RoughSea.Intro/$name") -Destination (Join-Path $menuStage $name) -Force
}
Write-Host 'Staging alone is not visual/runtime acceptance. No original game archive was modified.'
