# Run under scripts/with-game-lock.sh against the installed matched binary pair.
[CmdletBinding()]
param(
    [string]$Tag = 'fixture',
    [ValidateRange(-1,1)][double]$Wetness = 1,
    [ValidateRange(0,1)][int]$Enabled = 1,
    [string]$Camera = '4697.60 3998.02 50.23 220.6 -5',
    [ValidateRange(0,1)][double]$Rain = 0,
    [ValidateRange(15,120)][int]$Seconds = 35,
    [switch]$Original
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run under scripts/with-game-lock.sh' }
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Game already running' }
$root = Split-Path $PSScriptRoot -Parent
$game = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
$out = Join-Path $root ("build/terrain-puddles/$Tag-" + (Get-Date -Format yyyyMMdd-HHmmss))
$user = Join-Path $out 'user'
New-Item -ItemType Directory -Force $user | Out-Null
$env:POSEIDON_USER_DIR = $user
$env:WGR_TERRAIN_PUDDLES = [string]$Enabled
$env:WGR_TERRAIN_PUDDLE_FIXTURE = '1'
$env:POSEIDON_TEST_RAIN = $Rain.ToString([Globalization.CultureInfo]::InvariantCulture)
if ($Wetness -ge 0) { $env:WGR_TERRAIN_PUDDLE_WETNESS = $Wetness.ToString([Globalization.CultureInfo]::InvariantCulture) }
else { Remove-Item Env:WGR_TERRAIN_PUDDLE_WETNESS -ErrorAction SilentlyContinue }
$world = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
$worldArgs = @()
if ($Original) {
    foreach ($key in @('POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM')) {
        Remove-Item ('Env:' + $key) -ErrorAction SilentlyContinue
    }
} else {
    if (!(Test-Path -LiteralPath $world)) { throw 'Native Reforger archives missing' }
    $env:POSEIDON_REFORGER_WORLD = 'worlds/eden'
    $env:POSEIDON_REFORGER_OBJECTS = '1'
    $env:POSEIDON_REFORGER_STREAM = '1'
    $worldArgs = @('--test-world',('"' + $world + '"'))
}
[IO.File]::WriteAllText((Join-Path $user 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$pose = @($Camera -split '\s+' | Where-Object { $_ })
if ($pose.Count -ne 5) { throw 'Camera must contain X Z altitude azimuth elevation' }
$exe = Join-Path $game 'OpenPoseidon.exe'
$dll = Join-Path $game 'wgpu_renderer.dll'
$log = Join-Path $out 'engine.log'
$metrics = Join-Path $out 'metrics.json'
$shots = @(0..5 | ForEach-Object { Join-Path $out "frame-$_.png" })
$spec = (0..5 | ForEach-Object {
    ($Seconds + $_ * 0.25).ToString([Globalization.CultureInfo]::InvariantCulture) + 's:' + $shots[$_]
}) -join ','
$argv = @('--check','--render=wgpu','--window','--width=1280','--height=720','--dev',
    '--test-mission',('"' + (Join-Path $root 'tests/perf/missions/perf_field.eden') + '"'),
    '--test-type','screenshot','--screenshot',('"' + (Join-Path $out 'fallback.png') + '"'),
    '--screenshot-delay','100000','--test-world-freefly') + $pose + $worldArgs + @(
    '--test-world-hour','16','--auto-screenshot',('"' + $spec + '"'),
    '--capture-metrics',('"' + $metrics + '"'),'--log-file',('"' + $log + '"'))
$hashes = @(Get-FileHash -LiteralPath $exe,$dll)
@{head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt'));
  hashes=$hashes;argv=$argv;wetness=$Wetness;rain=$Rain;enabled=$Enabled;original=[bool]$Original;
  snowTestDepth=$env:POSEIDON_SNOW_TEST_DEPTH;
  scope='Fixed-camera scheduled captures; does not establish actual rain fill/dry lifecycle or adjacent rendered-frame flicker.'
} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'provenance.json')
$p = $null
$forced = $false
try {
    $p = Start-Process -FilePath $exe -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $argv -PassThru
    $until = [DateTime]::UtcNow.AddSeconds($Seconds + 180)
    while (!$p.HasExited -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 250 }
    if (!$p.HasExited) { throw 'Owned puddle capture timed out' }
    $p.WaitForExit()
    $logText = Get-Content -LiteralPath $log -Raw
    if ($p.ExitCode -ne 0 -or $logText -notmatch 'Shutdown complete' -or
        $logText -notmatch 'wgpu renderer created' -or
        $logText -match 'panicked|UNHANDLED EXCEPTION|wgr_create failed|Validation Error|DeviceLost') { throw 'Puddle capture failed' }
    foreach ($shot in $shots) { if (!(Test-Path -LiteralPath $shot)) { throw "Missing $shot" } }
    $capture = Get-Content -LiteralPath $metrics -Raw | ConvertFrom-Json
    if (@($capture.gpu_timings_ms | Where-Object { $_.name -eq 'Terrain: main colour' -and $_.milliseconds -ge 0 }).Count -ne 1) {
        throw 'Terrain pass missing from capture'
    }
    $after = @(Get-FileHash -LiteralPath $exe,$dll)
    for ($i=0; $i -lt 2; $i++) { if ($after[$i].Hash -ne $hashes[$i].Hash) { throw 'Installed pair changed' } }
    @{passed=$true;exitCode=$p.ExitCode;normalShutdown=$true;forcedTermination=$false;shots=$shots;
      scope='Startup, terrain render and capture smoke; source eligibility and appearance require separate assessment.'
    } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'result.json')
    Write-Output $out
} finally {
    if ($p -and !$p.HasExited) { $forced=$true; Stop-Process -Id $p.Id -Force; $p.WaitForExit(5000) | Out-Null }
    if (!(Test-Path (Join-Path $out 'result.json'))) {
        @{passed=$false;forcedTermination=$forced} | ConvertTo-Json | Set-Content (Join-Path $out 'failure.json')
    }
}
