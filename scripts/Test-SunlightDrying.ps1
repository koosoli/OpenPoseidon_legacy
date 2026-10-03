[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/sunlight-drying-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$lights=[IO.File]::ReadAllText((Join-Path $root 'engine/Poseidon/Graphics/Rendering/Lighting/Lights.cpp'))
$snapshot=[IO.File]::ReadAllText((Join-Path $root 'engine/Poseidon/Graphics/Rendering/Frame/PresentationSnapshot.cpp'))
$world=[IO.File]::ReadAllText((Join-Path $root 'engine/Poseidon/World/WorldImpl.cpp'))
$terrain=[IO.File]::ReadAllText((Join-Path $root 'engine/WgpuRenderer/TerrainWgpu.cpp'))
if (!$lights.Contains('_sunDirection = _direction;') -or !$lights.Contains('float sinSun = -_direction.Y();') -or
    !$lights.Contains('_direction = -sunToDirection;') -or !$snapshot.Contains('snap.sunDirection = sun->SunDirection();')) {
    throw 'Actual light-travel producer/snapshot contract changed; re-audit solar drying before accepting.'
}
if ($world -notmatch 'SunlightDryingExposure\(sun->SunDirection\(\)\.Y\(\),\s*overcast,\s*sun->NightEffect\(\)\)' -or
    $terrain -notmatch 'SunlightDryingExposure\(weather\.sunDirection\.Y\(\),\s*weather\.overcast,\s*weather\.nightEffect\)') {
    throw 'Actual physical/cosmetic source call sites do not use the same astronomical solar helper.'
}
if ($world -match 'clamp\(sun->SunDirection\(\)\.Y\(\)' -or $terrain -match 'clamp\(weather\.sunDirection\.Y\(\)' -or
    !$world.Contains('GWind.Sample().speed/20.0f') -or !$terrain.Contains('weather.windSpeed / 20.0f')) {
    throw 'Legacy sun sign or changed wind source remains in a drying call site.'
}
$exe=Join-Path $output 'sunlight_drying_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $root 'tests/unit/engine/weather/test_sunlight_drying.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) {throw 'Sunlight drying policy compilation failed.'}
& $exe
if ($LASTEXITCODE -ne 0) {throw 'Sunlight drying production assertions failed.'}
Write-Host 'PASS actual sun-travel source/snapshot/call sites, day/night/cloud/invalid inputs, unchanged physical/cosmetic rates, wind, mass and pause; no game/device/full build.'
