[CmdletBinding()]
param([Parameter(Mandatory)][string]$DependencyDirectory,[string]$RustCompiler='rustc')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $root 'build/earth-water-coverage'
$null=New-Item -ItemType Directory -Force -Path $out
$source=Join-Path $root 'engine/WgpuRenderer/rust/src/water_tw/cdlod.rs'
$glam=Get-ChildItem -LiteralPath $DependencyDirectory -Filter 'libglam-*.rlib'|Sort-Object LastWriteTime -Descending|Select-Object -First 1
if(!$glam){throw 'Supply existing matching host glam dependency library'}
$exe=Join-Path $out 'actual-earth-water-coverage.exe'
# Compile/run the actual production CPU selector and its tests. No device,
# installed game, shader substitution or Cargo/full renderer build is involved.
& $RustCompiler '--edition=2021' '-Dwarnings' '--test' '--crate-name' 'actual_earth_water_coverage' $source '-L' ('dependency='+$DependencyDirectory) '--extern' ('glam='+$glam.FullName) '-o' $exe
if($LASTEXITCODE-ne0){throw 'Actual Earth water coverage compilation failed'}
& $exe
if($LASTEXITCODE-ne0){throw 'Actual Earth water coverage tests failed'}
