[CmdletBinding()]
param([Parameter(Mandatory)][string]$DependencyDirectory,[string]$RustCompiler='rustc')
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/wet-soil-fragment-cpu';New-Item -ItemType Directory -Force -Path $out|Out-Null
$shared=Join-Path $root 'engine/WgpuRenderer/rust/src/shaders'
function Expand-Includes([string]$code,[string]$directory){
 return [regex]::Replace($code,'include_str!\("([^\"]+)"\)',{param($m)
  $path=(Resolve-Path -LiteralPath (Join-Path $directory $m.Groups[1].Value)).Path.Replace('\','/')
  return 'include_str!("'+$path+'")'
 })
}
$composer=Get-Content -LiteralPath (Join-Path $shared 'mod.rs') -Raw
$begin=$composer.IndexOf('static EARLY_DEPTH_SUPPORTED:');$end=$composer.IndexOf('pub fn make_module(')
if($begin -lt 0 -or $end -le $begin){throw 'Actual production composer seam missing.'}
$prefix=Expand-Includes $composer.Substring($begin,$end-$begin) $shared
$terrain=Get-Content -LiteralPath (Join-Path $shared '../terrain/mod.rs') -Raw
function Require-Source([bool]$condition,[string]$message){if(!$condition){throw $message}}
# Source guards cover the public-control/default path that a shader-only composer
# cannot exercise. No game, worker, GPU resource or material flag is modified.
$cpp=Get-Content -LiteralPath (Join-Path $root 'engine/WgpuRenderer/EngineWgpu.cpp') -Raw
$owner=[regex]::Match($cpp,'(?s)int EngineWgpu::ControlWetSoilDiagnostic\(int mode\).*?\n\}')
Require-Source ($owner.Success -and $owner.Value.Contains('Foundation::IsMainThread()') -and $owner.Value.Contains('!_hdrEnabled') -and $owner.Value.Contains('std::strcmp(enabled, "1")') -and $owner.Value.Contains('QueueTerrainParams(_wetSoilLastTerrainParams)')) 'Owner/HDR/opt-in or complete producer input control guard changed.'
$base=Get-Content -LiteralPath (Join-Path $root 'engine/Poseidon/Graphics/Core/Engine.hpp') -Raw
Require-Source ($base.Contains('virtual int ControlWetSoilDiagnostic(int /*mode*/ = -1) { return -2; }')) 'Other renderer unavailable fallback changed.'
$producer=Get-Content -LiteralPath (Join-Path $root 'engine/WgpuRenderer/TerrainWgpu.hpp') -Raw
$grass=Get-Content -LiteralPath (Join-Path $shared '../grass/mod.rs') -Raw
Require-Source ($producer.Contains('WgrTerrainParams _params{};') -and $terrain.Contains('_pad3: 0.0,') -and $grass.Contains('_pad3: 0.0,')) 'An existing production terrain constructor no longer starts in mode zero.'
$gfx=Get-Content -LiteralPath (Join-Path $shared '../gfx3d/mod.rs') -Raw
Require-Source ($gfx.Contains("self.cameras.upload_mapping(queue, shadow_mapping);`n        if cameras.is_empty()") -or $gfx.Contains("self.cameras.upload_mapping(queue, shadow_mapping);`r`n        if cameras.is_empty()")) 'Shared mapping upload must remain unconditional before camera/shadow branches.'
$mode=[regex]::Match($terrain,'(?s)fn wet_soil_debug_mode\(mode: f32\) -> f32 \{.*?\n\}')
if(!$mode.Success){throw 'Actual production mode admission helper missing.'}
$entries=@('../terrain/terrain.wgsl','../rain_water.wgsl','../gfx3d/shader3d.wgsl','../gfx3d/gpu_driven.wgsl')|ForEach-Object{
 $path=(Resolve-Path -LiteralPath (Join-Path $shared $_)).Path.Replace('\','/')
 '(include_str!("'+$path+'"), "'+$_+'")'
}
$code='use naga_oil::compose::{ComposableModuleDescriptor,Composer,NagaModuleDescriptor,ShaderLanguage};'+"`n"+$prefix+"`n"+$mode.Value+@'

fn main() {
 for m in [0.,1.,2.,3.,4.] { assert_eq!(wet_soil_debug_mode(m),m); }
 for m in [-1.,0.5,5.,f32::NAN,f32::INFINITY] { assert_eq!(wet_soil_debug_mode(m),0.); }
 let mut terrain_layout=false;
 let mut shared_layout=false;
 for (source,file_path) in [
'@+($entries -join ",`n")+@'

 ] {
  let mut composer=build_composer();
  let module=composer.make_naga_module(NagaModuleDescriptor{source,file_path,shader_defs:shader_defs(),..Default::default()})
    .unwrap_or_else(|e|panic!("{file_path}: {}",e.emit_to_string(&composer)));
  for (_,ty) in module.types.iter() {
   if let naga::TypeInner::Struct{members,span}= &ty.inner {
    if ty.name.as_ref().is_some_and(|n|n.starts_with("TerrainParams")) {
     assert_eq!(*span,88);
     assert_eq!(members.iter().find(|m|m.name.as_deref()==Some("_pad3")).unwrap().offset,80);
     terrain_layout=true;
    }
    if ty.name.as_ref().is_some_and(|n|n.starts_with("TerrainShadowMap")) {
     assert_eq!(*span,64);
     assert_eq!(members.iter().find(|m|m.name.as_deref()==Some("pad_c")).unwrap().offset,44);
     shared_layout=true;
    }
   }
  }
 }
 assert!(terrain_layout && shared_layout);
 println!("Actual enum refusal/reset, unchanged WGSL byte layouts, complete terrain/rainwater/direct/retained composition CPU PASS; device/runtime pending");
}
'@
$file=Join-Path $out 'actual-wet-soil-fragment-cpu.rs';Set-Content -LiteralPath $file -Value $code -Encoding UTF8
$naga=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
$oil=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga_oil-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
if(!$naga -or !$oil){throw 'Supply matching existing naga/naga_oil dependencies; this check never builds Cargo or opens a device.'}
$exe=Join-Path $out 'actual-wet-soil-fragment-cpu.exe'
& $RustCompiler '--edition=2021' '-Dwarnings' '--crate-name' 'actual_wet_soil_fragment_cpu' $file '-L' ('dependency='+$DependencyDirectory) '--extern' ('naga='+$naga.FullName) '--extern' ('naga_oil='+$oil.FullName) '-o' $exe
if($LASTEXITCODE -ne 0){throw 'Actual fragment CPU composition compilation failed.'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Actual fragment helper/layout/composed shader validation failed.'}
