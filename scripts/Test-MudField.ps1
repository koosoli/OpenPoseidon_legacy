[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/mud-field-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'mud_field_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I' + (Join-Path $repo 'engine')) (Join-Path $repo 'tests/unit/engine/Poseidon/World/Weather/test_mud_field.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Mud field policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Mud field policy assertions failed.' }
Write-Host 'Mud field wetness, signed snapshot/gradient parity, persistent bounded geometry and stock soil policy tests passed.'
