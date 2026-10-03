[CmdletBinding()]
param([string]$Compiler='clang++',[string]$RustCompiler='rustc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/tidewater-rotor-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$cppExe=Join-Path $output 'rotor_water_policy.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $root 'engine')) `
    (Join-Path $root 'tests/unit/engine/weather/test_rotor_water_policy.cpp') '-o' $cppExe
if ($LASTEXITCODE -ne 0) {throw 'Sea-relative rotor CPU policy compilation failed.'}
& $cppExe
if ($LASTEXITCODE -ne 0) {throw 'Sea-relative rotor CPU policy tests failed.'}
$rustExe=Join-Path $output 'tidewater_rotor_policy.exe'
& $RustCompiler '--edition=2024' '--test' '-Dwarnings' `
    (Join-Path $root 'engine/WgpuRenderer/rust/src/water_tw/rotor_wash.rs') '-o' $rustExe
if ($LASTEXITCODE -ne 0) {throw 'Tidewater rotor descriptor CPU compilation failed.'}
& $rustExe
if ($LASTEXITCODE -ne 0) {throw 'Tidewater rotor descriptor CPU tests failed.'}
Write-Host 'PASS rotor sea eligibility/descriptor lifecycle; combined shader/device/installed checks pending.'
