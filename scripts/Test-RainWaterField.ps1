[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'build/rain-water-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'rain_water_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/weather/test_rain_water.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Rain water policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Rain water policy assertions failed.' }
Write-Host 'Rainwater actual hollow, spill, downhill flow, sea outlet, water budget, drainage, sunlight, pause, reset and partition checks passed.'
$sourceExe=Join-Path $output 'rain_water_source_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/weather/test_rain_water_source.cpp') '-o' $sourceExe
if ($LASTEXITCODE -ne 0) { throw 'Fine water source compilation failed.' }
& $sourceExe
if ($LASTEXITCODE -ne 0) { throw 'Fine water source assertions failed.' }
