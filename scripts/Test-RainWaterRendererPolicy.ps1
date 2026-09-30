[CmdletBinding()]
param([string]$RustCompiler='rustc',[string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$out=Join-Path $repo 'build/rain-water-render-policy'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$module=Get-Content (Join-Path $repo 'engine/WgpuRenderer/rust/src/rain_water.rs') -Raw
$start=$module.IndexOf('const MAX_CELLS:')
$end=$module.IndexOf('pub struct RainWater')
if ($start -lt 0 -or $end -le $start) {throw 'Actual pure renderer helper declarations missing.'}
$types="type WgrVec4=[f32;4];`n#[allow(dead_code)] #[derive(Clone,Copy)] struct WgrRainWaterParams {domain:WgrVec4,control:WgrVec4,generation:u64,reserved:u64}`n"
$tests=Get-Content (Join-Path $repo 'tests/unit/engine/weather/test_rain_water_renderer.rs') -Raw
$source=Join-Path $out 'actual_rain_water_renderer.rs'
[IO.File]::WriteAllText($source,$types+$module.Substring($start,$end-$start)+$tests)
$exe=Join-Path $out 'actual_rain_water_renderer.exe'
& $RustCompiler '--edition=2024' '--test' '-D' 'warnings' $source '-o' $exe
if ($LASTEXITCODE -ne 0) {throw 'Actual rainwater renderer helper compilation failed.'}
& $exe
if ($LASTEXITCODE -ne 0) {throw 'Actual rainwater renderer helper policy failed.'}
$header=(Join-Path $repo 'engine/WgpuRenderer/include/wgpu_renderer.hpp').Replace('\','/')
$cpp=Join-Path $out 'actual_rain_water_abi.cpp'
[IO.File]::WriteAllText($cpp,"#include `"$header`"`n#include <cstddef>`nstatic_assert(sizeof(WgrRainWaterParams)==48);`nstatic_assert(offsetof(WgrRainWaterParams,control)==16);`nstatic_assert(offsetof(WgrRainWaterParams,generation)==32);`nstatic_assert(sizeof(WgrWaterParams)==1472);`nint main() {return 0;}`n")
$cppExe=Join-Path $out 'actual_rain_water_abi.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' $cpp '-o' $cppExe
if ($LASTEXITCODE -ne 0) {throw 'Actual rainfall/ocean ABI checks failed.'}
& $cppExe
if ($LASTEXITCODE -ne 0) {throw 'Rainwater ABI execution failed.'}
$shader=Get-Content (Join-Path $repo 'engine/WgpuRenderer/rust/src/rain_water.wgsl') -Raw
foreach ($proof in @('rain_water_support_depth(water,bed','bare_surface_valid','interior_rain_coverage','ground_sky_reflection',
    'ground_puddle_ripple_normal','flow * rainwater.domain.w','water.y <= 0.001',
    'let water = mix(mix(a,c,f.x),mix(d,e,f.x),f.y)',
    'if (f.x+f.y <= 1.0)', 'bed+water.y,water.y,water.zw')) {
    if (!$shader.Contains($proof)) {throw "Production rainwater shader proof missing: $proof"}
}
if ($shader.Contains('ground_puddle_mask')) {throw 'Physical rainwater geometry must not use a procedural puddle mask.'}
$cppSource=Get-Content (Join-Path $repo 'engine/WgpuRenderer/WaterWgpu.cpp') -Raw
if (!$cppSource.Contains('field.Snapshot()') -or !$cppSource.Contains('QueueRainWater(look.enabled)')) {throw 'Simulation snapshot/independent ocean-pruning upload missing.'}
& "$PSScriptRoot/Test-RainWaterField.ps1" -Compiler $Compiler
Write-Host 'Actual rainwater CPU selection, ABI and source contracts PASS. Naga composition, actual-device pipeline/geometry and installed runtime remain pending.'
