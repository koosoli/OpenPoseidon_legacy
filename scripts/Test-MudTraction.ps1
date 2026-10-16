[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/mud-traction-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'mud_traction_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I' + (Join-Path $repo 'engine')) (Join-Path $repo 'tests/unit/engine/Poseidon/World/Weather/test_mud_traction.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Mud traction policy compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Mud traction policy assertions failed.' }
Write-Host 'Mud traction grip, wheel spin, digging and spray policy tests passed.'
