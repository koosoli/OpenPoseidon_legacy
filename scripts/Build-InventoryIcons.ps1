<# Stage only the project's own icons. Regeneration is an explicit authoring step;
   ordinary source/release builds copy the versioned PAA payload without game art. #>
[CmdletBinding()]
param([string]$Preset='win-x64-clang-rwdi')
$ErrorActionPreference='Stop'
if ($Preset -notmatch '^win-x64-clang-(rwdi|rel|dbg)$') { throw 'Unsupported icon staging preset' }
$root=Split-Path $PSScriptRoot -Parent
$suffix=($Preset -split '-')[-1]
$source=Join-Path $root 'assets/inventory'
$stage=Join-Path $root "dist/x64-win-$suffix/assets/inventory"
$names=@('m16','rifle','ak_rifle','scoped_rifle','smg','machinegun','launcher','pistol','binoculars','nvg','magazine','curved_magazine','grenade','smoke','satchel','mine','rocket')
$null=New-Item -ItemType Directory -Path $stage -Force
foreach ($name in $names) {
    $path=Join-Path $source ($name+'.paa')
    if (!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -eq 0) { throw "Missing versioned inventory icon: $path" }
    Copy-Item -LiteralPath $path -Destination (Join-Path $stage ($name+'.paa')) -Force
}
Write-Host 'Staged 17 original inventory icons; local game/mod pictures remain fallbacks.'
