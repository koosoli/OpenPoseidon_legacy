<#
.SYNOPSIS
    Build the shareable OpenPoseidon package: exactly three things to hand a friend who has
    ARMA: Cold War Assault installed -- OpenPoseidon.exe, wgpu_renderer.dll and an assets folder.

.DESCRIPTION
    Everything the game needs beyond the retail install and the two binaries goes into assets\:
      * the authored content the build stages (menu scene, logo, inventory icons, grass plates),
      * the default content packs (assets\Mods\@OP_*),
      * what Install.ps1 otherwise copies from the free Demo (BIN, fonts, dtaExt, cwr_logo.pbo),
      * the DLSS runtime (nvngx_dlss.dll) when it is at hand,
      * the Island Editor (PoseidonIslandEditor.exe, started from the main menu),
      * the licence and credits files, and a README.
    The engine looks in assets\ for each of these when the game folder does not have them
    (QIFStreamB assets overlay, assets\AddOns banks, assets\Mods default packs, NGX snippet path),
    so the friend copies the three items into the game folder and nothing else.

    The package is written to dist\share\OpenPoseidon-<date>-<commit>\ and zipped beside it.
    Run after Build.ps1; the binaries are taken from dist\, checked against the build tree the
    same way Deploy.ps1 checks them.

.PARAMETER Preset
    Build preset whose dist\ folder is packaged (default win-x64-clang-rwdi).
.PARAMETER NoZip
    Leave the folder unzipped.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'win-x64-clang-rwdi',
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\CwaCommon.ps1"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$suffix   = ($Preset -split '-')[-1]
$distDir  = Join-Path $repoRoot "dist\x64-win-$suffix"
$buildDir = Join-Path $repoRoot "build\$Preset"

foreach ($name in @('OpenPoseidon.exe', 'wgpu_renderer.dll'))
{
    if (-not (Test-Path -LiteralPath (Join-Path $distDir $name))) {
        throw "Not built: $(Join-Path $distDir $name). Run .\scripts\Build.ps1 -Preset $Preset first."
    }
}
# dist staging fails silently while a game holds the DLL (see Deploy.ps1): refuse a stale pair.
$pairs = @{
    'OpenPoseidon.exe'  = Join-Path $buildDir 'apps\cwr\Game\OpenPoseidon.exe'
    'wgpu_renderer.dll' = Join-Path $buildDir 'engine\WgpuRenderer\wgpu_renderer.dll'
}
foreach ($name in $pairs.Keys)
{
    $built = $pairs[$name]; $staged = Join-Path $distDir $name
    if ((Test-Path -LiteralPath $built) -and
        (Get-Item -LiteralPath $built).LastWriteTime -gt (Get-Item -LiteralPath $staged).LastWriteTime.AddSeconds(2)) {
        throw "$name in dist\ is older than the build tree's: staging failed (a game held the file?). Rebuild first."
    }
}

$commit = (& git -C $repoRoot rev-parse --short HEAD).Trim()
$branch = (& git -C $repoRoot rev-parse --abbrev-ref HEAD).Trim()
$dirty  = @(& git -C $repoRoot status --porcelain --untracked-files=no 2>$null | Where-Object { $_ -notmatch 'Cargo\.lock$' })
$tag    = if ($dirty.Count -gt 0) { "$commit+dirty" } else { $commit }
$name   = "OpenPoseidon-$(Get-Date -Format yyyyMMdd)-$tag"
$out    = Join-Path $repoRoot "dist\share\$name"
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
$assets = Join-Path $out 'assets'
New-Item -ItemType Directory -Force $assets | Out-Null

function Copy-Into([string]$Source, [string]$Relative)
{
    $dest = Join-Path $assets $Relative
    New-Item -ItemType Directory -Force (Split-Path $dest -Parent) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $dest -Force
}
function Copy-Tree([string]$Source, [string]$Relative)
{
    if (-not (Test-Path -LiteralPath $Source)) { return 0 }
    $n = 0
    foreach ($file in Get-ChildItem -LiteralPath $Source -Recurse -File) {
        $rest = $file.FullName.Substring($Source.Length).TrimStart('\', '/')
        Copy-Into $file.FullName $(if ($Relative) { [IO.Path]::Combine($Relative, $rest) } else { $rest })
        $n++
    }
    return $n
}

# 1. the two binaries, at the top
Copy-Item -LiteralPath (Join-Path $distDir 'OpenPoseidon.exe')  -Destination $out
Copy-Item -LiteralPath (Join-Path $distDir 'wgpu_renderer.dll') -Destination $out
Write-Host "  + OpenPoseidon.exe, wgpu_renderer.dll"

# 2. the authored content the build stages into dist\assets
$n = Copy-Tree (Join-Path $distDir 'assets') ''
Write-Host "  + assets\ (built content)            $n files"

# 2b. assets the installed game folder has and the build does not stage (the grass plates under
#     assets\grass are hand-made files the renderer reads from there; see TerrainWgpu.cpp)
$retail = Get-CwaRetailDir
if ($retail -and (Test-Path -LiteralPath (Join-Path $retail 'assets'))) {
    $n = 0
    $installed = Join-Path $retail 'assets'
    foreach ($file in Get-ChildItem -LiteralPath $installed -Recurse -File) {
        $rest = $file.FullName.Substring($installed.Length).TrimStart('\', '/')
        # The clump plate is local scratch with no recorded origin (docs/assets/source-registry.yaml:
        # "never shippable"); the blade layers are registered CC0 and come with the build anyway.
        if ($rest -like 'grass\meadow-grass-clump-*' -or $rest -like 'grass\more_grass\*') { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $assets $rest))) { Copy-Into $file.FullName $rest; $n++ }
    }
    Write-Host "  + assets\ (from the installed game, not staged by the build)  $n files"
}

