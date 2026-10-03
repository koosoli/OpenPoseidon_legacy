[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'build/rain-water-fine-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'rain_water_fine_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/weather/test_rain_water_fine.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Fine water policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Fine water policy assertions failed.' }
$geometryExe=Join-Path $output 'rain_water_geometry_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/weather/test_rain_water_geometry.cpp') '-o' $geometryExe
if ($LASTEXITCODE -ne 0) { throw 'Fine water geometry compilation failed.' }
& $geometryExe
if ($LASTEXITCODE -ne 0) { throw 'Fine water geometry assertions failed.' }
$fluxExe=Join-Path $output 'rain_water_flux_stability_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/weather/test_rain_water_flux_stability.cpp') '-o' $fluxExe
if ($LASTEXITCODE -ne 0) { throw 'Fine water flux stability compilation failed.' }
& $fluxExe
if ($LASTEXITCODE -ne 0) { throw 'Fine water flux stability assertions failed.' }
