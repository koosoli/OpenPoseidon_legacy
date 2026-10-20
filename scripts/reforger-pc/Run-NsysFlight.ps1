# One Nsight Systems trace of a stutter-test flight (15 m/s drift, 3440x1440, VSync off), exported
# to SQLite for scripts/reforger-pc/nsys_long_frames.py.
#
#   -World Reforger (default): Arma Reforger Everon, the Durras pose, through the go.ps1 harness.
#   -World DayZ: DayZ Chernarus+ (packages\dayz-compat\world\chernarusplus\chernarusplus.wrp in the
#                main checkout + the DayZ install's Addons), the Test-DayZStreamingLatency pose
#                flying west, launched directly.
#
# Run from an ADMINISTRATOR PowerShell -- CPU sampling, context switches, WDDM and the one-time
# Vulkan layer registration all need it (without it nsys silently drops them):
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\reforger-pc\Run-NsysFlight.ps1 [-World DayZ]
# Optional: -Harness <dir with go.ps1> (default %TEMP%\rfh\_OpenPoseidon-Reforger),
#           -Build <dist dir> (default: the bin\ junction's target), -Tag <shots subfolder>,
#           -Trace <nsys --trace list> and -SkipExport (an agent's unelevated launch test: -Trace nvtx).
# Takes ~5 minutes: ~2 min flight, ~3 min report + SQLite export (3-4 GB). Changes nothing
# permanent; the graphics.cfg VSync tweak is restored. Full recipe: .agents/PROFILING-WITH-NSIGHT.md
param(
    [ValidateSet('Reforger', 'DayZ', 'Arma3')][string]$World = 'Reforger',
    [string]$Harness = (Join-Path $env:LOCALAPPDATA 'Temp\rfh\_OpenPoseidon-Reforger'),
    [string]$Build = '',
    [string]$Tag = '',
    [string]$Trace = 'vulkan,wddm,nvtx',
    [switch]$SkipExport
)
$ErrorActionPreference = 'Stop'
$h = $Harness
if (-not $Tag) { $Tag = @{ DayZ = 'ns-dayz'; Arma3 = 'ns-arma3'; Reforger = 'ns-flight' }[$World] }
if (-not (Test-Path (Join-Path $h 'go.ps1'))) { throw "No harness at $h (see .agents/PROFILING-WITH-NSIGHT.md, setup)" }

# Newest Nsight Systems on C: or D:. 2022.x needed admin even to start and is not supported here.
$nsys = Get-ChildItem 'C:\Program Files\NVIDIA Corporation', 'D:\Program Files\NVIDIA Corporation' -Directory `
            -Filter 'Nsight Systems 202*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'target-windows-x64\nsys.exe' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $nsys) { throw 'Nsight Systems not found under C:\ or D:\Program Files\NVIDIA Corporation' }
Write-Host "Nsight: $nsys"
if (Get-Process -Name OpenPoseidon -ErrorAction SilentlyContinue) {
    throw 'OpenPoseidon is running. Close the game first, then run this again.'
}

# The build under test, copied into a plain folder (bin-nsys): nsys mis-launches through junctions.
if (-not $Build) {
    $bin = Get-Item (Join-Path $h 'bin')
    $Build = if ($bin.LinkType -eq 'Junction') { [string]($bin.Target | Select-Object -First 1) } else { $bin.FullName }
}
$copy = Join-Path $h 'bin-nsys'
New-Item -ItemType Directory -Force $copy | Out-Null
foreach ($f in 'OpenPoseidon.exe', 'OpenPoseidon.pdb', 'wgpu_renderer.dll', 'wgpu_renderer.pdb', 'nvngx_dlss.dll',
               'OpenAL32.dll', 'PoseidonFormats.dll', 'BUILT-FROM.txt') {
    if (Test-Path (Join-Path $Build $f)) { Copy-Item (Join-Path $Build $f) $copy -Force }
}
foreach ($j in 'assets', 'Mods') {
    $link = Join-Path $copy $j
    if (Test-Path $link) { (Get-Item $link).Delete() }
    if (Test-Path (Join-Path $Build $j)) { New-Item -ItemType Junction -Path $link -Target (Join-Path $Build $j) | Out-Null }
}
Write-Host "Build: $Build -> $copy"
$out = Join-Path $h "shots\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Filter 'nsys*' -ErrorAction SilentlyContinue | Remove-Item -Force

if ($World -eq 'Reforger') {
    # The Durras pose, drifting 15 m/s from 60 s; nsys records 45..95 s.
    'durras 8595.2 3069.61 234.15A 145.0 -12.0' | Set-Content (Join-Path $h 'nsys-poses.txt') -Encoding ascii
    @(
        'MODE=poses', 'POSES=nsys-poses.txt', 'ONLY=durras', 'HOUR=11.18', 'FIRST=80', 'SETTLE=20', 'FOVY=55.4', 'QUIET=5',
        'REFORGER=C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons',
        'TOOLS=C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger Tools',
        'POSEIDON_TEST_DATE=1989-08-14', 'POSEIDON_FREEFLY_DRIFT=15,0,60', 'WGR_SLOW_BLOCK_MS=45',
        'WIDTH=3440', 'HEIGHT=1440', 'WINDOW=1', "TAG=$Tag", 'EXE=bin-nsys',
        "NSYS=$nsys", 'NSYSDELAY=45', 'NSYSDUR=50', "NSYSTRACE=$Trace"
    ) | Set-Content (Join-Path $h "job-$Tag.txt") -Encoding ascii

    # VSync off for the flight (as in every stutter test), restored afterwards.
    $cfg = Join-Path $h 'user\graphics.cfg'
    $userCfg = Join-Path $env:APPDATA 'CWR\graphics.cfg'
    if (Test-Path $cfg) { Copy-Item $cfg "$cfg.bak" -Force }
    if (Test-Path $userCfg) { (Get-Content $userCfg) -replace 'vsync=1;', 'vsync=0;' | Set-Content $cfg }
    try {
        Write-Host 'Flying the Durras test flight under Nsight (the game window opens; leave it alone)...'
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $h 'go.ps1') (Join-Path $h "job-$Tag.txt") *> (Join-Path $h 'nsys-run.log')
    } finally {
        if (Test-Path "$cfg.bak") { Copy-Item "$cfg.bak" $cfg -Force }
    }
} else {
    # DayZ Chernarus+: no harness of its own here, so the launch is built directly. The world file
    # lives in the MAIN checkout's packages\ (gitignored, proprietary data -- never committed).
    $cwaGame = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
    $repo = (git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir) -replace '[\\/]\.git$', ''
    # Per world: the extracted .wrp (main checkout, gitignored), the game's Addons, the start pose
    # (x z altitude azimuth elevation) and the drift (vx,vz m/s from 60 s) -- the drift follows the azimuth.
    $worlds = @{
        DayZ  = @{ Wrp = 'packages\dayz-compat\world\chernarusplus\chernarusplus.wrp'; Addons = 'D:\SteamLibrary\steamapps\common\DayZ\Addons';
                   Pose = @('5704.58', '2523.37', '150', '260.9', '-8'); Drift = '-15,0,60'; Name = 'Chernarus+' }
        Arma3 = @{ Wrp = 'packages\a3-compat\world\stratis\stratis.wrp'; Addons = 'D:\SteamLibrary\steamapps\common\Arma 3\Addons';
                   Pose = @('1700', '5050', '150', '90', '-8'); Drift = '15,0,60'; Name = 'Stratis' }
    }
    $w = $worlds[$World]
    $wrp = Join-Path $repo $w.Wrp
    $addons = $w.Addons
    if (-not (Test-Path $wrp)) { throw "$($w.Name) world not found at $wrp" }
    if (-not (Test-Path $addons)) { throw "$World addons not found at $addons" }
    $profile = Join-Path $out 'user'
    $mission = Join-Path $out 'flight.eden'
    New-Item -ItemType Directory -Force $profile, $mission | Out-Null
    [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=0;`n")
    @'
