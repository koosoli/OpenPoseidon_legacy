[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/snow-bullet-impact-policy'
$shim=Join-Path $out 'shim/Poseidon/World/Terrain'
New-Item -ItemType Directory -Force -Path $shim | Out-Null
# Only the Landscape declaration is a headless seam. The complete production
# SnowField and impact policy are included unchanged; altitude cover is disabled
# in these tests and no fake landscape can supply surface admission.
[IO.File]::WriteAllText((Join-Path $shim 'Landscape.hpp'),"#pragma once`nnamespace Poseidon { class Landscape { public: float SurfaceY(float,float) const { return 0; } }; extern Landscape* GLandscape; }`n")
$exe=Join-Path $out 'actual_snow_bullet_impact.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $out 'shim')) ('-I'+(Join-Path $root 'engine')) (Join-Path $root 'tests/unit/engine/Poseidon/World/Weather/test_snow_bullet_impact.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) {throw 'Actual snow field/impact policy compilation failed.'}
& $exe
if ($LASTEXITCODE -ne 0) {throw 'Actual snow field/impact policy assertions failed.'}
$source=Get-Content (Join-Path $root 'engine/Poseidon/World/Entities/Weapons/Shots.cpp') -Raw
foreach($proof in @('SnowBulletTerrainImpact(this,_parent,isect,lDirNorm','IsLocal(),Type()->explosive,hitSea || _waterImpactDone',
    'GSnow().BulletImpact(x,z,cut.radius,cut.depth,','groove.directionX,groove.directionZ,groove.length',
    'SnowBulletGroovePolicy(remaining,normal.Y(),incoming*normal,incoming.X(),incoming.Z())',
    'centre+a+b,centre+a-b,centre-a+b,centre-a-b','RoadSurfaceY(origin','ObjIntersectView','budget.Take(Glob.time.toFloat())')) {
    if (!$source.Contains($proof)) {throw "Actual projectile impact source contract missing: $proof"}
}
$call=$source.IndexOf('SnowBulletTerrainImpact(this,_parent,isect,lDirNorm')
$bounce=$source.LastIndexOf('BounceOff(normal, isect);',$call)
$impact=$source.LastIndexOf('Ballistics::NotifyImpact',$call)
$fx=$source.IndexOf('const bool showLegacyWaterImpact',$call)
if ($bounce -lt 0 -or $impact -le $bounce -or $call -le $impact -or $fx -le $call) {throw 'Snow edit moved ahead of terrain/fuse resolution or replaced existing FX.'}
Write-Host 'Snow impact CPU/source checks passed. Real projectile, physical roof rays, GPU geometry and installed shots remain pending.'
