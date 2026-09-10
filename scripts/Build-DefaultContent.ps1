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
$pack = Join-Path $dist '@OP_VisualUpgrade'
New-Item -ItemType Directory -Force $stage, (Join-Path $pack 'AddOns') | Out-Null
& $Python "$PSScriptRoot/default-content/generate_materials.py" --spec "$source/materials.json" --out $generated --size 1024
if ($LASTEXITCODE -ne 0) { throw 'Material generation failed' }
# Only the verified road candidate enters the first integration package. The
# other authored surfaces remain sources until their real terrain mappings pass.
foreach ($kind in @('co','nohq')) {
    & $tools image convert "$generated/road_asphalt_$kind.png" "$stage/road_asphalt_$kind.paa" --format DXT5
    if ($LASTEXITCODE -ne 0) { throw "PAA encoding failed: $kind" }
}
Copy-Item -LiteralPath "$source/config.cpp" -Destination "$stage/config.cpp" -Force
Copy-Item -LiteralPath "$source/material-overrides.json" -Destination "$stage/materials.json" -Force
$expected = @('config.cpp','materials.json','road_asphalt_co.paa','road_asphalt_nohq.paa')
foreach ($file in Get-ChildItem -LiteralPath $stage -Force) {
    if ($file.PSIsContainer -or $file.Name -notin $expected) { throw "Unexpected staging content: $($file.FullName)" }
}
& $tools pbo pack $stage "$pack/AddOns/op_ground_materials.pbo" --prefix op_ground_materials
if ($LASTEXITCODE -ne 0) { throw 'Ground material PBO packaging failed' }
Copy-Item -LiteralPath "$source/mod.json" -Destination "$pack/mod.json" -Force
Write-Host "Staged first road integration package: $pack"
Write-Host 'Not a three-surface or Jeep acceptance. No original game archive was modified.'
