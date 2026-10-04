[CmdletBinding()]
param([Parameter(Mandatory)][string]$DependencyDirectory,[string]$RustCompiler='rustc')
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/layered-fog-cpu';New-Item -ItemType Directory -Force -Path $out|Out-Null
$shared=Join-Path $root 'engine/WgpuRenderer/rust/src/shaders'
$composer=Get-Content -LiteralPath (Join-Path $shared 'mod.rs') -Raw
$begin=$composer.IndexOf('static EARLY_DEPTH_SUPPORTED:');$end=$composer.IndexOf('pub fn make_module(')
if($begin -lt 0 -or $end -le $begin){throw 'Actual production composer seam missing.'}
$prefix=[regex]::Replace($composer.Substring($begin,$end-$begin),'include_str!\("([^\"]+)"\)',{param($m)
 $path=(Resolve-Path -LiteralPath (Join-Path $shared $m.Groups[1].Value)).Path.Replace('\','/')
 return 'include_str!("'+$path+'")'
})
$entries=@('../layered_fog.wgsl','../layered_fog_background.wgsl','../sky/cloud_composite.wgsl','../terrain/terrain.wgsl','../gfx3d/shader3d.wgsl','../gfx3d/gpu_driven.wgsl','../grass/grass.wgsl')|ForEach-Object{
 $path=(Resolve-Path -LiteralPath (Join-Path $shared $_)).Path.Replace('\','/')
 '(include_str!("'+$path+'"), "'+$_+'")'
}
$code='use naga_oil::compose::{ComposableModuleDescriptor,Composer,NagaModuleDescriptor,ShaderLanguage};'+"`n"+$prefix+"`n"+@'
fn main(){
 for (source,file_path) in [
'@+($entries -join ",`n")+@'
 ] {
  let mut composer=build_composer();
  let module=composer.make_naga_module(NagaModuleDescriptor{source,file_path,shader_defs:shader_defs(),..Default::default()})
   .unwrap_or_else(|e|panic!("{file_path}: {}",e.emit_to_string(&composer)));
  naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::all()).validate(&module).unwrap();
  println!("Actual full production module CPU PASS: {file_path}");
 }
}
'@
$file=Join-Path $out 'layered-fog-shaders.rs';Set-Content -LiteralPath $file -Value $code -Encoding UTF8
$naga=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
$oil=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga_oil-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
if(!$naga -or !$oil){throw 'Supply matching cached naga/naga_oil dependencies.'}
$exe=Join-Path $out 'layered-fog-shaders.exe'
& $RustCompiler '--edition=2021' '-Dwarnings' '--crate-name' 'layered_fog_shaders' $file '-L' ('dependency='+$DependencyDirectory) '--extern' ('naga='+$naga.FullName) '--extern' ('naga_oil='+$oil.FullName) '-o' $exe
if($LASTEXITCODE -ne 0){throw 'Actual shader CPU composition compilation failed.'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Actual production shader validation failed.'}
