[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'build/earth-tests';$null=New-Item -ItemType Directory -Force -Path $output
$exe=Join-Path $output 'earth_ground_detail_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $repo 'engine')) (Join-Path $repo 'tests/unit/engine/WgpuRenderer/test_earth_ground_detail.cpp') '-o' $exe
if($LASTEXITCODE-ne0){throw 'Earth ground detail CPU test compilation failed'}
& $exe
if($LASTEXITCODE-ne0){throw 'Earth ground detail CPU test failed'}
