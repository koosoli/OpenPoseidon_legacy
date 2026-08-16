# Generic N-arm capture: same world and pose, one arm per env set, one PNG each.
#
# Built because every open visual question here has the same shape — "does this env var change the
# picture?" — and the one-off scripts kept re-deriving the same launch incantation and its traps.
#
# Traps encoded here so they are not rediscovered (see the substituted-world memory):
#  - --test-world-freefly takes X Z Y (ALTITUDE LAST), while logs print (x, altitude, z). A pose
#    copied from a log must be transposed, and getting it wrong silently flies the camera into the
#    sky and photographs clouds without erroring.
#  - --test-mission needs an ABSOLUTE path.
#  - --auto-screenshot accepts exactly one capture.
#  - PowerShell 5.1 Start-Process mangles quoting on paths with spaces and the game dies before it
#    opens its log; launch via cmd start. Never redirect stdout/stderr (kills the renderer, os 232).
#  - The launcher returns while the game runs; poll for the artefact.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$World,
    [string]$Addons  = '',
    # Arma 3 worlds need their mod registered (e.g. a3stratis) or the world loads with no data.
    [string[]]$Mods  = @(),
    [Parameter(Mandatory)][string]$Freefly,   # "X Z Y az el"
    [int]$Hour       = 12,
    [int]$ShotFrame  = 900,
    # --auto-screenshot also accepts a TIME form ("120s:path"). Needed where the artefact only
    # appears after the object stream has settled: at this Stratis pose frame 900 shows the bare
    # white bar 4/4 but the full bloomed orb only 1/4, while 120 s gives it 2/2.
    [string]$ShotSpec = '',
    [int]$Width      = 1600,
    [int]$Height     = 900,
    [int]$TimeoutMin = 8,
    # Arms: ordered pairs of name -> env hashtable. Use [ordered] so arm order is stable.
    [Parameter(Mandatory)][System.Collections.IDictionary]$Arms
)

$ErrorActionPreference = 'Stop'
$game    = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
$exe     = Join-Path $game 'ColdWarAssault.exe'
$mission = Join-Path $game 'dev-missions\devtest-dawn.abel'

foreach ($p in @($exe, $World, $mission)) { if (-not (Test-Path $p)) { throw "missing: $p" } }
if ($Addons -and -not (Test-Path $Addons)) { throw "missing: $Addons" }
New-Item -ItemType Directory -Force $Out | Out-Null
Write-Host ("world: {0} ({1} MB)" -f $World, [math]::Round((Get-Item $World).Length / 1MB, 1))

$ff = $Freefly -split '\s+'

foreach ($name in $Arms.Keys) {
    $png = Join-Path $Out "$name.png"
    $log = Join-Path $Out "$name.log"
    Remove-Item $png, $log -ErrorAction SilentlyContinue

    $envSet = $Arms[$name]
    foreach ($k in $envSet.Keys) { Set-Item -Path "Env:$k" -Value $envSet[$k] }

    # NOT $args — read-only automatic variable; assigning it throws under -ErrorAction Stop.
    $cmdArgs = @(
        '--render wgpu', '--window', '--dev',
        "--width $Width", "--height $Height",
        "--test-mission `"$mission`"",
        "--test-world `"$World`"",
        $(if ($Addons) { "--addon-root `"$Addons`"" } else { '' }),
        $(($Mods | ForEach-Object { "--mod $_" }) -join ' '),
        "--test-world-freefly $($ff[0]) $($ff[1]) $($ff[2]) $($ff[3]) $($ff[4])",
        "--test-world-hour $Hour",
        $(if ($ShotSpec) { "--auto-screenshot `"${ShotSpec}:$png`"" } else { "--auto-screenshot `"${ShotFrame}:$png`"" }),
        "--log-file `"$log`"", '--log-level info'
    ) -join ' '

    Write-Host "`n=== arm: $name === $(($envSet.GetEnumerator() | % { "$($_.Key)=$($_.Value)" }) -join ' ')" -ForegroundColor Cyan
    cmd /c "start `"cap-$name`" /D `"$game`" `"$exe`" $cmdArgs"

    $deadline = (Get-Date).AddMinutes($TimeoutMin)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $png) { Start-Sleep -Seconds 3; break }
        Start-Sleep -Seconds 5
    }
    Get-Process ColdWarAssault -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 2
    foreach ($k in $envSet.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }

    if (Test-Path $png) { Write-Host "  ok: $png" -ForegroundColor Green }
    else                { Write-Host "  NO PNG - check $log" -ForegroundColor Red }
}
