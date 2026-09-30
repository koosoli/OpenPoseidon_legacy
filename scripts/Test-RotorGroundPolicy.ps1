[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/rotor-ground-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'rotor_ground_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $root 'engine')) `
    (Join-Path $root 'tests/unit/engine/weather/test_rotor_ground.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) {throw 'Rotor ground CPU policy compilation failed.'}
& $exe
if ($LASTEXITCODE -ne 0) {throw 'Rotor ground CPU policy failed.'}
$cpp=Get-Content (Join-Path $root 'engine/Poseidon/World/Entities/Vehicles/Air/Helicopter.cpp') -Raw
foreach ($proof in @('RotorLandPuff::MaxLive','RotorGroundRing(_landRotorPhase','GRainWater().At(x, z)',
    'GLandscape->SurfaceAt(x, z)','ObjIntersectView','hits.Size()',
    'EvaluateRotorWash(wash, origin)','GAirflow.Sample(Position())','puff->SetSmokeShadow(false)',
    'EmitLandRotorRing(deltaT, prec)','GSnow().ErodeRotor')) {
    if (!$cpp.Contains($proof)) {throw "Rotor actual emission/support proof missing: $proof"}
}
Write-Host 'Rotor land policy/hydrology/annulus CPU tests and production call-site contracts PASS; full build and installed hover proof pending.'
