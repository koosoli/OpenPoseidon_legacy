[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'build/earth-tests';$null=New-Item -ItemType Directory -Force -Path $output
$exe=Join-Path $output 'earth_road_coverage_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_earth_road_coverage.cpp') '-o' $exe
if($LASTEXITCODE-ne0){throw 'Earth road coverage policy compilation failed'}
& $exe
if($LASTEXITCODE-ne0){throw 'Earth road coverage policy test failed'}