version=11;
class Mission {randomSeed=1234;
 class Intel {year=1985;month=6;day=21;hour=10;minute=0;};
 class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=1;
  class Item0 {position[]={%POSX%,0,%POSZ%};id=0;side="WEST";vehicle="SoldierWB";player="PLAYER COMMANDER";leader=1;skill=1;
   init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""; this disableAI ""TARGET""";};
 };};};
};
class Intro {randomSeed=1;class Intel {};};
class OutroWin {randomSeed=2;class Intel {};};
class OutroLoose {randomSeed=3;class Intel {};};
'@ -replace '%POSX%', $w.Pose[0] -replace '%POSZ%', $w.Pose[1] | Set-Content (Join-Path $mission 'mission.sqm')
    # The Test-DayZStreamingLatency pose (x z altitude azimuth elevation), climbing to 150 m and
    # drifting west (its azimuth) at 15 m/s from 60 s; nsys records 60..120 s, then ends the game.
    $env:POSEIDON_USER_DIR = $profile
    $env:POSEIDON_FREEFLY_DRIFT = $w.Drift
    $env:WGR_SLOW_BLOCK_MS = '45'
    $log = Join-Path $out 'run.log'
    $q = { param($s) if ($s -match ' ') { '"' + $s + '"' } else { $s } }
    $nsysArgs = @('profile', "--trace=$Trace", '--delay=60', '--duration=60', '--force-overwrite=true',
                  '-o', (& $q (Join-Path $out 'nsys')), (Join-Path $copy 'OpenPoseidon.exe'),
                  '--render=wgpu', '--window', '--width=3440', '--height=1440', '--vd', '2000',
                  '--test-world-freefly', $w.Pose[0], $w.Pose[1], $w.Pose[2], $w.Pose[3], $w.Pose[4],
                  '--test-world', (& $q $wrp), '--addon-root', (& $q $addons),
                  '--test-world-hour', '10', '--test-mission', (& $q $mission),
                  '--log-file', (& $q $log), '--log-level', 'info')
    Write-Host "Flying over $($w.Name) under Nsight (the game window opens; leave it alone, ~2.5 min)..."
    # nsys cannot start the game from a junction working directory; the game folder is a real one.
    $p = Start-Process -FilePath $nsys -ArgumentList $nsysArgs -WorkingDirectory $cwaGame -PassThru -Wait `
            -RedirectStandardError (Join-Path $out 'stderr.txt') -RedirectStandardOutput (Join-Path $out 'stdout.txt')
    Write-Host "nsys exit $($p.ExitCode)"
}

$rep = Get-ChildItem $out -Filter 'nsys*.nsys-rep' -ErrorAction SilentlyContinue | Where-Object Length -gt 0 | Select-Object -First 1
if (-not $rep) {
    Write-Host 'No trace was written. Last lines:' -ForegroundColor Red
    Get-Content (Join-Path $h 'go.status') -Tail 5 -ErrorAction SilentlyContinue
    Get-Content (Join-Path $out 'stderr.txt') -Tail 5 -ErrorAction SilentlyContinue
    exit 1
}
if ($SkipExport) { Write-Host "Trace (not exported): $($rep.FullName)"; exit 0 }
Write-Host 'Exporting to SQLite (a few minutes)...'
& $nsys export --type sqlite --force-overwrite true --output (Join-Path $out 'nsys.sqlite') $rep.FullName
Write-Host "Done. $($rep.FullName) and nsys.sqlite" -ForegroundColor Green
