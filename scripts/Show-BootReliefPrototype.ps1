<# Opens the wet Truck5t demo with optional sole relief on actual mud/sand marks.
   Drive, get out, walk fresh mud and inspect footprints from 1-3m. The launcher
   holds the game lock until the owner closes the game. #>
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Existing game preserved; close it before opening the prototype.'}
$gitBash='C:/Program Files/Git/bin/bash.exe'
if(!(Test-Path -LiteralPath $gitBash)){throw 'Git for Windows Bash is required for the installed-game lock.'}
Push-Location $repo
try {
    & $gitBash -c 'LOCK_OWNER="owner-boot-relief-prototype" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Show-MudVehiclePrototype.ps1 -VehicleClass Truck5t -BootRelief'
    if($LASTEXITCODE -ne 0){throw 'Boot/truck prototype launcher failed.'}
} finally { Pop-Location }
