[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'build/terrain-contact-focus-policy'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe=Join-Path $output 'terrain_contact_focus_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $repo 'engine')) (Join-Path $repo 'tests/unit/engine/Poseidon/World/test_terrain_contact_focus.cpp') '-o' $exe
if($LASTEXITCODE -ne 0){throw 'Terrain contact-focus compilation failed.'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Terrain contact-focus policy assertions failed.'}
