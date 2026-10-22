[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/reflect-material'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'reflect_material_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_reflect_material.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Reflect material classifier compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Reflect material classifier assertions failed.' }
Write-Host 'REFLECT-001 surface classification (declared/RVMAT/part/body/default, era lamps, military matt) tests passed.'
