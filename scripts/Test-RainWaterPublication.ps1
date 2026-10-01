[CmdletBinding()]
param([string]$Compiler='clang++',[string]$RustCompiler='rustc')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'build/rain-water-publication-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'rain_water_publication_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' (Join-Path $repo 'tests/unit/engine/weather/test_rain_water_publication.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) { throw 'Rain water publication compilation failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Rain water publication assertions failed.' }
$ffi=Get-Content (Join-Path $repo 'engine/WgpuRenderer/rust/src/ffi.rs') -Raw
$definitions=@('pub type WgrVec4 = [f32; 4];')
foreach($name in @('WgrRainWaterParams','WgrRainWaterSourceKey','WgrRainWaterFineCell','WgrRainWaterPublication')) {
    $definition=[regex]::Match($ffi,"(?s)#\[repr\(C\)\]\s*#\[derive\([^)]*\)\]\s*pub struct $name\s*\{[^}]*\}").Value
    if(!$definition){throw "Missing actual Rust ABI definition $name"}
    # Only remove the external proc-macro derive; the actual repr/fields remain.
    $definitions+=[regex]::Replace($definition,'#\[derive\([^)]*\)\]','')
    $assertion=[regex]::Match($ffi,"(?s)const _: \(\) = assert!\(std::mem::size_of::<$name>.*?\);").Value
    if(!$assertion){throw "Missing actual Rust ABI layout assertion $name"}
    $definitions+=$assertion
}
$definitions+='fn main() { println!("Actual Rust FFI rainfall definitions and layout assertions passed."); }'
$rustSource=Join-Path $output 'rain_water_ffi_layout.rs'
[IO.File]::WriteAllText($rustSource,($definitions -join "`n"),[Text.UTF8Encoding]::new($false))
$rustExe=Join-Path $output 'rain_water_ffi_layout.exe'
& $RustCompiler '--edition=2024' '-Dwarnings' $rustSource '-o' $rustExe
if($LASTEXITCODE -ne 0){throw 'Actual Rust FFI ABI layout compilation failed.'}
& $rustExe
if($LASTEXITCODE -ne 0){throw 'Actual Rust FFI ABI layout check failed.'}
