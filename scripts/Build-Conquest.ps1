param([string]$Preset='win-x64-clang-rwdi')
# Stage the @OP_Conquest default content pack: content/default-packs/conquest's config.cpp packed as
# Mods/@OP_Conquest/AddOns/op_conquest.pbo (prefix "op_conquest"), its mod.json, and the Conquest missions in
# Missions/OpenPoseidon (where the main menu's CONQUEST entry and the Missions list find them).
# Called by Build-DefaultContent.ps1; Deploy.ps1 installs the result.
$ErrorActionPreference='Stop'
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') {throw 'Unsupported content staging preset'}
$root=Split-Path $PSScriptRoot -Parent
$suffix=($Preset -split '-')[-1]
$source=Join-Path $root 'content/default-packs/conquest'
$stage=Join-Path $root 'build/conquest/pack-source'
$pack=Join-Path $root "dist/x64-win-$suffix/assets/Mods/@OP_Conquest"
$tools=Join-Path $root "dist/x64-win-$suffix/PoseidonTools.exe"
if (!(Test-Path -LiteralPath $tools)) {throw 'Build PoseidonTools before packaging Conquest'}
if (Test-Path -LiteralPath $stage) {Remove-Item -LiteralPath $stage -Recurse -Force}
New-Item -ItemType Directory -Force $stage, (Join-Path $pack 'AddOns') | Out-Null
Copy-Item -LiteralPath "$source/config.cpp" -Destination "$stage/config.cpp"
& $tools pbo pack $stage "$pack/AddOns/op_conquest.pbo" --prefix op_conquest
if ($LASTEXITCODE -ne 0) {throw 'Conquest packaging failed'}
Copy-Item -LiteralPath "$source/mod.json" -Destination "$pack/mod.json" -Force
# Every Conquest mission carries its own copy of the war's rules, conquest.hpp (a mission folder is
# what the game, and a server's clients, receive: an include from outside it would not reach them).
# The copies must match Everon's, and the build stops if one has drifted. Each mission's
# objectives.hpp is its own: Everon's by hand, the other islands' objectives = "auto".
$war=Join-Path $source 'missions/conquest-everon.Eden/conquest.hpp'
foreach ($copy in Get-ChildItem -LiteralPath "$source/missions", "$source/mpmissions" -Recurse -Filter conquest.hpp -ErrorAction SilentlyContinue) {
    if ((Get-FileHash -LiteralPath $copy.FullName).Hash -ne (Get-FileHash -LiteralPath $war).Hash) {
        throw "$($copy.FullName) differs from $war -- copy the war's rules over it"
    }
}
$missions=Join-Path $pack 'Missions/OpenPoseidon'
New-Item -ItemType Directory -Force $missions | Out-Null
foreach ($mission in Get-ChildItem -LiteralPath "$source/missions" -Directory) {
    $target=Join-Path $missions $mission.Name
    if (Test-Path -LiteralPath $target) {Remove-Item -LiteralPath $target -Recurse -Force}
    Copy-Item -LiteralPath $mission.FullName -Destination $missions -Recurse -Force
}
# Multiplayer missions go to MPMissions.
$mpMissions=Join-Path $pack 'MPMissions'
New-Item -ItemType Directory -Force $mpMissions | Out-Null
foreach ($mission in Get-ChildItem -LiteralPath "$source/mpmissions" -Directory -ErrorAction SilentlyContinue) {
    $target=Join-Path $mpMissions $mission.Name
    if (Test-Path -LiteralPath $target) {Remove-Item -LiteralPath $target -Recurse -Force}
    Copy-Item -LiteralPath $mission.FullName -Destination $mpMissions -Recurse -Force
}
Write-Host "Staged Conquest: $pack"
