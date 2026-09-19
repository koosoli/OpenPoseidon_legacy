param(
    [ValidateSet('Helicopter', 'Stress')][string]$Scenario = 'Helicopter',
    [switch]$LegacyHelicopter,
    [switch]$LegacyTargetScan,
    [switch]$CheckOnly,
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (!$env:LOCK_OWNER) {
    $bash = 'C:\Program Files\Git\bin\bash.exe'
    if (!(Test-Path -LiteralPath $bash)) { throw "Git Bash missing: $bash" }
    # Reuse this shell by absolute path: Windows PowerShell users need neither
    # PowerShell 7 nor a pwsh entry on the PATH inherited by Git Bash.
    $shellName = if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' }
    $shellExe = (Join-Path $PSHOME $shellName).Replace('\', '/')
    if (!(Test-Path -LiteralPath $shellExe)) { throw "PowerShell executable missing: $shellExe" }
    $lockArgs = @((Join-Path $PSScriptRoot 'with-game-lock.sh'), $shellExe, '-NoProfile', '-File', $PSCommandPath,
        '-Scenario', $Scenario, '-GameDir', $GameDir)
    if ($LegacyHelicopter) { $lockArgs += '-LegacyHelicopter' }
    if ($LegacyTargetScan) { $lockArgs += '-LegacyTargetScan' }
    if ($CheckOnly) { $lockArgs += '-CheckOnly' }
    $env:LOCK_OWNER = 'owner AI distance test'
    try { & $bash @lockArgs; if ($LASTEXITCODE -ne 0) { throw "Test exited with $LASTEXITCODE" } }
    finally { Remove-Item Env:LOCK_OWNER }
    return
}
$mission = if ($Scenario -eq 'Stress') {
    Join-Path $GameDir 'Mods/@63units/63MAX.west_ls2'
} else {
    Join-Path $repoRoot 'tests/perf/missions/perf_heli_distance.Intro'
}
if (!(Test-Path -LiteralPath $mission)) { throw "Mission missing: $mission" }
$gameExe = Join-Path $GameDir 'OpenPoseidon.exe'
if (!(Test-Path -LiteralPath $gameExe)) { throw "Game executable missing: $gameExe" }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Close the running OpenPoseidon first.' }
if ($CheckOnly) {
    Write-Host "Launch check passed: $Scenario, PowerShell $($PSVersionTable.PSVersion), game lock acquired."
    Write-Host "Mission: $mission"
    Write-Host 'No game started.'
    return
}
$logDir = Join-Path $repoRoot 'build/ai-distance'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir ($Scenario + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$previous = $env:POSEIDON_HELI_FAR_FULL_RATE
$previousScan = $env:POSEIDON_TARGET_SCAN_LINEAR
try {
    $env:POSEIDON_HELI_FAR_FULL_RATE = if ($LegacyHelicopter) { '0' } else { '1' }
    $env:POSEIDON_TARGET_SCAN_LINEAR = if ($LegacyTargetScan) { '1' } else { '0' }
    $gameArgs = @('--render=wgpu', '--dev', '--test-mission', $mission, '--log-file', $log)
    if ($Scenario -eq 'Stress') { $gameArgs += @('--addon-root', (Join-Path $GameDir 'Mods/@63units')) }
    Write-Host "Log: $log"
    Push-Location $GameDir
    try { & (Join-Path $GameDir 'OpenPoseidon.exe') @gameArgs }
    finally { Pop-Location }
} finally {
    $env:POSEIDON_HELI_FAR_FULL_RATE = $previous
    $env:POSEIDON_TARGET_SCAN_LINEAR = $previousScan
}