# 3. the default content packs, as assets\Mods\@OP_* -- the build stages them there (step 2 copied
#    them); an older dist\Mods staging is still picked up.
$n = @(Get-ChildItem -LiteralPath (Join-Path $assets 'Mods') -Recurse -File -ErrorAction SilentlyContinue).Count
foreach ($pack in Get-ChildItem -LiteralPath (Join-Path $distDir 'Mods') -Directory -ErrorAction SilentlyContinue) {
    if (-not (Test-Path -LiteralPath (Join-Path $pack.FullName 'mod.json'))) { continue }
    if (Test-Path -LiteralPath (Join-Path $assets ('Mods\' + $pack.Name))) { continue }
    $n += Copy-Tree $pack.FullName (Join-Path 'Mods' $pack.Name)
}
Write-Host "  + assets\Mods (default packs)        $n files"
if ($n -eq 0) {
    Write-Warning "No default content packs in $distDir\Mods: run .\scripts\Build.ps1 (it stages them via Build-DefaultContent.ps1), or the friend plays without the ground materials, jeep actions and swimming."
}

# 4. what Install.ps1 copies from the Demo
$demo = Get-CwaDemoDir
if ($demo) {
    $n = 0
    foreach ($rel in @('fonts', 'dtaExt', 'BIN')) { $n += Copy-Tree (Join-Path $demo $rel) $rel }
    $logo = Join-Path $demo 'AddOns\cwr_logo.pbo'
    if (Test-Path -LiteralPath $logo) { Copy-Into $logo 'AddOns\cwr_logo.pbo'; $n++ }
    Write-Host "  + assets\BIN, fonts, dtaExt, AddOns (from the Demo)  $n files"
} else {
    Write-Warning ("The CWA Demo (Steam App $CwaDemoAppId) is not installed here: the package lacks the " +
                   "remaster's menu resources, fonts, dtaExt and cwr_logo.pbo, and the friend must run the Demo's Install.ps1 step.")
}

# 5. the DLSS runtime, if this machine has it (Deploy.ps1 looks in the same two places)
foreach ($snippet in @((Join-Path $repoRoot '..\..\..\.tmp-dlss-sdk\lib\Windows_x86_64\rel\nvngx_dlss.dll'),
                       (Join-Path $repoRoot '.tmp-dlss-sdk\lib\Windows_x86_64\rel\nvngx_dlss.dll'))) {
    if (Test-Path -LiteralPath $snippet) { Copy-Into $snippet 'nvngx_dlss.dll'; Write-Host "  + assets\nvngx_dlss.dll"; break }
}

# 6. the Island Editor
$editor = Join-Path $distDir 'PoseidonIslandEditor.exe'
if (Test-Path -LiteralPath $editor) { Copy-Into $editor 'PoseidonIslandEditor.exe'; Write-Host "  + assets\PoseidonIslandEditor.exe" }

# 7. licences, credits, a README and the build stamp
foreach ($doc in @('LICENSE', 'LICENSE.md', 'CREDITS.md', 'NOTICE', 'NOTICE.md')) {
    $p = Join-Path $repoRoot $doc
    if (Test-Path -LiteralPath $p) { Copy-Into $p (Join-Path 'legal' $doc) }
}
# Steam Audio is linked statically (Apache-2.0): its licence and the CIPIC HRTF notice ship with it.
foreach ($notice in Get-ChildItem -LiteralPath (Join-Path $repoRoot 'engine\PoseidonSteamAudio\notices') -File -ErrorAction SilentlyContinue) {
    Copy-Into $notice.FullName (Join-Path 'legal' $notice.Name)
}
@"
OpenPoseidon $branch $tag ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))

Install: copy OpenPoseidon.exe, wgpu_renderer.dll and this assets folder into your
ARMA: Cold War Assault folder (the one with ColdWarAssault.exe), then start OpenPoseidon.exe.
Nothing else is replaced; ColdWarAssault.exe keeps working as before.

The game data (models, textures, sounds, missions) is not included: it comes from your own
Cold War Assault install. The engine source is GPL-3.0-or-later (see legal\).
"@ | Set-Content -LiteralPath (Join-Path $assets 'README.txt') -Encoding utf8
"$branch $tag $(Get-Date -Format 'yyyy-MM-dd_HH:mm') package-share-ps1" |
    Set-Content -LiteralPath (Join-Path $assets 'BUILD.txt') -Encoding utf8

$top = @(Get-ChildItem -LiteralPath $out | ForEach-Object Name)
if (@(Compare-Object $top @('assets', 'OpenPoseidon.exe', 'wgpu_renderer.dll')).Count -ne 0) {
    throw "The package must hold exactly OpenPoseidon.exe, wgpu_renderer.dll and assets\, found: $($top -join ', ')"
}
$size = (Get-ChildItem -LiteralPath $out -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("`nPackage: {0}  ({1:N1} MB)" -f $out, ($size / 1MB))
if (-not $NoZip) {
    $zip = "$out.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip
    Write-Host ("Zip:     {0}  ({1:N1} MB)" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB))
}
