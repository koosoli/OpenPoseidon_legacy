[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/forest-snow-surface-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'forest_snow_surface_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_forest_snow_surface.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Forest snow source policy compilation failed.' }
& $exe (Join-Path $repo 'tests/unit/engine/WgpuRenderer/forest_stock_visuals.txt') (Join-Path $repo 'tests/unit/engine/WgpuRenderer/forest_nogova_visuals.txt')
if ($LASTEXITCODE -ne 0) { throw 'Forest snow source policy assertions failed.' }
