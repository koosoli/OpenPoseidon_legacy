[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/object-snow-surface-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'object_snow_surface_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_object_snow_surface.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Object snow surface policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Object snow surface policy assertions failed.' }
Write-Host 'Object snow current-section pose, proof bounds and effective-cover policy tests passed.'
