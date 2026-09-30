[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'build/rain-water-fine-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'rain_water_fine_bench.exe'
& $Compiler '-O2' '-DNDEBUG' '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/weather/bench_rain_water_fine.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Fine water benchmark compilation failed.' }
& $exe | Tee-Object -FilePath (Join-Path $output 'fine-cpu.csv')
if ($LASTEXITCODE -ne 0) { throw 'Fine water benchmark failed.' }
