param(
 [string]$Compiler='clang-cl',
 [string]$VcpkgDirectory='',
 [string]$CellFile=''
)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(!$VcpkgDirectory){$VcpkgDirectory=Join-Path $repo 'build/win-x64-clang-rwdi/vcpkg_installed/x64-windows-clang'}
if(!(Test-Path -LiteralPath (Join-Path $VcpkgDirectory 'lib/cjson.lib'))){throw 'Pass the existing configured build vcpkg directory; no dependency installation is performed'}
$output=Join-Path $repo 'build/earth-buildings';New-Item -ItemType Directory -Force -Path $output|Out-Null
$exe=Join-Path $output 'earth-buildings-test.exe';$savedPath=$env:PATH
Push-Location $output
try{
 & $Compiler /nologo /std:c++20 /EHsc /MD /D_CRT_SECURE_NO_WARNINGS ('/I'+(Join-Path $repo 'engine')) ('/I'+(Join-Path $VcpkgDirectory 'include')) `
  ('/Fe'+$exe) (Join-Path $repo 'tests/unit/engine/Poseidon/World/test_earth_buildings.cpp') (Join-Path $repo 'engine/WgpuRenderer/EarthBuildingStream.cpp') (Join-Path $VcpkgDirectory 'lib/cjson.lib')
 if($LASTEXITCODE-ne0){throw 'Earth building CPU test compilation failed'}
 $env:PATH=(Join-Path $VcpkgDirectory 'bin')+';'+$env:PATH
 if($CellFile){& $exe $CellFile}else{& $exe}
 if($LASTEXITCODE-ne0){throw 'Earth building CPU test failed'}
}finally{$env:PATH=$savedPath;Pop-Location}
