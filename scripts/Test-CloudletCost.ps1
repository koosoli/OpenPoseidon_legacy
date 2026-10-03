[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/cloudlet-cost'
New-Item -ItemType Directory -Force -Path $output|Out-Null
$exe=Join-Path $output 'cloudlet_cost.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' '-D_CRT_SECURE_NO_WARNINGS' ('-I'+(Join-Path $root 'engine')) `
 (Join-Path $root 'tests/unit/engine/weather/test_cloudlet_cost.cpp') `
 (Join-Path $root 'engine/Poseidon/World/SimVehicleCost.cpp') '-o' $exe
if($LASTEXITCODE -ne 0){throw 'Cloudlet cost source test compilation failed.'}
$saved=[Environment]::GetEnvironmentVariable('POSEIDON_SIM_VEHICLE_COST','Process')
try{
 foreach($value in @('0','1')){
  [Environment]::SetEnvironmentVariable('POSEIDON_SIM_VEHICLE_COST',$value,'Process')
  & $exe $value
  if($LASTEXITCODE -ne 0){throw 'Cloudlet cost accounting/gate test failed.'}
 }
}finally{[Environment]::SetEnvironmentVariable('POSEIDON_SIM_VEHICLE_COST',$saved,'Process')}
