[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/church-snow-surface-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'church_snow_surface_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_church_snow_surface.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Church snow surface policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Church snow surface policy assertions failed.' }
# The material veto is consumed by the shared receiver used by colour and prepass.
$shader = Get-Content (Join-Path $repo 'engine/WgpuRenderer/rust/src/gfx3d/gpu_driven.wgsl') -Raw
$receiver = [regex]::Match($shader, '(?s)fn retained_snow_receiver\(.*?\n}').Value
if ($receiver -notmatch 'SECTION_NO_OBJECT_SNOW' -or
    ([regex]::Matches($shader, 'retained_snow_receiver\(in\)').Count -lt 2)) {
    throw 'Clock material veto is missing from shared retained colour/prepass receiver.'
}
Write-Host 'Church fixed-corner, clock veto, finite-pose and bounded-source policy checks passed.'
