<#
.SYNOPSIS
    Explicitly download/prepare official ReShade 6.8.0 locally and build our add-on.
    Never runs the installer or installs a global Vulkan layer. Needs Python 3,
    curl, CMake, Ninja and a Windows x64 C++ toolchain (same as the engine build).
#>
[CmdletBinding()]
param(
    [string]$RuntimeDirectory = (Join-Path $env:LOCALAPPDATA 'OpenPoseidon/reshade/6.8.0'),
    [string]$InstallerPath = '',
    [string]$SourceArchivePath = ''
)
$ErrorActionPreference = 'Stop'
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) {
    throw 'Close the game before updating the optional ReShade runtime.'
}
$repo = Split-Path $PSScriptRoot -Parent
$argsPy = @((Join-Path $PSScriptRoot 'Prepare-ReShade.py'), $RuntimeDirectory)
if ($InstallerPath) { $argsPy += @('--installer', $InstallerPath) }
if ($SourceArchivePath) { $argsPy += @('--source-archive', $SourceArchivePath) }
python @argsPy
if ($LASTEXITCODE -ne 0) { throw 'Official ReShade preparation failed.' }
$build = Join-Path $repo 'build/reshade-bridge'
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
Write-Host "Prepared opt-in ReShade preview: $RuntimeDirectory"
