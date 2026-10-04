[CmdletBinding()]
param([string]$Preset='win-x64-clang-rwdi')
# Sinkhole W1/W1b test content (WLD-HOLE-001, LGT-CAVE-001): pack tests/content/sinkhole/ugcave and ugtest into an
# opt-in test mod, build/sinkhole-test/@OP_SinkholeTest/AddOns/{ugcave,ugtest}.pbo. Not shipped or installed by
# default. Run the missions dev-missions/ugcave-test.Eden and ugbasement-test.Eden with --mod pointing to the folder.
$ErrorActionPreference = 'Stop'
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') { throw 'Unsupported preset' }
$root = Split-Path -Parent $PSScriptRoot
$suffix = ($Preset -split '-')[-1]
$tools = Join-Path $root "dist/x64-win-$suffix/PoseidonTools.exe"
if (!(Test-Path -LiteralPath $tools)) { throw "Build PoseidonTools first: $tools" }
$mod = Join-Path $root 'build/sinkhole-test/@OP_SinkholeTest'
$addons = Join-Path $mod 'AddOns'
New-Item -ItemType Directory -Force -Path $addons | Out-Null
$expected = @{
    'ugcave' = @('config.cpp', 'ugcave.p3d', 'cave_wall.dds', 'cave_ceil.dds', 'cave_floor.dds', 'cave_rock.dds')
    'ugtest' = @('config.cpp', 'ugbasement.p3d')
}
foreach ($name in $expected.Keys) {
    $source = Join-Path $root "tests/content/sinkhole/$name"
    foreach ($file in $expected[$name]) {
        if (!(Test-Path -LiteralPath (Join-Path $source $file))) { throw "Missing test content: $source/$file" }
    }
    foreach ($file in Get-ChildItem -LiteralPath $source -Force) {
        if ($file.PSIsContainer -or $file.Name -notin $expected[$name]) { throw "Unexpected test content: $($file.FullName)" }
    }
    & $tools pbo pack $source (Join-Path $addons "$name.pbo") --prefix $name
    if ($LASTEXITCODE) { throw "Packing $name failed." }
}
Write-Host "Sinkhole test mod: $mod"
Write-Host "Missions: dev-missions/ugcave-test.Eden, dev-missions/ugbasement-test.Eden (copy into the game's Missions folder,"
Write-Host "or run one with --test-mission), and start the game with --mod `"$mod`"."
