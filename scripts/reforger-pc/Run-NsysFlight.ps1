# One Nsight Systems trace of the Reforger Durras stutter-test flight (15 m/s drift, 3440x1440,
# VSync off), exported to SQLite for scripts/reforger-pc/nsys_long_frames.py.
#
# Run from an ADMINISTRATOR PowerShell -- CPU sampling, context switches, WDDM and the one-time
# Vulkan layer registration all need it (without it nsys silently drops them):
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\reforger-pc\Run-NsysFlight.ps1
# Optional: -Harness <dir with go.ps1> (default %TEMP%\rfh\_OpenPoseidon-Reforger),
#           -Build <dist dir> (default: the bin\ junction's target), -Tag <shots subfolder>.
# Takes ~5 minutes: ~2 min flight, ~3 min report + SQLite export (3-4 GB). Changes nothing
# permanent; the graphics.cfg VSync tweak is restored. Full recipe: .agents/PROFILING-WITH-NSIGHT.md
param(
    [string]$Harness = (Join-Path $env:LOCALAPPDATA 'Temp\rfh\_OpenPoseidon-Reforger'),
    [string]$Build = '',
    [string]$Tag = 'ns-flight'
)
$ErrorActionPreference = 'Stop'
$h = $Harness
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

# The flight: the Durras pose, drifting 15 m/s from 60 s; nsys records 45..95 s.
'durras 8595.2 3069.61 234.15A 145.0 -12.0' | Set-Content (Join-Path $h 'nsys-poses.txt') -Encoding ascii
$out = Join-Path $h "shots\$Tag"
@(
    'MODE=poses', 'POSES=nsys-poses.txt', 'ONLY=durras', 'HOUR=11.18', 'FIRST=80', 'SETTLE=20', 'FOVY=55.4', 'QUIET=5',
    'REFORGER=C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons',
    'TOOLS=C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger Tools',
    'POSEIDON_TEST_DATE=1989-08-14', 'POSEIDON_FREEFLY_DRIFT=15,0,60', 'WGR_SLOW_BLOCK_MS=45',
    'WIDTH=3440', 'HEIGHT=1440', 'WINDOW=1', "TAG=$Tag", 'EXE=bin-nsys',
    "NSYS=$nsys", 'NSYSDELAY=45', 'NSYSDUR=50'
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

$rep = Get-ChildItem $out -Filter 'nsys*.nsys-rep' -ErrorAction SilentlyContinue | Where-Object Length -gt 0 | Select-Object -First 1
if (-not $rep) {
    Write-Host 'No trace was written. Last lines:' -ForegroundColor Red
    Get-Content (Join-Path $h 'go.status') -Tail 5
    Get-Content (Join-Path $out 'stderr.txt') -Tail 5 -ErrorAction SilentlyContinue
    exit 1
}
Write-Host 'Exporting to SQLite (a few minutes)...'
& $nsys export --type sqlite --force-overwrite true --output (Join-Path $out 'nsys.sqlite') $rep.FullName
Write-Host "Done. $($rep.FullName) and nsys.sqlite" -ForegroundColor Green
