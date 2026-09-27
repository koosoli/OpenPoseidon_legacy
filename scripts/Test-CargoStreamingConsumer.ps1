param([switch]$Disabled,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
      [string]$Python='C:\Program Files\Python311\python.exe')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {
    $shellName=if ($PSVersionTable.PSEdition -eq 'Core') {'pwsh.exe'} else {'powershell.exe'}
    $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),(Join-Path $PSHOME $shellName).Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-GameDir',$GameDir,'-Python',$Python)
    if ($Disabled) {$argv+='-Disabled'}
    $env:LOCK_OWNER='cargo streaming consumer fixture'
    try {& 'C:\Program Files\Git\bin\bash.exe' @argv; if ($LASTEXITCODE) {throw "Cargo fixture exited $LASTEXITCODE"}}
    finally {Remove-Item Env:LOCK_OWNER}
    return
}
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) {throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$fixture=Join-Path $root ('build/stream-residency/cargo-world-'+(Get-Date -Format yyyyMMdd-HHmmss))
& $Python (Join-Path $PSScriptRoot 'streaming/build_simulation_residency_fixture.py') $fixture --absolute-model-path --land-side 256
if ($LASTEXITCODE) {throw 'Cargo world generation failed'}
# Keep the established real seat/weapon/target mission unchanged. Its coordinates
# fit this larger flat original OPRW world. The two authored walls remain far from
# the cargo shooter: this checks natural consumer execution and clear firing,
# not a newly streamed barrel obstruction or whole-map coverage.
& (Join-Path $PSScriptRoot 'Test-JeepPassenger.ps1') -NpcFire -DefaultPackage -StreamingWorld (Join-Path $fixture 'simulation-residency.wrp') -SimulationResidency:(!$Disabled) -GameDir $GameDir
