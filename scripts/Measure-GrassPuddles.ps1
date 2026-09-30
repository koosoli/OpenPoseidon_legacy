# Same installed binary, alternating arms, single end capture per run.
[CmdletBinding()]
param(
    [ValidateSet('Grass','Puddles')][string]$Scene = 'Grass',
    [ValidateRange(2,8)][int]$Pairs = 3,
    [ValidateRange(30,180)][int]$WarmupSeconds = 40
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Use scripts/with-game-lock.sh' }
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Game already running' }
$root = Split-Path $PSScriptRoot -Parent
$game = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
$stamp = Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw
$hashes = @(Get-FileHash -LiteralPath (Join-Path $game 'OpenPoseidon.exe'),(Join-Path $game 'wgpu_renderer.dll'))
$out = Join-Path $root ("build/grass-puddle-perf/$Scene-" + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $out | Out-Null
@{installed=$stamp;hashes=$hashes;head=(& git -C $root rev-parse HEAD);scene=$Scene;pairs=$Pairs;
  scope='Alternating single-capture runs, fixed profile and camera; GPU regions are final snapshots, CPU p95 comes from the frame ring. Screenshot maximum is excluded.'
} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'provenance.json')
$rows = @()
for ($pair=1; $pair -le $Pairs; $pair++) {
    # Reverse order every other pair to expose warm-cache/thermal drift.
    $arms = if ($pair % 2) { @('off','on') } else { @('on','off') }
    foreach ($arm in $arms) {
        $label = "$arm-$pair"
        $user = Join-Path $out "user-$label"
        New-Item -ItemType Directory -Force $user | Out-Null
        & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0
        $settings = @{POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0'; WGR_TERRAIN_PUDDLES='1';
            WGR_GRASS_FILTER_SUBPIXEL='1'; POSEIDON_TEST_RAIN='0'; WGR_TERRAIN_PUDDLE_WETNESS='0'}
        $benchmarkArgs = @{Label=$label;Out=$out;Env=$settings;Repeats=1;Mission='tests/perf/missions/perf_field.eden';
            Width=1280;Height=720;Windowed=$true;WarmupSeconds=$WarmupSeconds;TimeoutSeconds=($WarmupSeconds+180);RequireAll=$true}
        if ($Scene -eq 'Grass') {
            $settings.WGR_GRASS_FILTER_SUBPIXEL = if ($arm -eq 'on') {'1'} else {'0'}
            # ExtraArgs intentionally apply freefly to the mission's stock world.
            $benchmarkArgs.ExtraArgs = @('--dev','--test-world-freefly','5018.51','4087.66','117.58','220.8','-86.0','--test-world-hour','12')
        } else {
            $settings.WGR_TERRAIN_PUDDLE_WETNESS = if ($arm -eq 'on') {'1'} else {'0'}
            $settings.POSEIDON_REFORGER_WORLD='worlds/eden'; $settings.POSEIDON_REFORGER_OBJECTS='1'; $settings.POSEIDON_REFORGER_STREAM='1'
            $benchmarkArgs.World='C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
            $benchmarkArgs.Freefly=@(9481.25,2994.25,209.72,0,-18); $benchmarkArgs.WorldHour=16; $benchmarkArgs.ExtraArgs=@('--dev')
        }
        & "$PSScriptRoot/farfield-bench.ps1" @benchmarkArgs
        if ($LASTEXITCODE -ne 0) { throw "Benchmark $label failed" }
        $stem = Join-Path (Join-Path $out $label) 'run-01'
        & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath "$stem.meta.json" -LogPath "$stem.log"
        $m = Get-Content "$stem.json" -Raw | ConvertFrom-Json
        if ($m.build.render_width -ne 1280 -or $m.build.render_height -ne 720 -or
            $m.build.msaa_samples -ne 4 -or $m.build.dlss_active -or
            $m.cpu_frame_phases_ms.sampled_frames -lt 256) { throw "Incomplete benchmark profile/ring: $label" }
        if ($Scene -eq 'Grass' -and ($m.grass.near_instances + $m.grass.mid_instances) -le 0) { throw 'Empty grass scene' }
        if ($Scene -eq 'Puddles' -and ($m.streaming.preparer.queued -gt 0 -or $m.streaming.preparer.ready -gt 0)) { throw 'Native preparer has not settled' }
        $gpu = @{}
        foreach ($name in @('Grass colour','Grass prepass','Terrain: main colour')) {
            $timing = @($m.gpu_timings_ms | Where-Object {$_.name -eq $name -and $_.milliseconds -ge 0})
            if ($timing.Count -ne 1) { throw "Missing $name" }
            $gpu[$name] = $timing[0].milliseconds
        }
        $rows += @{arm=$arm;pair=$pair;gpu=$gpu;grass=$m.grass;objects=$m.objects;
            cpu=$m.cpu_frame_phases_ms;build=$m.build;metrics="$stem.json"}
    }
}
$after = @(Get-FileHash -LiteralPath (Join-Path $game 'OpenPoseidon.exe'),(Join-Path $game 'wgpu_renderer.dll'))
if ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw) -ne $stamp) { throw 'Installed stamp changed' }
for ($i=0; $i -lt 2; $i++) { if ($hashes[$i].Hash -ne $after[$i].Hash) { throw 'Installed binary changed' } }
@{scene=$Scene;installed=$stamp;runs=$rows;scope='Compare distributions and medians across alternating arms; final GPU snapshots are not a whole-run GPU average.'} |
    ConvertTo-Json -Depth 12 | Set-Content (Join-Path $out 'results.json')
Write-Output $out
