[CmdletBinding()]
param([Parameter(Mandatory)][string]$DependencyDirectory,[string]$RustCompiler='rustc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/earth-sky-horizon';$null=New-Item -ItemType Directory -Force -Path $out
$shared=Join-Path $root 'engine/WgpuRenderer/rust/src/shaders'
$skyPath=Join-Path $root 'engine/WgpuRenderer/rust/src/sky/sky.wgsl'
$sky=Get-Content -LiteralPath $skyPath -Raw
# Evaluate the actual production interval predicate (the WGSL scalar boolean
# expression is valid Rust after mapping its vec2 components to array indices).
$predicate=[regex]::Match($sky,'(?s)fn earth_forward_sphere_hit\(interval: vec2<f32>\) -> bool \{\s*return ([^;]+);\s*\}')
if(!$predicate.Success){throw 'Actual Earth sphere interval predicate missing'}
$expression=$predicate.Groups[1].Value.Replace('interval.x','interval[0]').Replace('interval.y','interval[1]')
if(!$sky.Contains('min(atmo.y, ground.x), earth_forward_sphere_hit(ground)') -or
   !$sky.Contains('earth_preview() && !earth_forward_sphere_hit(ground)')){throw 'Earth atmosphere/disc paths must both use the actual tested interval predicate'}
$composer=Get-Content -LiteralPath (Join-Path $shared 'mod.rs') -Raw
$begin=$composer.IndexOf('static EARLY_DEPTH_SUPPORTED:');$end=$composer.IndexOf('pub fn make_module(')
if($begin-lt0 -or $end-le$begin){throw 'Actual production composer seam missing'}
$prefix=[regex]::Replace($composer.Substring($begin,$end-$begin),'include_str!\("([^\"]+)"\)',{param($m)
 $path=(Resolve-Path -LiteralPath (Join-Path $shared $m.Groups[1].Value)).Path.Replace('\','/')
 return 'include_str!("'+$path+'")'
})
$code='use naga_oil::compose::{ComposableModuleDescriptor,Composer,NagaModuleDescriptor,ShaderLanguage};'+"`n"+$prefix+"`nfn main(){`nlet hit=|interval:[f32;2]| -> bool {"+$expression+'};'+@'

 // Contract intervals: the real miss sentinel, behind-camera roots, forward
 // surface hits and a positive tangent. No copied intersection implementation.
 for (interval,expected) in [([1.0,-1.0],false),([-20.0,-10.0],false),
     ([-5.0,10.0],false),([1000.0,12757274.0],true),([113000.0,113000.0],true)] {
  assert_eq!(hit(interval),expected,"interval {interval:?}");
 }
 for altitude in [1000.0f64,9000.0] {
  let radius=6378137.0f64;
  let angle=(radius/(radius+altitude)).acos().to_degrees();
  assert!((altitude==1000.0 && angle>1.0 && angle<1.1) || (altitude==9000.0 && angle>3.0 && angle<3.1));
  // A horizontal ray has closest radius R+h > R: independently a real miss.
  // Old positive-first-value logic truncated it to one metre; new logic keeps
  // the entire available atmosphere interval (same for the sun-disc gate).
  let ground=[1.0f32,-1.0];let atmosphere_exit=1_000_000.0f32;
  let old=if ground[0]>0.0 {ground[0]} else {atmosphere_exit};
  let corrected=if hit(ground) {ground[0]} else {atmosphere_exit};
  assert_eq!(old,1.0);assert_eq!(corrected,atmosphere_exit);
  println!("Earth grazing miss at {altitude} m: horizon half-angle {angle:.4} deg, old march {old} m, corrected {corrected} m");
 }
 let mut composer=build_composer();
'@+"`nlet source=include_str!(`""+$skyPath.Replace('\','/')+"`");`n"+@'
 let module=composer.make_naga_module(NagaModuleDescriptor{source,file_path:"sky/sky.wgsl",shader_defs:shader_defs(),..Default::default()})
  .unwrap_or_else(|e|panic!("{}",e.emit_to_string(&composer)));
 naga::valid::Validator::new(naga::valid::ValidationFlags::all(),naga::valid::Capabilities::all()).validate(&module).unwrap();
 println!("Actual Earth hit/miss predicate, both consumers and complete production sky composition CPU PASS; runtime pixels pending");
}
'@
$file=Join-Path $out 'actual-earth-sky-horizon.rs';Set-Content -LiteralPath $file -Value $code -Encoding UTF8
$naga=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
$oil=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libnaga_oil-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
if(!$naga -or !$oil){throw 'Supply matching existing host naga/naga_oil libraries'}
$exe=Join-Path $out 'actual-earth-sky-horizon.exe'
& $RustCompiler '--edition=2021' '-Dwarnings' '--crate-name' 'actual_earth_sky_horizon' $file '-L' ('dependency='+$DependencyDirectory) '--extern' ('naga='+$naga.FullName) '--extern' ('naga_oil='+$oil.FullName) '-o' $exe
if($LASTEXITCODE-ne0){throw 'Actual Earth sky predicate/composer compilation failed'}
& $exe
if($LASTEXITCODE-ne0){throw 'Actual Earth sky predicate/full composition failed'}
