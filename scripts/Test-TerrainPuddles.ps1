[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/terrain-puddle-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'terrain_puddles_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_terrain_puddles.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Terrain puddle policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Terrain puddle policy assertions failed.' }
Write-Host 'Terrain puddle CPU ground-receiver/soft-soil/liquid-rain/accumulation/pause/reset/frame-rate tests passed.'
