[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'build/earth-tests'
$null=New-Item -ItemType Directory -Force -Path $output
$exe=Join-Path $output 'earth_coordinates_test.exe'
& $Compiler '-std=c++20' '-D_CRT_SECURE_NO_WARNINGS' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $repo 'engine')) (Join-Path $repo 'tests/unit/engine/Poseidon/World/test_earth_coordinates.cpp') '-o' $exe
if($LASTEXITCODE -ne 0){throw 'Earth coordinate test compilation failed.'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Earth coordinate tests failed.'}
