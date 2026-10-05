# Run under scripts/with-game-lock.sh. Uses the installed matched binary pair.
[CmdletBinding()]
param(
    [ValidateSet('0','1')][string]$Filter = '1',
    [ValidateSet('0','1')][string]$MediumLighting = '1',
    [ValidateSet('Aerial','Close','OwnerEveron')][string]$View = 'Aerial',
    [string]$Tag = 'grass',
    [int]$Msaa = 4,
    [float]$Altitude = -1,
    [ValidateRange(-90,90)][float]$Elevation = 0,
    [switch]$WindWaves
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh.' }
$root = Split-Path $PSScriptRoot -Parent
$game = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault'
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Game already running' }
$out = Join-Path $root ("build/grass-stability/$Tag-$View-$Filter-" + (Get-Date -Format yyyyMMdd-HHmmss))
$profile = Join-Path $out 'user'
New-Item -ItemType Directory -Force $profile | Out-Null
$env:POSEIDON_USER_DIR = $profile
$env:WGR_GRASS_FILTER_SUBPIXEL = $Filter
$env:WGR_GRASS_MEDIUM_LIGHTING = $MediumLighting
if ($WindWaves) { $env:POSEIDON_WIND_OVERRIDE = '8 220 0.7' }
[IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=$Msaa;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$exe = Join-Path $game 'OpenPoseidon.exe'
$dll = Join-Path $game 'wgpu_renderer.dll'
$mission = Join-Path $game 'dev-missions/devtest-dawn.abel'
if ($View -eq 'OwnerEveron') { $mission = Join-Path $root 'tests/perf/missions/perf_field.eden' }
$log = Join-Path $out 'engine.log'
$metrics = Join-Path $out 'metrics.json'
$pose = if ($View -eq 'Aerial') { @('5058.75','3902.55','110','0','-80') } else { @('5058.75','3902.55','73','0','-18') }
if ($View -eq 'OwnerEveron') { $pose = @('5018.51','4087.66','117.58','220.8','-86.0') }
if ($Altitude -ge 0) { $pose[2] = $Altitude.ToString([Globalization.CultureInfo]::InvariantCulture) }
if ($PSBoundParameters.ContainsKey('Elevation')) { $pose[4] = $Elevation.ToString([Globalization.CultureInfo]::InvariantCulture) }
$frames = if ($WindWaves) { @(300,360,420,480,540,600) } else { @(300..305) }
$shots = @($frames | ForEach-Object { Join-Path $out ("frame-$_.png") })
$spec = (0..5 | ForEach-Object { "$($frames[$_]):$($shots[$_])" }) -join ','
$argv = @('--check','--render=wgpu','--window','--width=1280','--height=720','--dev',
    '--test-mission',('"' + $mission + '"'),'--test-type','screenshot',
    '--screenshot',('"' + (Join-Path $out 'fallback.png') + '"'),'--screenshot-delay','1000',
    '--test-world-freefly') + $pose + @('--test-world-hour','12',
    '--auto-screenshot',('"' + $spec + '"'),'--capture-metrics',('"' + $metrics + '"'),
    '--log-file',('"' + $log + '"'))
$hashes = @(Get-FileHash -LiteralPath $exe,$dll)
@{head=(& git -C $root rev-parse HEAD);installed=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt'));
    hashes=$hashes;argv=$argv;filter=$Filter;mediumLighting=$MediumLighting;view=$View;msaa=$Msaa;
    windWaveSamples=[bool]$WindWaves;
    scope='Scheduled captures, not a guarantee of adjacent rendered frames; native resolution, isolated profile.'
} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'provenance.json')
$p = $null
$forced = $false
try {
    $p = Start-Process -FilePath $exe -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $argv -PassThru
    $until = [DateTime]::UtcNow.AddSeconds(120)
    while (!$p.HasExited -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 250 }
    if (!$p.HasExited) { throw 'Owned grass capture timed out' }
    $p.WaitForExit()
    $logText = Get-Content -LiteralPath $log -Raw
    if ($p.ExitCode -ne 0 -or $logText -notmatch 'Shutdown complete' -or
        $logText -notmatch 'wgpu renderer created' -or
        $logText -match 'panicked|UNHANDLED EXCEPTION|wgr_create failed|Validation Error|DeviceLost') { throw 'Grass capture failed' }
    foreach ($shot in $shots) { if (!(Test-Path -LiteralPath $shot)) { throw "Missing $shot" } }
    $capture = Get-Content -LiteralPath $metrics -Raw | ConvertFrom-Json
    if (($capture.grass.near_instances + $capture.grass.mid_instances) -le 0) {
        throw 'Capture contains no near or mid grass; fix the camera before comparing shading'
    }
    $after = @(Get-FileHash -LiteralPath $exe,$dll)
    for ($i=0; $i -lt 2; $i++) { if ($after[$i].Hash -ne $hashes[$i].Hash) { throw 'Installed pair changed' } }
    @{passed=$true;exitCode=$p.ExitCode;normalShutdown=$true;forcedTermination=$false;shots=$shots;
      scope='Startup and fixed-camera capture only; assess grass population and temporal image differences separately.'
    } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'result.json')
    Write-Output $out
} finally {
    if ($p -and !$p.HasExited) { $forced=$true; Stop-Process -Id $p.Id -Force; $p.WaitForExit(5000) | Out-Null }
    if (!(Test-Path (Join-Path $out 'result.json'))) {
        @{passed=$false;forcedTermination=$forced} | ConvertTo-Json | Set-Content (Join-Path $out 'failure.json')
    }
}
