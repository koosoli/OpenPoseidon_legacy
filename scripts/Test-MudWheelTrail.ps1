[CmdletBinding()]
param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/mud-wheel-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'mud_wheel_trail_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I' + (Join-Path $repo 'engine')) (Join-Path $repo 'tests/unit/engine/Poseidon/World/Weather/test_mud_wheel_trail.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Mud wheel trail compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Mud wheel trail assertions failed.' }
$policyExe = Join-Path $output 'mud_vehicle_prototype_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I' + (Join-Path $repo 'engine')) (Join-Path $repo 'tests/unit/engine/Poseidon/World/Weather/test_mud_vehicle_prototype.cpp') '-o' $policyExe
if ($LASTEXITCODE -ne 0) { throw 'Mud vehicle policy compilation failed.' }
& $policyExe
if ($LASTEXITCODE -ne 0) { throw 'Mud vehicle policy assertions failed.' }
Write-Host 'Mud wheel distance sampling, frame independence, stationary/airborne/teleport and refused-gap tests passed.'
Write-Host 'Vehicle opt-in/SP isolation and bounded dissipative rolling resistance tests passed.'
