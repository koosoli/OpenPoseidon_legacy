[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/player-ground-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'player_ground_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $root 'engine')) `
    (Join-Path $root 'tests/unit/engine/Poseidon/World/Weather/test_player_ground_boot.cpp') `
    (Join-Path $root 'engine/Poseidon/World/Weather/SandField.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) {throw 'Player ground CPU policy compilation failed.'}
& $exe
if ($LASTEXITCODE -ne 0) {throw 'Player ground CPU policy failed.'}
$sand=Join-Path $output 'sand_field_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $root 'engine')) `
    (Join-Path $root 'tests/unit/engine/Poseidon/World/Weather/test_sand_field.cpp') `
    (Join-Path $root 'engine/Poseidon/World/Weather/SandField.cpp') '-o' $sand
if ($LASTEXITCODE -ne 0) {throw 'Sand field/source regression compilation failed.'}
& $sand
if ($LASTEXITCODE -ne 0) {throw 'Sand field/source regression failed.'}
& "$PSScriptRoot/Test-MudField.ps1" -Compiler $Compiler
# Call-site contracts complement actual helper/kernel execution above. The
# sound gate must not surround the physical helper; both dominant animations
# call it, while every real sand support tile retains four-quadrant proof.
$move=Get-Content (Join-Path $root 'engine/Poseidon/World/Entities/Infantry/SoldierOldMove.cpp') -Raw
if (($move | Select-String 'GroundBootEvents\(' -AllMatches).Matches.Count -ne 2 -or
    $move -notmatch 'DominantGroundBootMove\(_primaryFactor, true\)' -or
    $move -notmatch 'AnimatePoint\(_shape->FindMemoryLevel\(\), index\)') {throw 'Actual primary/secondary sole producer wiring missing.'}
$scene=Get-Content (Join-Path $root 'engine/Poseidon/World/Scene/SceneDraw.cpp') -Raw
if ($scene -notmatch 'ExplicitInsideViewDraw\(oi->drawLOD, oi->forceDrawLOD, obj->InsideLOD') {throw 'Actual explicit inside queue routing missing.'}
$contact=Get-Content (Join-Path $root 'engine/Poseidon/World/Weather/SandContact.cpp') -Raw
foreach ($proof in @('checkedX','for (const float v : {0.25f, 0.75f})','for (const float u : {0.25f, 0.75f})','StockSandSurface','supportRoadway != nullptr','GSnow().BaseDepthAt','sand.StampBoot')) {
    if (!$contact.Contains($proof)) {throw "Sand full support proof lost: $proof"}
}
Write-Host 'Player movement/queue CPU checks, sand/mud regression cases and actual call-site contracts PASS; installed manual-player proof pending.'
