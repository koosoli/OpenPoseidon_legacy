param([string]$Python='python')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$stage=Join-Path $root 'build/jeep-ffv/animation-source'
$pack=Join-Path $root 'build/jeep-ffv/animation-preview'
$source=Join-Path $root 'content/default-packs/vehicle-actions'
$tools=Join-Path $root 'dist/x64-win-rwdi/PoseidonTools.exe'
New-Item -ItemType Directory -Force $stage, (Join-Path $pack 'AddOns') | Out-Null
& $Python "$PSScriptRoot/default-content/generate_jeep_animations.py" --spec "$source/jeep-pose.json" --out $stage
if ($LASTEXITCODE -ne 0) {throw 'Jeep pose generation failed'}
Copy-Item -LiteralPath "$source/preview-config.cpp" -Destination "$stage/config.cpp" -Force
$expected=@('config.cpp','jeep_idle.rtm','jeep_raise.rtm','jeep_aim.rtm','jeep_recoil.rtm','jeep_reload.rtm','jeep_lower.rtm')
foreach ($file in Get-ChildItem -LiteralPath $stage -Force) {
    if ($file.PSIsContainer -or $file.Name -notin $expected) {throw "Unexpected preview staging file: $($file.FullName)"}
}
& $tools pbo pack $stage "$pack/AddOns/op_jeep_animation_preview.pbo" --prefix op_jeep_animation_preview
if ($LASTEXITCODE -ne 0) {throw 'Preview packaging failed'}
Write-Host "Development preview only, one SP NPC integration seat, no default installation: $pack"
