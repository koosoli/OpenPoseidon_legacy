param(
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$mission = Join-Path $repo 'dev-missions/tw-showcase.eden'
$exe = Join-Path $GameDir 'OpenPoseidon.exe'
$stamp = Join-Path $GameDir 'DEPLOYED-FROM.txt'
foreach ($file in @($exe, (Join-Path $mission 'mission.sqm'), $stamp)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing showcase input: $file" }
}
if (Get-Process OpenPoseidon, ColdWarAssault -ErrorAction SilentlyContinue) {
    throw 'A game session is already running; leave it to its owner.'
}

$logDir = Join-Path $repo 'build/tidewater-showcase-interactive'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$log = Join-Path $logDir ('showcase-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$oldBackend = [Environment]::GetEnvironmentVariable('WGR_WATER_BACKEND')
$oldSoak = [Environment]::GetEnvironmentVariable('WGR_WATER_SOAK')
try {
    # This is an interactive 120-second mission, not a --check or screenshot fixture.
    $env:WGR_WATER_BACKEND = '1'
    Remove-Item Env:WGR_WATER_SOAK -ErrorAction SilentlyContinue
    $showcaseArgs = @(
        '--render=wgpu', '--window', '--width=1600', '--height=900', '--dev',
        '--test-mission', ('"' + $mission + '"'),
        '--log-file', ('"' + $log + '"')
    )
    $process = Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Normal -ArgumentList $showcaseArgs -PassThru
} finally {
    if ($null -eq $oldBackend) { Remove-Item Env:WGR_WATER_BACKEND -ErrorAction SilentlyContinue }
    else { $env:WGR_WATER_BACKEND = $oldBackend }
    if ($null -eq $oldSoak) { Remove-Item Env:WGR_WATER_SOAK -ErrorAction SilentlyContinue }
    else { $env:WGR_WATER_SOAK = $oldSoak }
}

Write-Output "Tidewater showcase started (PID $($process.Id)). Source: $([IO.File]::ReadAllText($stamp).Trim())"
Write-Output "Log: $log"
Write-Output 'The game remains under your control; this launcher does not close it.'
