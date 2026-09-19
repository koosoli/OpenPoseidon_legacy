param(
    [switch]$Legacy,
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
if (!$Legacy) {
    $stamp = Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt')
    $installedCommit = (($stamp -split ' ')[1] -split '\+')[0]
    & git -C (Split-Path $PSScriptRoot -Parent) merge-base --is-ancestor c9006e58 $installedCommit
    if ($LASTEXITCODE) {throw 'The revised curling prototype has not been installed yet; the old ocean remains available normally.'}
}
# Private review profile only. Normal game starts keep the old ocean default.
$runner = Join-Path $PSScriptRoot 'Test-OceanCoherence.ps1'
if ($Legacy) {
    & $runner -CurlingBaseline -Review -Preset 4 -BinaryDir $GameDir -GameDir $GameDir -Label owner-curl-baseline
} else {
    & $runner -Curling -Review -Preset 4 -BinaryDir $GameDir -GameDir $GameDir -Label owner-curl-review
}
