# Nsight Graphics GPU Trace of the parked Reforger Durras pose (owner's graphics.cfg, VSync off), the
# per-pass companion to Run-NsysFlight.ps1. Run from an ADMINISTRATOR PowerShell -- GPU Trace reads
# the GPU performance counters ("GPU Performance Counters unavailable" otherwise):
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\reforger-pc\Run-GpuTrace.ps1 [-Count 3]
# Each trace is one launch (~3.5 min): the game loads, holds the pose, and -DelaySec later traces
# -Frames frames; -Count launches start their traces 30 s apart so they land on different moments
# (the alpha-test pass flips between two cost levels for seconds at a time). Output:
# <harness>\shots\gt-<n>\ -- the .ngfx-gputrace (open in Nsight Graphics), BASE\*.xls (auto-export)
# and run.log, whose "Frame all regions" lines say which state the traced frames were in.
#
# WGR_GPU_DEBUG_LABELS=1 is set, so passes carry their wgpu labels and every GPU timer region is
# named (in-pass regions as debug groups: ObjColorAlpha, TerrainColor, ... -- gpu_timers.rs). It
# also names every buffer and texture, which costs CPU time but not GPU time.
#
# ngfx traps (each cost a round trip on 2026-10-23):
#  - never pass --platform: Qt reads it as its own option and dies with a "no Qt platform plugin"
#    popup;
#  - --output-dir must already exist;
#  - --args must hold NO quotes: ngfx re-quotes it and the game started with no arguments at all,
#    over the sea. Every path here is space-free (the Reforger addons by their 8.3 short name).
param(
    [int]$Count = 3,
    [int]$DelaySec = 95,
    [int]$Frames = 3,
    [string]$Harness = (Join-Path $env:LOCALAPPDATA 'Temp\rfh\_OpenPoseidon-Reforger'),
    [switch]$NoMetrics
)
$ErrorActionPreference = 'Stop'
$h = $Harness
$ngfx = Get-ChildItem 'C:\Program Files\NVIDIA Corporation', 'D:\Program Files\NVIDIA Corporation' -Directory `
            -Filter 'Nsight Graphics 20*' -ErrorAction SilentlyContinue | Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'host\windows-desktop-nomad-x64\ngfx.exe' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ngfx) { throw 'Nsight Graphics not found under C:\ or D:\Program Files\NVIDIA Corporation' }
if (Get-Process OpenPoseidon -ErrorAction SilentlyContinue) { throw 'Close the game first.' }
if (-not (Test-Path (Join-Path $h 'run\parity.eden\mission.sqm'))) { throw "No harness mission under $h (run go.ps1 once)" }

# A plain copy of the build: injectors do not follow the bin\ junction.
$bin = Get-Item (Join-Path $h 'bin')
$build = if ($bin.LinkType -eq 'Junction') { [string]($bin.Target | Select-Object -First 1) } else { $bin.FullName }
$copy = Join-Path $h 'bin-nsys'
New-Item -ItemType Directory -Force $copy | Out-Null
foreach ($f in 'OpenPoseidon.exe', 'OpenPoseidon.pdb', 'wgpu_renderer.dll', 'wgpu_renderer.pdb', 'nvngx_dlss.dll',
               'OpenAL32.dll', 'PoseidonFormats.dll', 'BUILT-FROM.txt') {
    if (Test-Path (Join-Path $build $f)) { Copy-Item (Join-Path $build $f) $copy -Force }
}
foreach ($j in 'assets', 'Mods') {
    $link = Join-Path $copy $j
    if (Test-Path $link) { (Get-Item $link).Delete() }
    if (Test-Path (Join-Path $build $j)) { New-Item -ItemType Junction -Path $link -Target (Join-Path $build $j) | Out-Null }
}

# A mission that holds the pose long enough for every trace.
$mis = Join-Path $h 'run\gputrace.eden'
New-Item -ItemType Directory -Force $mis | Out-Null
Copy-Item (Join-Path $h 'run\parity.eden\mission.sqm') $mis -Force
"~3`n_r = triFreeFlyPose ""8595.2 3069.61 234.15 145.0 -12.0""`n~600`ntriEndGame`n" | Set-Content (Join-Path $mis 'init.sqs') -Encoding ascii

$user = Join-Path $h 'user'
$cfg = Join-Path $user 'graphics.cfg'
if (Test-Path $cfg) { Copy-Item $cfg "$cfg.gt-bak" -Force }
(Get-Content (Join-Path $env:APPDATA 'CWR\graphics.cfg')) -replace 'vsync=1;', 'vsync=0;' | Set-Content $cfg
$game = [string]((Get-Item (Join-Path (Split-Path $h -Parent) 'Remastered')).Target | Select-Object -First 1)
$fso = New-Object -ComObject Scripting.FileSystemObject
$addons = $fso.GetFolder('C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons').ShortPath
try {
    for ($i = 1; $i -le $Count; $i++) {
        $out = Join-Path $h "shots\gt-$i"
        New-Item -ItemType Directory -Force $out | Out-Null
        Get-ChildItem $out -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
        $log = Join-Path $out 'run.log'
        foreach ($pth in $mis, $log, $addons, $user, $out) { if ($pth -match ' ') { throw "path with a space: $pth" } }
        $gargs = "--window --render=wgpu --width=3440 --height=1440 --test-mission $mis --test-world $addons " +
                 "--reforger-world worlds/eden --test-world-freefly 8595.2 3069.61 400 145.0 -12.0 " +
                 "--test-world-hour 11.18 --log-file $log --log-level info"
        $envs = "POSEIDON_USER_DIR=$user; POSEIDON_REFORGER_WORLD=worlds/eden; POSEIDON_REFORGER_OBJECTS=1; " +
                "POSEIDON_REFORGER_STREAM=1; POSEIDON_VSYNC=0; POSEIDON_TEST_OVERCAST=0; POSEIDON_FREEFLY_FOV=0.7; " +
                "POSEIDON_TEST_DATE=1989-08-14; WGR_GPU_REGIONS_ALL=1; WGR_GPU_DEBUG_LABELS=1; TRI_OUTPUT_DIR=$out;"
        $delayMs = ($DelaySec + 30 * ($i - 1)) * 1000
        $al = @('--activity', 'GPU Trace Profiler', '--exe', (Join-Path $copy 'OpenPoseidon.exe'), '--dir', $game,
                '--args', $gargs, '--env', $envs, '--start-after-ms', "$delayMs", '--limit-to-frames', "$Frames",
                '--max-duration-ms', '3000', '--auto-export', '--output-dir', $out, '--no-timeout')
        if (-not $NoMetrics) {
            $al += @('--architecture', 'Ampere GA10x', '--metric-set-name', 'Top-Level Triage', '--multi-pass-metrics')
        }
        Write-Host "GPU trace $i of $Count (at $([int]($delayMs / 1000)) s) -- the game window opens; leave it alone..."
        & $ngfx @al *> (Join-Path $out 'ngfx.log')
        Get-Process OpenPoseidon -ErrorAction SilentlyContinue | Stop-Process -Force
        Start-Sleep 5
    }
} finally {
    if (Test-Path "$cfg.gt-bak") { Copy-Item "$cfg.gt-bak" $cfg -Force }
    Get-Process OpenPoseidon -ErrorAction SilentlyContinue | Stop-Process -Force
}
Write-Host "All $Count GPU traces done: $h\shots\gt-1..gt-$Count"
