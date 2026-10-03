[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/rain-particle-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'rain_particle_policy.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $root 'engine')) `
    (Join-Path $root 'tests/unit/engine/weather/test_rain_particle_policy.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) {throw 'Rain population policy compilation failed.'}
& $exe
if ($LASTEXITCODE -ne 0) {throw 'Rain population policy tests failed.'}
