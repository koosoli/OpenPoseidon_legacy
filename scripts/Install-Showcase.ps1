param([string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh with LOCK_OWNER set.' }
$root=Split-Path $PSScriptRoot -Parent
$output=Join-Path $root 'build/showcase/showcase_lab.pbo'
python "$PSScriptRoot/showcase/build_showcase.py" $output
if ($LASTEXITCODE -ne 0) { throw 'Showcase addon generation failed' }
if (!(Test-Path -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe'))) { throw 'Not a game install' }
New-Item -ItemType Directory -Force (Join-Path $GameDir 'Addons') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $GameDir 'Missions') | Out-Null
Copy-Item -LiteralPath $output -Destination (Join-Path $GameDir 'Addons/showcase_lab.pbo') -Force
Copy-Item -LiteralPath (Join-Path $root 'content/showcase/ShowcaseLab.Intro') -Destination (Join-Path $GameDir 'Missions') -Recurse -Force
Write-Output 'Installed: Single Missions / Showcase Lab - Physics and Light'
