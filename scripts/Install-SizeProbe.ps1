param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Run via scripts/with-game-lock.sh with LOCK_OWNER set.'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/fusion/size-probe'
if (!(Test-Path -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe'))) {throw 'Not an OpenPoseidon game install'}
python (Join-Path $PSScriptRoot 'fusion/build_size_probe.py') $output
if ($LASTEXITCODE -ne 0) {throw 'Size probe generation failed'}
& "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir $GameDir
$pack=Join-Path $GameDir 'Mods/@OP_SizeProbe'
New-Item -ItemType Directory -Force "$pack/AddOns", "$pack/Missions/OpenPoseidon" | Out-Null
Copy-Item -LiteralPath (Join-Path $output 'fusion_size_probe.pbo') -Destination "$pack/AddOns/fusion_size_probe.pbo" -Force
Copy-Item -LiteralPath (Join-Path $output 'SizeProbe.FusionSizeProbe') -Destination "$pack/Missions/OpenPoseidon" -Recurse -Force
Copy-Item -LiteralPath "$root/content/default-packs/layout/@OP_SizeProbe.json" -Destination "$pack/mod.json" -Force
Write-Output 'Installed sparse 102.4 km diagnostic world, not a populated-world benchmark.'
