[CmdletBinding()]
param(
    [string]$Extract = 'M:\ForestClaude\extract',
    [string]$Preset = 'win-x64-clang-rwdi',
    [string]$Python = 'python',
    [double]$SeaLevel = 40.0
)
# Sinkhole W5: build The Forest's island as a LOCAL mod from the owner's own extracted game data:
# build/forest/@ForestIsland/AddOns/forestisland.pbo (prefix forestisland: theforest.wrp with its trees, rocks and
# cliffs placed, ground materials, object models and textures, CfgWorlds >> TheForest). Needs the extract folder's
# prefab_roots.json and colliders.json (M:\ForestClaude\tools\export_prefab_roots.py, export_colliders.py) besides
# the models and scenes; trees, rocks and cliffs collide (forest_collision.py), bushes do not. Not shipped, not
# committed: the Forest data is not distributable. Start the game with --mod pointing to the folder and play
# dev-missions/forest-beach.TheForest.
$ErrorActionPreference = 'Stop'
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') { throw 'Unsupported preset' }
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$suffix = ($Preset -split '-')[-1]
$tools = Join-Path $root "dist/x64-win-$suffix/PoseidonTools.exe"
if (!(Test-Path -LiteralPath $tools)) { throw "Build PoseidonTools first: $tools" }
$stage = Join-Path $root 'build/forest/addon'
$mod = Join-Path $root 'build/forest/@ForestIsland'
$allowedStage = [IO.Path]::GetFullPath((Join-Path $root 'build/forest/addon'))
if (Test-Path -LiteralPath $stage) {
    $resolvedStage = (Resolve-Path -LiteralPath $stage).ProviderPath
    if (![string]::Equals($resolvedStage, $allowedStage, [StringComparison]::OrdinalIgnoreCase) -or
        ((Get-Item -LiteralPath $stage -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to remove unexpected or linked staging folder: $resolvedStage"
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
& $Python "$PSScriptRoot/forest_terrain.py" --extract (Join-Path $Extract 'terrain') --out $stage --sea-level $SeaLevel
if ($LASTEXITCODE) { throw 'Forest terrain conversion failed' }
Remove-Item -LiteralPath (Join-Path $stage 'forest_info.json') -Force
New-Item -ItemType Directory -Force (Join-Path $mod 'AddOns') | Out-Null
& $tools pbo pack $stage (Join-Path $mod 'AddOns/forestisland.pbo') --prefix forestisland
if ($LASTEXITCODE) { throw 'Packing forestisland failed' }
Write-Host "Forest island mod: $mod"
Write-Host "Mission: dev-missions/forest-beach.TheForest (run with --mod `"$mod`")"
