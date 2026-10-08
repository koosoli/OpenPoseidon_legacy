<#
.SYNOPSIS
    Start an optional single-player official ReShade Vulkan preview.
    Run Prepare-ReShade.ps1 once first. Normal launches stay unchanged.
.EXAMPLE
    .\scripts\Launch-ReShade.ps1 -GameArguments @('--window','--width','1600','--height','900')
#>
[CmdletBinding()]
param(
    [string]$GameDir = 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [string]$RuntimeDirectory = (Join-Path $env:LOCALAPPDATA 'OpenPoseidon/reshade/6.8.0'),
    [switch]$EnableDlss,
    # Internal/native -File forwarding: a single JSON value preserves string[]
    # arguments (including paths with spaces) across the lock's child process.
    [string]$GameArgumentsJson,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$GameArguments
)
$ErrorActionPreference = 'Stop'
if ($GameArgumentsJson) {
    if ($GameArguments) { throw 'Use GameArguments or GameArgumentsJson, not both.' }
    $GameArguments = @(ConvertFrom-Json -InputObject $GameArgumentsJson)
}
$repo = Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
    $forward = @('-NoProfile', '-File', $PSCommandPath, '-GameDir', $GameDir,
        '-RuntimeDirectory', $RuntimeDirectory)
    if ($EnableDlss) { $forward += '-EnableDlss' }
    if ($GameArguments) { $forward += @('-GameArgumentsJson', (ConvertTo-Json -InputObject @($GameArguments) -Compress)) }
    $previous = $env:LOCK_OWNER
    try {
        $env:LOCK_OWNER = 'Optional ReShade preview'
        & 'C:/Program Files/Git/bin/bash.exe' (Join-Path $repo 'scripts/with-game-lock.sh') `
            (Get-Process -Id $PID).Path @forward
        if ($LASTEXITCODE -ne 0) { throw "ReShade preview exited with $LASTEXITCODE" }
    } finally { $env:LOCK_OWNER = $previous }
    return
}
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) {
    throw 'Close the existing game before starting this preview.'
}
foreach ($name in @('ReShade64.dll','ReShade64.json','ReShade.ini','OpenPoseidonReShade.addon64')) {
    if (!(Test-Path -LiteralPath (Join-Path $RuntimeDirectory $name))) {
        throw "Missing $name. Run scripts/Prepare-ReShade.ps1 first."
    }
}
$expected = '0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7'
if ((Get-FileHash -LiteralPath (Join-Path $RuntimeDirectory 'ReShade64.dll')).Hash.ToLowerInvariant() -ne $expected) {
    throw 'ReShade runtime differs from the pinned official 6.8.0 build.'
}
$manifestExpected = 'aa21713718843e531da396e2bfc80772c9cb3c369d6c30b836be6b0ae812d503'
if ((Get-FileHash -LiteralPath (Join-Path $RuntimeDirectory 'ReShade64.json')).Hash.ToLowerInvariant() -ne $manifestExpected) {
    throw 'ReShade layer manifest differs from the pinned official 6.8.0 build.'
}
$variables = @('VK_ADD_LAYER_PATH','VK_INSTANCE_LAYERS','WGPU_BACKEND','WGR_DLSS','RESHADE_BASE_PATH_OVERRIDE')
$saved = @{}
foreach ($name in $variables) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    # Explicit per-process layer discovery: no HKCU/HKLM layer registration.
    $env:VK_ADD_LAYER_PATH = $RuntimeDirectory + $(if ($saved.VK_ADD_LAYER_PATH) { ';' + $saved.VK_ADD_LAYER_PATH })
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_reshade' + $(if ($saved.VK_INSTANCE_LAYERS) { ';' + $saved.VK_INSTANCE_LAYERS })
    $env:RESHADE_BASE_PATH_OVERRIDE = $RuntimeDirectory
    $env:WGPU_BACKEND = 'vulkan'
    if (!$EnableDlss) { $env:WGR_DLSS = '0' }
    Push-Location $GameDir
    try { & (Join-Path $GameDir 'OpenPoseidon.exe') --render=wgpu --dev @GameArguments }
    finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw "OpenPoseidon exited with $LASTEXITCODE; inspect engine and ReShade logs." }
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
