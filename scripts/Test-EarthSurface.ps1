param([string]$Compiler='clang-cl',[string]$VcpkgDirectory='',[string]$CacheDirectory='')
$ErrorActionPreference='Stop';$repo=Split-Path $PSScriptRoot -Parent
if(!$VcpkgDirectory){$VcpkgDirectory=Join-Path $repo 'build/win-x64-clang-rwdi/vcpkg_installed/x64-windows-clang'}
if(!(Test-Path -LiteralPath (Join-Path $VcpkgDirectory 'lib/cjson.lib'))){throw 'Pass existing configured vcpkg directory; no installation is performed'}
$output=Join-Path $repo 'build/earth-surface';New-Item -ItemType Directory -Force -Path $output|Out-Null
$exe=Join-Path $output 'earth-surface-test.exe';$savedPath=$env:PATH
Push-Location $output
try{
 & $Compiler /nologo /std:c++20 /EHsc /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING `
  ('/I'+(Join-Path $repo 'engine')) ('/I'+(Join-Path $VcpkgDirectory 'include')) ('/Fe'+$exe) `
  (Join-Path $repo 'tests/unit/engine/Poseidon/World/test_earth_surface.cpp') (Join-Path $repo 'engine/WgpuRenderer/EarthSurfaceStream.cpp') `
  (Join-Path $VcpkgDirectory 'lib/cjson.lib')
 if($LASTEXITCODE-ne0){throw 'Earth surface CPU compilation failed'}
 $env:PATH=(Join-Path $VcpkgDirectory 'bin')+';'+$env:PATH
 if($CacheDirectory){& $exe $CacheDirectory}else{& $exe}
 if($LASTEXITCODE-ne0){throw 'Earth surface CPU test failed'}
}finally{$env:PATH=$savedPath;Pop-Location}
