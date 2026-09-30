[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/tree-snow-surface-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'tree_snow_surface_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_tree_snow_surface.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Tree snow source/lease policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Tree snow source/lease policy assertions failed.' }
Write-Host 'Tree snow loaded bounds, owner/leaf gates, frame budget, literal birth/model/transform, expiry and self-ignore geometry transport tests passed.'
