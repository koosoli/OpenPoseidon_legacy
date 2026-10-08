param([string]$Compiler='clang-cl',[string]$VcpkgDirectory='',[string]$CacheDirectory='',[switch]$Optimized)
$ErrorActionPreference='Stop';$repo=Split-Path $PSScriptRoot -Parent
if(!$VcpkgDirectory){$VcpkgDirectory=Join-Path $repo 'build/win-x64-clang-rwdi/vcpkg_installed/x64-windows-clang'}
if(!(Test-Path -LiteralPath (Join-Path $VcpkgDirectory 'lib/cjson.lib'))){throw 'Pass existing configured vcpkg directory; no installation is performed'}
$output=Join-Path $repo 'build/earth-surface';New-Item -ItemType Directory -Force -Path $output|Out-Null
$suffix=if($Optimized){'optimized'}else{'debug'}
$exe=Join-Path $output ('earth-surface-test-'+$suffix+'.exe');$savedPath=$env:PATH
# Match the parent's RelWithDebInfo optimization and explicit-inline policy.
$optimizationArgs=@();if($Optimized){$optimizationArgs+=@('/O2','/Ob1')}
$cacheHashes=@{}
if($CacheDirectory){
 foreach($relative in @('blue-marble-june-5400.jpg','surface/lux-context-2023.jpg','surface/lux-context-2023.json','surface/lux-district-2023.jpg','surface/lux-district-2023.json','surface/lux-ortho-2023.jpg','surface/lux-ortho-2023.json')){
  $path=Join-Path $CacheDirectory $relative;if(Test-Path -LiteralPath $path){$cacheHashes[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
 }
}
Push-Location $output
try{
 & $Compiler @optimizationArgs /nologo /std:c++20 /EHsc /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING `
  ('/I'+(Join-Path $repo 'engine')) ('/I'+(Join-Path $VcpkgDirectory 'include')) ('/Fe'+$exe) `
  (Join-Path $repo 'tests/unit/engine/Poseidon/World/test_earth_surface.cpp') (Join-Path $repo 'engine/WgpuRenderer/EarthSurfaceStream.cpp') `
  (Join-Path $VcpkgDirectory 'lib/cjson.lib')
 if($LASTEXITCODE-ne0){throw 'Earth surface CPU compilation failed'}
 $env:PATH=(Join-Path $VcpkgDirectory 'bin')+';'+$env:PATH
 if($CacheDirectory){$testOutput=@(& $exe $CacheDirectory)}else{$testOutput=@(& $exe)}
 $testExit=$LASTEXITCODE;$testOutput|Write-Output
 $testOutput|Set-Content -LiteralPath (Join-Path $output ($suffix+'.log'))
 if($testExit-ne0){throw 'Earth surface CPU test failed'}
 foreach($path in $cacheHashes.Keys){if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash-ne$cacheHashes[$path]){throw ('CPU test changed cached input: '+$path)}}
 $timing=@($testOutput|Where-Object {$_-like'EARTH_SURFACE_TIMING *'})
 $receipt=@{passed=$true;gpuDeviceCreated=$false;optimized=[bool]$Optimized;optimizationArguments=$optimizationArgs;compiler=$Compiler;cacheDirectory=$CacheDirectory;utc=[DateTime]::UtcNow.ToString('o');cacheInputsUnchanged=$true;cacheInputHashes=$cacheHashes;timing=$null}
 if($timing.Count){$receipt.timing=$timing[-1].Substring('EARTH_SURFACE_TIMING '.Length)|ConvertFrom-Json}
 $receipt|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output ($suffix+'-result.json'))
}finally{$env:PATH=$savedPath;Pop-Location}
