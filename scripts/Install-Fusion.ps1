param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Run via scripts/with-game-lock.sh with LOCK_OWNER set.'}
$root=Split-Path $PSScriptRoot -Parent
$tools=Join-Path $root 'dist/x64-win-rwdi/PoseidonTools.exe'
$build=Join-Path $root 'build/fusion'
if (!(Test-Path -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe'))) {throw 'Not an OpenPoseidon game install'}
New-Item -ItemType Directory -Force (Join-Path $build 'sources') | Out-Null
& $tools pbo extract (Join-Path $GameDir 'AddOns/Noe.pbo') (Join-Path $build 'source-noe')
if ($LASTEXITCODE -ne 0) {throw 'Nogova extraction failed'}
foreach ($name in @('eden','abel','cain','noe')) {
 $source=if ($name -eq 'noe') {Join-Path $build 'source-noe/noe.wrp'} else {Join-Path $GameDir "Worlds/$name.wrp"}
 & $tools terrain export-rvw4 $source (Join-Path $build "sources/$name.wrp")
 if ($LASTEXITCODE -ne 0) {throw "Terrain export failed: $name"}
}
# the source worlds' airports and map names: the game's own config (BIN) and Nogova's, debinarised
& $tools config debin (Join-Path $GameDir 'BIN/config.bin') | Set-Content -Encoding Default (Join-Path $build 'config-base.cpp')
if ($LASTEXITCODE -ne 0) {throw 'Debinarising BIN/config.bin failed'}
& $tools config debin (Join-Path $build 'source-noe/config.bin') | Set-Content -Encoding Default (Join-Path $build 'config-noe.cpp')
if ($LASTEXITCODE -ne 0) {throw 'Debinarising Noe config.bin failed'}
python (Join-Path $PSScriptRoot 'fusion/build_fusion.py') (Join-Path $build 'sources') (Join-Path $build 'output') --config (Join-Path $build 'config-base.cpp') --config (Join-Path $build 'config-noe.cpp')
if ($LASTEXITCODE -ne 0) {throw 'Fusion generation failed'}
& $tools terrain inspect (Join-Path $build 'output/fusion.wrp')
if ($LASTEXITCODE -ne 0) {throw 'Generated WRP validation failed'}
& "$PSScriptRoot/Move-DefaultContentToMods.ps1" -GameDir $GameDir
$pack=Join-Path $GameDir 'Mods/@OP_Fusion'
New-Item -ItemType Directory -Force "$pack/AddOns", "$pack/Missions/OpenPoseidon" | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'output/fusion_ofp.pbo') -Destination "$pack/AddOns/fusion_ofp.pbo" -Force
# replace, not merge: files dropped from the tour (e.g. desert.sqs) must not linger
$tour="$pack/Missions/OpenPoseidon/FusionTour.FusionOFP"
if (Test-Path -LiteralPath $tour) {Remove-Item -LiteralPath $tour -Recurse -Force}
Copy-Item -LiteralPath (Join-Path $root 'content/fusion/FusionTour.FusionOFP') -Destination "$pack/Missions/OpenPoseidon" -Recurse -Force
Copy-Item -LiteralPath "$root/content/default-packs/layout/@OP_Fusion.json" -Destination "$pack/mod.json" -Force
Write-Output 'Installed: Single Missions / OpenPoseidon / OFP Fusion - Four Islands; editor world: OFP Fusion - Four Islands.'
