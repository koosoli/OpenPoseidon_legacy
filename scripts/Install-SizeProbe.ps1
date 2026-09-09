param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Run via scripts/with-game-lock.sh with LOCK_OWNER set.'}
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/fusion/size-probe'
if (!(Test-Path -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe'))) {throw 'Not an OpenPoseidon game install'}
python (Join-Path $PSScriptRoot 'fusion/build_size_probe.py') $output
if ($LASTEXITCODE -ne 0) {throw 'Size probe generation failed'}
Copy-Item -LiteralPath (Join-Path $output 'fusion_size_probe.pbo') -Destination (Join-Path $GameDir 'AddOns/fusion_size_probe.pbo') -Force
Copy-Item -LiteralPath (Join-Path $output 'SizeProbe.FusionSizeProbe') -Destination (Join-Path $GameDir 'Missions') -Recurse -Force
Write-Output 'Installed sparse 102.4 km diagnostic world, not a populated-world benchmark.'
