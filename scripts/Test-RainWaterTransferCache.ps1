[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/rain-water-transfer-cache'
New-Item -ItemType Directory -Force -Path $output|Out-Null
# Generate a second actual class from the audited pre-change source. Only the
# class/singleton identifiers change; no hand-written substitute solver.
$reference=(git -C $root show 'e017e028812dcd7ce050497fd40bf81253a1bcf1:engine/Poseidon/World/Weather/RainWaterField.hpp') -join "`n"
if($LASTEXITCODE -ne 0){throw 'Pinned actual coarse reference is unavailable.'}
$reference+="`n"
$hash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($reference)))
if($hash -cne '4BB54A8046F2D7E7C287EDC68F75F5F4582E097C744791CCBE188B43B77216A7'){throw 'Pinned actual coarse reference changed.'}
$reference=$reference.Replace('RainWaterField','RainWaterFieldReference').Replace('GRainWater()','GRainWaterReference()')
[IO.File]::WriteAllText((Join-Path $output 'RainWaterFieldReference.hpp'),$reference,[Text.UTF8Encoding]::new($false))
$exe=Join-Path $output 'rain_water_transfer_cache.exe'
foreach($optimization in @('-O0','-O2')){
 & $Compiler '-std=c++20' $optimization '-Wall' '-Wextra' '-Werror' ('-I'+$output) `
  ('-I'+(Join-Path $root 'engine/Poseidon/World/Weather')) `
  (Join-Path $root 'tests/unit/engine/weather/test_rain_water_transfer_cache.cpp') '-o' $exe
 if($LASTEXITCODE -ne 0){throw 'Actual coarse transfer reference compilation failed.'}
 & $exe
 if($LASTEXITCODE -ne 0){throw 'Actual coarse transfer equivalence failed.'}
}
