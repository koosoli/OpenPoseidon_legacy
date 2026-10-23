# One Nsight Systems trace of a Cold War Assault test mission (e.g. the Everon combat scene),
# exported to SQLite for scripts/reforger-pc/nsys_long_frames.py. The CWA counterpart of
# scripts/reforger-pc/Run-NsysFlight.ps1: no harness, the mission runs straight from the game
# folder with the exe that is installed there (or -Build).
#
# Run from an ADMINISTRATOR PowerShell -- CPU sampling, context switches, WDDM and the one-time
# Vulkan layer registration all need it (without it nsys silently drops them):
#   powershell -NoProfile -ExecutionPolicy Bypass -File Run-NsysMission.ps1 -Mission <dir of mission.sqm>
# Optional: -Game <game folder with OpenPoseidon.exe and the data> (default: the H:\ Steam install's
#           Remastered), -Build <folder with exe/dll/pdb to trace> (default: -Game), -Out <output dir>
#           (default: <script dir>\nsys\<Tag>), -Tag, -Delay/-Duration (seconds of the capture window,
#           counted from program start; the mission starts ~10 s in), -Timeout (game exits after it),
#           -Trace <nsys --trace list>, -SkipExport.
# Takes ~4 minutes: ~2 min game, ~2 min report + SQLite export. Changes nothing permanent.
# Full recipe and the analysis: .agents/PROFILING-WITH-NSIGHT.md
param(
    [Parameter(Mandatory = $true)][string]$Mission,
    [string]$Game = 'H:\SteamLibrary\steamapps\common\ARMA Cold War Assault\Remastered',
    [string]$Build = '',
    [string]$Out = '',
    [string]$Tag = 'ns-cwa',
    [int]$Delay = 45,
    [int]$Duration = 40,
    [int]$Timeout = 100,
    [int]$Width = 1600,
    [int]$Height = 900,
    [string]$Trace = 'vulkan,wddm,nvtx',
    [switch]$SkipExport
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path (Join-Path $Mission 'mission.sqm'))) { throw "No mission.sqm in $Mission" }
if (-not (Test-Path (Join-Path $Game 'OpenPoseidon.exe'))) { throw "No OpenPoseidon.exe in $Game" }
if (-not $Build) { $Build = $Game }
if (-not $Out) { $Out = Join-Path $PSScriptRoot "nsys\$Tag" }

# Newest Nsight Systems on C:, D: or H:. 2022.x needed admin even to start and is not supported.
$roots = 'C:\Program Files\NVIDIA Corporation', 'D:\Program Files\NVIDIA Corporation', 'H:\Program Files\NVIDIA Corporation'
$nsys = Get-ChildItem $roots -Directory -Filter 'Nsight Systems 202*' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike 'Nsight Systems 2022*' } |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'target-windows-x64\nsys.exe' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $nsys) { throw "Nsight Systems (2023 or newer) not found under $($roots -join ', ')" }
Write-Host "Nsight: $nsys"
if (Get-Process -Name OpenPoseidon, PoseidonGame -ErrorAction SilentlyContinue) {
    throw 'An OpenPoseidon game is running. Close it first, then run this again.'
}
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Write-Host 'Not elevated: no CPU sampling and no Vulkan trace (use Terminal (Admin)).' -ForegroundColor Yellow }

New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Filter 'nsys*' -ErrorAction SilentlyContinue | Remove-Item -Force

# The traced binaries + PDBs, kept next to the trace: nsys_long_frames.py needs the exact files
# (--bin), and the game folder may be redeployed before the analysis runs.
$sym = Join-Path $Out 'bin-sym'
New-Item -ItemType Directory -Force $sym | Out-Null
foreach ($f in 'OpenPoseidon.exe', 'OpenPoseidon.pdb', 'wgpu_renderer.dll', 'wgpu_renderer.pdb', 'BUILT-FROM.txt', 'DEPLOYED-FROM.txt') {
    if (Test-Path (Join-Path $Build $f)) { Copy-Item (Join-Path $Build $f) $sym -Force }
}
if (-not (Test-Path (Join-Path $sym 'OpenPoseidon.pdb'))) {
    Write-Host 'No OpenPoseidon.pdb next to the traced exe: the trace will record, but stay unsymbolised.' -ForegroundColor Yellow
}
$exe = Join-Path $Build 'OpenPoseidon.exe'

$env:POSEIDON_FPS_CAP = '0'
$env:POSEIDON_VSYNC = '0'
$env:WGR_SLOW_BLOCK_MS = '45'
$env:WGR_RENDER_CALL_CPU_TIMINGS = '1'
$log = Join-Path $Out 'run.log'
# nsys takes literal quotes as part of a path, so quote only what has a space.
$q = { param($s) if ($s -match ' ') { '"' + $s + '"' } else { $s } }
$nsysArgs = @('profile', "--trace=$Trace", "--delay=$Delay", "--duration=$Duration", '--force-overwrite=true',
              '-o', (& $q (Join-Path $Out 'nsys')), (& $q $exe),
              '--render', 'wgpu', '--window', '--width', "$Width", '--height', "$Height", '-nosplash',
              '--timeout', "$Timeout", '--test-mission', (& $q $Mission), '--log-file', (& $q $log))
Write-Host "Running $(Split-Path $Mission -Leaf) under Nsight (the game window opens; leave it alone, ~$([math]::Ceiling($Timeout / 60 + 0.5)) min)..."
# nsys cannot start the game from a junction working directory; the game folder is a real one.
$p = Start-Process -FilePath $nsys -ArgumentList $nsysArgs -WorkingDirectory $Game -PassThru -Wait `
        -RedirectStandardError (Join-Path $Out 'stderr.txt') -RedirectStandardOutput (Join-Path $Out 'stdout.txt')
Write-Host "nsys exit $($p.ExitCode)"

$rep = Get-ChildItem $Out -Filter 'nsys*.nsys-rep' -ErrorAction SilentlyContinue | Where-Object Length -gt 0 | Select-Object -First 1
if (-not $rep) {
    Write-Host 'No trace was written. Last lines:' -ForegroundColor Red
    Get-Content (Join-Path $Out 'stderr.txt') -Tail 8 -ErrorAction SilentlyContinue
    exit 1
}
if ($SkipExport) { Write-Host "Trace (not exported): $($rep.FullName)"; exit 0 }
Write-Host 'Exporting to SQLite (a few minutes)...'
& $nsys export --type sqlite --force-overwrite true --output (Join-Path $Out 'nsys.sqlite') $rep.FullName
Write-Host "Done. $($rep.FullName) and nsys.sqlite" -ForegroundColor Green
