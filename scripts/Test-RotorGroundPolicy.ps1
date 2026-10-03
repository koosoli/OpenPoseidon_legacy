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
foreach ($proof in @('RotorLandPuff::MaxLive','RotorGroundRing(_landRotorPhase','GRainWater().At(px,pz)',
    'GLandscape->SurfaceAt(px,pz)','ObjIntersectView','hits.Size()',
    'RotorLandFootprint(probe)','LandRotorBoundsClear(point,centre,this)',
    'RotorLandProfile(_age, _density, _snow)','RotorLandCullScale(_radius, _shapeSphere)',
    'EvaluateRotorWash(wash, origin)','GAirflow.Sample(Position())','puff->SetSmokeShadow(false)',
    'EmitLandRotorRing(deltaT, prec)','GSnow().ErodeRotor',
    'POSEIDON_ROTOR_LAND_DRAW_TRACE','landDrawReceipt.Record(RotorLandDrawOutcome::DecalCall','RotorLandTint(emission.snow)',
    'scope=all-land-puffs-cpu-drawdecal-call-not-gpu-pixels')) {
    if (!$cpp.Contains($proof)) {throw "Rotor actual emission/support proof missing: $proof"}
}
# Stateful Cloudlet subclasses must not silently inherit its fixed-block pool.
# The actual class also has compiler-enforced distinct new/delete assertions.
$puffStart=$cpp.IndexOf('class RotorLandPuff final : public Cloudlet')
$puffEnd=$cpp.IndexOf('bool LandRotorFlag', $puffStart)
if ($puffStart -lt 0 -or $puffEnd -le $puffStart) {throw 'Rotor puff class boundary missing.'}
$puff=$cpp.Substring($puffStart,$puffEnd-$puffStart)
foreach ($allocatorProof in @('USE_FAST_ALLOCATOR;', 'DEFINE_FAST_ALLOCATOR(RotorLandPuff)',
    'sizeof(RotorLandPuff) > sizeof(Cloudlet)',
    '&RotorLandPuff::operator new != &Cloudlet::operator new',
    '&RotorLandPuff::operator delete != &Cloudlet::operator delete')) {
    if (!$puff.Contains($allocatorProof)) {throw "Stateful rotor puff allocator contract missing: $allocatorProof"}
}
Write-Host 'Rotor land policy/hydrology/annulus CPU tests and dedicated puff allocation contracts PASS; full build and installed hover proof pending.'
