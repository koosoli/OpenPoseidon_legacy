param([string]$Compiler='clang-cl',[string]$VcpkgDirectory='')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(!$VcpkgDirectory){$VcpkgDirectory=Join-Path $repo 'build/win-x64-clang-rwdi/vcpkg_installed/x64-windows-clang'}
$output=Join-Path $repo 'build/earth-content'
$null=New-Item -ItemType Directory -Force -Path $output
$exe=Join-Path $output 'earth-content-test.exe'
$savedPath=$env:PATH
Push-Location $output
try {
 & $Compiler /nologo /std:c++20 /EHsc /MD /D_CRT_SECURE_NO_WARNINGS ('/I'+(Join-Path $repo 'engine')) ('/I'+(Join-Path $VcpkgDirectory 'include')) `
  ('/Fe'+$exe) (Join-Path $repo 'tests/unit/engine/Poseidon/World/test_earth_content.cpp') (Join-Path $repo 'engine/WgpuRenderer/EarthContentSource.cpp') `
  (Join-Path $VcpkgDirectory 'lib/cjson.lib') (Join-Path $VcpkgDirectory 'lib/libcurl.lib') (Join-Path $VcpkgDirectory 'lib/zs.lib') bcrypt.lib advapi32.lib crypt32.lib secur32.lib ws2_32.lib iphlpapi.lib
 if($LASTEXITCODE-ne0){throw 'Earth content CPU test compilation failed'}
 $env:PATH=(Join-Path $VcpkgDirectory 'bin')+';'+$env:PATH
 & python (Join-Path $PSScriptRoot 'test_earth_content_http.py') $exe
 if($LASTEXITCODE-ne0){throw 'Earth content HTTP tests failed'}
} finally {$env:PATH=$savedPath;Pop-Location}
