[CmdletBinding()]
param([Parameter(Mandatory)][string]$DependencyDirectory,[string]$RustCompiler='rustc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/earth-water-shader';$null=New-Item -ItemType Directory -Force -Path $out
$shared=Join-Path $root 'engine/WgpuRenderer/rust/src/shaders'
$water=Join-Path $root 'engine/WgpuRenderer/rust/src/water_tw'
function Expand-Includes([string]$code,[string]$directory){
 return [regex]::Replace($code,'include_str!\("([^\"]+)"\)',{param($m)
  $path=(Resolve-Path -LiteralPath (Join-Path $directory $m.Groups[1].Value)).Path.Replace('\','/')
  return 'include_str!("'+$path+'")'
 })
}
# Compile the actual startup composer and concatenated water/kernel declarations,
# without constructing a renderer/device or touching the installed game.
$composer=Get-Content -LiteralPath (Join-Path $shared 'mod.rs') -Raw
$begin=$composer.IndexOf('static EARLY_DEPTH_SUPPORTED:');$end=$composer.IndexOf('pub fn make_module(')
if($begin-lt0 -or $end-le$begin){throw 'Actual production composer seam missing'}
$prefix=Expand-Includes $composer.Substring($begin,$end-$begin) $shared
$production=Get-Content -LiteralPath (Join-Path $water 'mod.rs') -Raw
$declarations=@('TW_WATER_WGSL','TW_WAKE_KERNEL_WGSL','TW_BRK_KERNEL_WGSL','TW_SIM_KERNEL_WGSL')|ForEach-Object{
 $match=[regex]::Match($production,'(?s)const '+$_+': &str = concat!\(.*?\);')
 if(!$match.Success){throw ('Actual production shader declaration missing: '+$_)}
 Expand-Includes $match.Value $water
}
$code='use naga_oil::compose::{ComposableModuleDescriptor,Composer,NagaModuleDescriptor,ShaderLanguage};'+"`n"+$prefix+"`n"+($declarations-join"`n")+@'

fn main() {
 for (source,file_path) in [(TW_WATER_WGSL,"water_tw/tw_water.wgsl"),
     (TW_WAKE_KERNEL_WGSL,"water_tw/tw_wake_kernel.wgsl"),
     (TW_BRK_KERNEL_WGSL,"water_tw/tw_breakers_kernel.wgsl"),
     (TW_SIM_KERNEL_WGSL,"water_tw/tw_sim_kernel.wgsl")] {
  let mut composer=build_composer();
  composer.make_naga_module(NagaModuleDescriptor{source,file_path,shader_defs:shader_defs(),..Default::default()})
    .unwrap_or_else(|e|panic!("{file_path}: {}",e.emit_to_string(&composer)));
 }
 println!("Actual complete Tidewater material/wake/breaker/shore shader composition CPU PASS; runtime pixels pending");
}
'@
$file=Join-Path $out 'actual-earth-water-shader.rs';Set-Content -LiteralPath $file -Value $code -Encoding UTF8
$naga=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
$oil=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga_oil-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
if(!$naga -or !$oil){throw 'Supply matching prebuilt naga/naga_oil dependency libraries'}
$exe=Join-Path $out 'actual-earth-water-shader.exe'
& $RustCompiler '--edition=2021' '-Dwarnings' '--crate-name' 'actual_earth_water_shader' $file '-L' ('dependency='+$DependencyDirectory) '--extern' ('naga='+$naga.FullName) '--extern' ('naga_oil='+$oil.FullName) '-o' $exe
if($LASTEXITCODE-ne0){throw 'Actual Earth water composer compilation failed'}
& $exe
if($LASTEXITCODE-ne0){throw 'Actual complete Earth water shader validation failed'}
