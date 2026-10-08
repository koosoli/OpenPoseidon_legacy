<# Build our optional add-on against an already cached pinned API. No downloads. #>
[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$RuntimeDirectory,
 [string]$BuildDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/reshade-bridge')
)
$ErrorActionPreference='Stop'
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Close the game before replacing the optional add-on.'}
$repo=Split-Path $PSScriptRoot -Parent
$RuntimeDirectory=[IO.Path]::GetFullPath($RuntimeDirectory)
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
if(!(Test-Path -LiteralPath (Join-Path $RuntimeDirectory 'api/reshade.hpp') -PathType Leaf)){throw 'Cached official API required; no downloads performed.'}
$build = $BuildDirectory
$configure = @('-S', (Join-Path $repo 'tools/reshade-bridge'), '-B', $build,
    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DRESHADE_API_DIR=$RuntimeDirectory/api")
$clang = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
if ($clang) {
    $configure += "-DCMAKE_CXX_COMPILER=$($clang.Source.Replace('\','/'))"
    $llvmBin = (Split-Path $clang.Source -Parent).Replace('\','/')
    $configure += "-DCMAKE_LINKER=$llvmBin/lld-link.exe"
    $configure += "-DCMAKE_RC_COMPILER=$llvmBin/llvm-rc.exe"
}
$kitsRoot = (Get-ItemProperty -Path 'HKLM:/SOFTWARE/Microsoft/Windows Kits/Installed Roots' `
    -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
if ($kitsRoot) {
    $sdk = Get-ChildItem (Join-Path $kitsRoot 'bin') -Directory |
        Where-Object { $_.Name -match '^10\.' -and (Test-Path (Join-Path $_.FullName 'x64/mt.exe')) } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if ($sdk) { $configure += "-DCMAKE_MT=$($sdk.FullName.Replace('\','/'))/x64/mt.exe" }
}
cmake --fresh @configure
if ($LASTEXITCODE -ne 0) { throw 'ReShade add-on configure failed.' }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'ReShade add-on build failed.' }
Copy-Item -LiteralPath (Join-Path $build 'OpenPoseidonReShade.addon64') -Destination $RuntimeDirectory
