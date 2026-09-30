<#
.SYNOPSIS
Capture native PlantMat grass with and without footprint filtering.
.DESCRIPTION
Run through scripts/with-game-lock.sh against an installed matched pair. Uses
farfield-bench's owned-process lifecycle, six scheduled captures at 35..36.25 s,
and a fresh native-resolution profile. These are scheduled samples, not proven
adjacent rendered frames, and multiple readbacks are not a performance run.
.EXAMPLE
scripts/with-game-lock.sh powershell.exe -NoProfile -File scripts/Test-NativeGrassStability.ps1 -Filter 1 -View Aerial -Wind '12 90 0.7'
#>
[CmdletBinding()]
param(
    [ValidateSet('0','1')][string]$Filter = '1',
    [ValidateSet('Close','Aerial')][string]$View = 'Close',
    [string]$Camera = '',
    # Empty uses the mission's actual wind, even if the parent has an override.
    [string]$Wind = '',
    [ValidatePattern('^[A-Za-z0-9_.-]+$')][string]$Tag = 'native-grass',
    [string]$GameDir = 'D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
    [string]$ReforgerAddons = 'C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Run through scripts/with-game-lock.sh' }
$root = Split-Path $PSScriptRoot -Parent

function Read-FiniteTokens {
    param([string]$Text, [int]$Count, [string]$Name)
    $tokens = @($Text.Trim() -split '\s+' | Where-Object { $_ })
    if ($tokens.Count -ne $Count) { throw "$Name requires exactly $Count numeric tokens" }
    $values = @()
    foreach ($token in $tokens) {
        $number = 0.0
        if (![double]::TryParse($token, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
            [double]::IsNaN($number) -or [double]::IsInfinity($number)) {
            throw "$Name contains a non-finite or invalid number: $token"
        }
        $values += $number
    }
    return $values
}

if ([string]::IsNullOrWhiteSpace($Camera)) {
    $Camera = if ($View -eq 'Close') { '9481.25 2994.25 209.72 0 -18' }
              else { '9481.25 3006.25 244.72 0 -80' }
}
[double[]]$pose = Read-FiniteTokens -Text $Camera -Count 5 -Name Camera
$windOverride = ''
if (![string]::IsNullOrWhiteSpace($Wind)) {
    $windTokens = Read-FiniteTokens -Text $Wind -Count 3 -Name Wind
    for ($i=0; $i -lt 3; $i++) {
        if ($windTokens[$i] -lt 0 -or $windTokens[$i] -gt @(30,360,1)[$i]) {
            throw 'Wind bounds are speed 0..30 m/s, bearing 0..360 degrees, gustiness 0..1'
        }
    }
    $windOverride = ($windTokens | ForEach-Object {
        $_.ToString('R', [Globalization.CultureInfo]::InvariantCulture)
    }) -join ' '
}
if (!(Test-Path -LiteralPath $ReforgerAddons -PathType Container)) { throw 'Native Reforger archives missing' }
if ([string]::IsNullOrWhiteSpace($Out)) { $Out = Join-Path $root 'build/native-grass-stability' }
if (![IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
$Out = [IO.Path]::GetFullPath($Out)
$label = "$Tag-$View-filter$Filter-" + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
$captureDir = Join-Path $Out $label
$user = Join-Path $captureDir 'user'
if (Test-Path -LiteralPath $captureDir) { throw "Capture directory already exists: $captureDir" }
New-Item -ItemType Directory -Path $user -Force | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0 -MsaaSamples 4

$pairPaths = @('OpenPoseidon.exe','wgpu_renderer.dll','DEPLOYED-FROM.txt') | ForEach-Object { Join-Path $GameDir $_ }
$stamp = Get-Content -LiteralPath $pairPaths[2] -Raw
if ([string]::IsNullOrWhiteSpace($stamp)) { throw 'Installed pair has an empty deployment stamp' }
$beforeHashes = @(Get-FileHash -LiteralPath $pairPaths -Algorithm SHA256)
# Serialize scalar records, avoiding PowerShell 5.1's traversal of FileHash
# extended properties (including provider metadata) at deeper JSON levels.
$hashRecords = @($beforeHashes | ForEach-Object {
    @{path=[string]$_.Path; sha256=[string]$_.Hash; algorithm='SHA256'}
})
$inheritedWind = [Environment]::GetEnvironmentVariable('POSEIDON_WIND_OVERRIDE', 'Process')
$settings = @{
    POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0';
    POSEIDON_REFORGER_WORLD='worlds/eden'; POSEIDON_REFORGER_OBJECTS='1'; POSEIDON_REFORGER_STREAM='1';
    WGR_NATIVE_TEXTURE_KEYS='1'; WGR_LOD_GOVERNOR_RANGE='1';
    WGR_GRASS_FILTER_SUBPIXEL=$Filter; POSEIDON_WIND_OVERRIDE=$windOverride;
    POSEIDON_TEST_RAIN='0'; WGR_TERRAIN_PUDDLES='0'; WGR_TERRAIN_PUDDLE_WETNESS='0'
}
$provenance = [ordered]@{
    head=[string](& git -C $root rev-parse HEAD); installed=[string]$stamp; hashes=$hashRecords;
    filter=$Filter; view=$View; camera=$pose; wind=$windOverride; inheritedWind=$inheritedWind;
    width=1280; height=720; msaa=4; warmupSeconds=35; samples=6; intervalSeconds=0.25;
    settings=$settings;
    scope='Fixed-camera scheduled samples; not guaranteed adjacent rendered frames or comparable GPU timing.'
}
$provenance | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $captureDir 'provenance.json') -Encoding UTF8

$benchmarkArgs = @{
    Label=$label; Mission='tests/perf/missions/perf_field.eden'; World=$ReforgerAddons;
    Freefly=$pose; WorldHour=16; Out=$Out; Env=$settings; Repeats=1; GameDir=$GameDir;
    Width=1280; Height=720; Windowed=$true; WarmupSeconds=35; TimeoutSeconds=215;
    MotionSamples=6; MotionInterval=0.25; RequireAll=$true; ExtraArgs=@('--dev')
}
& "$PSScriptRoot/farfield-bench.ps1" @benchmarkArgs
if ($LASTEXITCODE -ne 0) { throw 'Native grass capture failed; inspect farfield lifecycle metadata' }
if ([Environment]::GetEnvironmentVariable('POSEIDON_WIND_OVERRIDE', 'Process') -cne $inheritedWind) {
    throw 'Capture did not restore the inherited wind environment'
}
$stem = Join-Path $captureDir 'run-01'
& "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath "$stem.meta.json" -LogPath "$stem.log"
$metrics = Get-Content -LiteralPath "$stem.json" -Raw | ConvertFrom-Json
if ($metrics.build.render_width -ne 1280 -or $metrics.build.render_height -ne 720 -or
    $metrics.build.msaa_samples -ne 4 -or $metrics.build.dlss_active -ne $false) {
    throw 'Capture did not use native 1280x720 MSAA4 with DLSS disabled'
}
foreach ($name in @('near_instances','mid_instances')) {
    if ($null -eq $metrics.grass.$name -or $metrics.grass.$name -le 0) {
        throw "Native grass capture has no populated $name; adjust the camera before comparing"
    }
}
if (!(Select-String -LiteralPath "$stem.log" -Pattern 'near blades from Reforger PlantMat' -Quiet)) {
    throw 'Native PlantMat blade source was not used'
}
$filterStatus = if ($Filter -eq '1') { 'true' } else { 'false' }
$filterProof = "Wgpu grass subpixel shading: $filterStatus (WGR_GRASS_FILTER_SUBPIXEL)"
if (!(Select-String -LiteralPath "$stem.stderr.txt" -SimpleMatch -Quiet -Pattern $filterProof)) {
    throw 'Renderer did not confirm the requested grass filtering mode'
}
$preparer = $metrics.streaming.preparer
if ($preparer.valid -ne $true) { throw 'Native preparer telemetry is unavailable' }
foreach ($name in @('queued','ready','parse_reserved_bytes','ready_payload_bytes')) {
    if ($null -eq $preparer.$name -or $preparer.$name -ne 0) {
        throw "Native preparer is not idle ($name); allow more settling before comparing"
    }
}
$shots = @(0..4 | ForEach-Object { "$stem.motion-{0:D2}.png" -f $_ }) + @("$stem.png")
foreach ($shot in $shots) {
    if (!(Test-Path -LiteralPath $shot -PathType Leaf) -or (Get-Item -LiteralPath $shot).Length -lt 24) {
        throw "Missing or empty scheduled capture: $shot"
    }
}
$afterHashes = @(Get-FileHash -LiteralPath $pairPaths -Algorithm SHA256)
for ($i=0; $i -lt $beforeHashes.Count; $i++) {
    if ($afterHashes[$i].Hash -ne $beforeHashes[$i].Hash) { throw 'Installed pair or deployment stamp changed during capture' }
}
if ((Get-Content -LiteralPath $pairPaths[2] -Raw) -cne $stamp) { throw 'Deployment stamp changed during capture' }
@{passed=$true; normalShutdown=$true; stableInstalledPair=$true; nativePlantMat=$true;
    grass=$metrics.grass; preparer=$preparer; shots=$shots; filter=$Filter; view=$View;
    scope=$provenance.scope
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $captureDir 'result.json') -Encoding UTF8
Write-Output $captureDir
