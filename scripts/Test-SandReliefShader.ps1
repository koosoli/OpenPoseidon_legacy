# Compose actual sand helper and full terrain shaders on CPU, without a device.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$DependencyDirectory,[string]$RustCompiler='rustc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/sand-relief-cpu';New-Item -ItemType Directory -Force $out|Out-Null
$shared=Join-Path $root 'engine/WgpuRenderer/rust/src/shaders'
function Expand-Includes([string]$code,[string]$directory){
 return [regex]::Replace($code,'include_str!\("([^\"]+)"\)',{
  param($m)
  $path=(Resolve-Path -LiteralPath (Join-Path $directory $m.Groups[1].Value)).Path.Replace('\','/')
  return 'include_str!("'+$path+'")'
 })
}
$composer=Get-Content -LiteralPath (Join-Path $shared 'mod.rs') -Raw
$begin=$composer.IndexOf('static EARLY_DEPTH_SUPPORTED:');$end=$composer.IndexOf('pub fn make_module(')
if($begin -lt 0 -or $end -le $begin){throw 'Actual production composer seam missing.'}
$prefix=Expand-Includes $composer.Substring($begin,$end-$begin) $shared
$tests=Get-Content -LiteralPath (Join-Path $shared 'sand_geometry_tests.rs') -Raw
$testBegin=$tests.IndexOf('const CONFORM:');$testEnd=$tests.IndexOf('fn snapshot(')
if($testBegin -lt 0 -or $testEnd -le $testBegin){throw 'Actual CPU/GPU test boundary missing.'}
# Retain the actual complete CPU fixture verbatim; braces in Rust strings and
# WGSL snippets cannot safely be counted as Rust declaration boundaries.
$cpu=Expand-Includes $tests.Substring($testBegin,$testEnd-$testBegin) $shared
$cpu=$cpu.Replace('#[test]','').Replace('fn sand_geometry_actual_helpers_match_and_compose_in_real_terrain','pub fn sand_geometry_actual_helpers_match_and_compose_in_real_terrain')
$cpu=[regex]::Replace($cpu,'const (GRID|SAMPLE):[^\r\n]+','') # GPU-only constants
$code='use naga_oil::compose::{ComposableModuleDescriptor,Composer,ShaderLanguage};'+"`n"+$prefix+"`n"+
 'mod actual_sand_cpu { use naga_oil::compose::NagaModuleDescriptor;'+"`n"+$cpu+@'

}
fn main() {
 let _ = shader_defs();
 actual_sand_cpu::sand_geometry_actual_helpers_match_and_compose_in_real_terrain();
 println!("Actual Sand/Conform helpers, colour/prepass call sites and complete terrain CPU composition PASS; device/runtime pending");
}
'@
$file=Join-Path $out 'actual-sand-relief-cpu.rs';Set-Content -LiteralPath $file -Value $code -Encoding UTF8
$naga=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
$oil=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga_oil-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
if(!$naga -or !$oil){throw 'Supply a prebuilt directory with matching naga/naga_oil libraries.'}
$exe=Join-Path $out 'actual-sand-relief-cpu.exe'
& $RustCompiler '--edition=2021' '-Dwarnings' '--crate-name' 'actual_sand_relief_cpu' $file '-L' ('dependency='+$DependencyDirectory) '--extern' ('naga='+$naga.FullName) '--extern' ('naga_oil='+$oil.FullName) '-o' $exe
if($LASTEXITCODE -ne 0){throw 'Actual sand CPU composition compilation failed.'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Actual sand helper/composed shader validation failed.'}
