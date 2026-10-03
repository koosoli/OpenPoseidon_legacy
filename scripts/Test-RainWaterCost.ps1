[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/rain-water-cost'
New-Item -ItemType Directory -Force -Path $output|Out-Null
$saved=$env:POSEIDON_RAIN_WATER_COST
try {
 foreach($opt in @('-O0','-O2')) {
  $exe=Join-Path $output 'rain-water-cost.exe'
  & $Compiler '-std=c++20' $opt '-Wall' '-Wextra' '-Werror' (Join-Path $root 'tests/unit/engine/weather/test_rain_water_cost.cpp') '-o' $exe
  if($LASTEXITCODE -ne 0){throw 'Actual coarse timing test compilation failed.'}
  foreach($flag in @('0','1','not-enabled')) {
   $env:POSEIDON_RAIN_WATER_COST=$flag
   & $exe
   if($LASTEXITCODE -ne 0){throw 'Actual coarse accounting test failed.'}
  }
  # Existing pinned actual reference compares all kernel outputs bitwise and
  # observes no per-Advance C++ allocations under both diagnostic settings.
 }
 foreach($flag in @('0','1')) {
  $env:POSEIDON_RAIN_WATER_COST=$flag
  & (Join-Path $PSScriptRoot 'Test-RainWaterTransferCache.ps1') -Compiler $Compiler
 }
} finally { $env:POSEIDON_RAIN_WATER_COST=$saved }
