param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh with LOCK_OWNER set.' }
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/showcase/showcase_lab.pbo'
python "$PSScriptRoot/showcase/build_showcase.py" $output
if ($LASTEXITCODE -ne 0) { throw 'Showcase addon generation failed' }
if (!(Test-Path -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe'))) { throw 'Not a game install' }
& "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir $GameDir
$pack=Join-Path $GameDir 'Mods/@OP_Showcase'
New-Item -ItemType Directory -Force "$pack/AddOns", "$pack/Missions/OpenPoseidon" | Out-Null
Copy-Item -LiteralPath $output -Destination "$pack/AddOns/showcase_lab.pbo" -Force
Copy-Item -LiteralPath (Join-Path $root 'content/showcase/ShowcaseLab.Intro') -Destination "$pack/Missions/OpenPoseidon" -Recurse -Force
Copy-Item -LiteralPath "$root/content/default-packs/layout/@OP_Showcase.json" -Destination "$pack/mod.json" -Force
Write-Output 'Installed: Single Missions / OpenPoseidon / Showcase Lab - Physics and Light'
