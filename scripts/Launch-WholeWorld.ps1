<#
.SYNOPSIS
    Opt-in Earth elevation streaming preview. Downloads public Terrain Tiles
    into a bounded local cache. Optional prepared Worldgen assets; freefly only.
#>
[CmdletBinding()]
param(
    [string]$GameDir = 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
    [ValidateRange(-84,84)][double]$Latitude = 46.6,
    [ValidateRange(-180,180)][double]$Longitude = 8.1,
    [ValidateRange(500,30000000)][double]$Altitude = 4500,
    [switch]$Globe,
    [switch]$CachedBuildings,
    [switch]$CachedFeatures,
    [switch]$LocalSurface,
    [string]$WorldgenManifest = '',
    [string]$WorldgenAddonRoot = '',
    [string]$ContentUrlTemplate = '',
    [string]$CacheDirectory = (Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
    [switch]$Fullscreen,
    [int]$HarnessPort = 0,
    [string]$LogFile = '',
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if ($Globe) { $Altitude = 18000000 }
if (!$PrepareOnly -and !$env:LOCK_OWNER) {
    $bash = 'C:/Program Files/Git/bin/bash.exe'
    $pwsh = (Get-Process -Id $PID).Path
    $forward = @('-NoProfile','-File',$PSCommandPath,'-GameDir',$GameDir,
        '-Latitude',$Latitude.ToString('R',[Globalization.CultureInfo]::InvariantCulture),
        '-Longitude',$Longitude.ToString('R',[Globalization.CultureInfo]::InvariantCulture),
        '-Altitude',$Altitude.ToString('R',[Globalization.CultureInfo]::InvariantCulture),
        '-CacheDirectory',$CacheDirectory,'-HarnessPort',"$HarnessPort")
    if ($Fullscreen) { $forward += '-Fullscreen' }
    if ($Globe) { $forward += '-Globe' }
    if ($CachedBuildings) { $forward += '-CachedBuildings' }
    if ($CachedFeatures) { $forward += '-CachedFeatures' }
    if ($ContentUrlTemplate) { $forward += @('-ContentUrlTemplate',$ContentUrlTemplate) }
    if ($LocalSurface) { $forward += '-LocalSurface' }
    if ($WorldgenManifest) { $forward += @('-WorldgenManifest',$WorldgenManifest) }
    if ($WorldgenAddonRoot) { $forward += @('-WorldgenAddonRoot',$WorldgenAddonRoot) }
    if ($LogFile) { $forward += @('-LogFile',$LogFile) }
    $previous = $env:LOCK_OWNER
    try {
        $env:LOCK_OWNER = 'Earth terrain streaming preview'
        & $bash (Join-Path $repo 'scripts/with-game-lock.sh') $pwsh @forward
        if ($LASTEXITCODE -ne 0) { throw "Earth preview exited with $LASTEXITCODE" }
    } finally { $env:LOCK_OWNER = $previous }
    return
}
if (Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue) { throw 'Close the existing game before starting this preview.' }
if ($WorldgenManifest) {
    $LocalSurface=$true
    $WorldgenManifest=(Resolve-Path -LiteralPath $WorldgenManifest).Path
    if (!(Test-Path -LiteralPath $WorldgenManifest -PathType Leaf)) { throw 'Worldgen manifest must be a file.' }
    if ($CachedBuildings -or $CachedFeatures -or $ContentUrlTemplate) {
        throw 'Use Worldgen assets or simplified OSM previews separately to avoid duplicate geometry.'
    }
}
if ($WorldgenAddonRoot) {
    if (!$WorldgenManifest) { throw 'WorldgenAddonRoot requires WorldgenManifest.' }
    $WorldgenAddonRoot=(Resolve-Path -LiteralPath $WorldgenAddonRoot).Path
    if (!(Test-Path -LiteralPath $WorldgenAddonRoot -PathType Container)) { throw 'Worldgen addon root must be a directory.' }
    if (!(Get-ChildItem -LiteralPath $WorldgenAddonRoot -File -Filter '*.pbo')) {
        $addonDirectory=Join-Path $WorldgenAddonRoot 'AddOns'
        if (!(Test-Path -LiteralPath $addonDirectory -PathType Container) -or
            !(Get-ChildItem -LiteralPath $addonDirectory -File -Filter '*.pbo')) {
            throw 'Worldgen addon root must contain PBO archives or an AddOns directory with PBO archives.'
        }
        $WorldgenAddonRoot=$addonDirectory
    }
}
$fixture = Join-Path $CacheDirectory 'fixture'
$mission = Join-Path $fixture 'earth-preview.abel'
$null = New-Item -ItemType Directory -Force -Path $mission
# Authored empty RVW4 container: no copied retail world, models or placements.
$world = Join-Path $fixture 'earth-preview.wrp'
$data = [byte[]]::new(12 + 256*256*4 + 512*32 + 128)
[Text.Encoding]::ASCII.GetBytes('4WVR').CopyTo($data,0)
[BitConverter]::GetBytes([uint32]256).CopyTo($data,4)
[BitConverter]::GetBytes([uint32]256).CopyTo($data,8)
$low = [BitConverter]::GetBytes([int16]-100)
for ($i=0;$i -lt 256*256;$i++) { $low.CopyTo($data,12+2*$i); $data[12+256*256*2+2*$i]=1 }
[Text.Encoding]::ASCII.GetBytes('landtext\mo.pac').CopyTo($data,12+256*256*4)
[Text.Encoding]::ASCII.GetBytes('o\pt.paa').CopyTo($data,12+256*256*4+32)
[IO.File]::WriteAllBytes($world,$data)
[IO.File]::WriteAllText((Join-Path $mission 'mission.sqm'), @'
version=11;
class Mission {
 randomSeed=42;
 class Intel {year=1985;month=6;day=21;hour=12;minute=0;};
 class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=1;class Item0 {
  position[]={6400,0,6400};id=0;side="WEST";vehicle="SoldierWB";
  player="PLAYER COMMANDER";leader=1;skill=1;init="this allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0";
 };};};};
};
class Intro {randomSeed=1;};class OutroWin {randomSeed=1;};class OutroLoose {randomSeed=1;};
'@)
[IO.File]::WriteAllText((Join-Path $CacheDirectory 'DATA-SOURCES.txt'), @'
Open Poseidon Engine Earth elevation streaming prototype.
Downloaded terrain is an external local cache, not bundled engine content.
Provider: https://registry.opendata.aws/terrain-tiles/
Format: Mapzen Terrarium, elevation in metres. Zoom 10 near, adaptive coarse horizon.
Source URL: https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png
Terrain Tiles source acknowledgements and individual source terms:
https://github.com/tilezen/joerd/blob/master/docs/attribution.md
EU terrain includes Copernicus EU-DEM; worldwide coverage combines multiple sources.
Retain/check the source acknowledgements before redistributing downloaded data.
Cache: at most 256 validated PNG tiles (2 MiB each maximum), 64 decoded in memory.
Freefly rendering with a curved horizon. Optional prepared regional Worldgen models.
No worldwide populated source, collision, navigation or simulation.
Global overview: NASA Blue Marble Next Generation, June 2004, NASA Earth Observatory.
https://science.nasa.gov/earth/earth-observatory/blue-marble-next-generation/base-map/
https://www.nasa.gov/nasa-brand-center/images-and-media/
Image is cached locally; no NASA endorsement. Overview is not worldwide gameplay.
'@)
if ($PrepareOnly) { Write-Output $world; return }
$imagery=Join-Path $CacheDirectory 'blue-marble-june-5400.jpg'
if (!(Test-Path -LiteralPath $imagery)) {
    # Bound the download before decoding, and publish only a completed JPEG.
    $temporary=$imagery+'.partial'
    $http=[Net.Http.HttpClient]::new();$http.Timeout=[TimeSpan]::FromSeconds(60)
    $response=$null;$inputStream=$null;$outputStream=$null
    try {
        $url='https://assets.science.nasa.gov/content/dam/science/esd/eo/images/bmng/bmng-base/june/world.200406.3x5400x2700.jpg'
        $response=$http.GetAsync($url,[Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
        $null=$response.EnsureSuccessStatusCode()
        if ($response.Content.Headers.ContentLength -gt 16MB) { throw 'Earth imagery exceeds 16 MiB' }
        $inputStream=$response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $outputStream=[IO.File]::Create($temporary)
        $buffer=[byte[]]::new(65536);$total=0
        $deadline=[DateTime]::UtcNow.AddSeconds(60)
        while (($count=$inputStream.Read($buffer,0,$buffer.Length)) -gt 0) {
            $total+=$count
            if ($total -gt 16MB -or [DateTime]::UtcNow -gt $deadline) { throw 'Earth imagery download limit' }
            $outputStream.Write($buffer,0,$count)
        }
        $outputStream.Dispose();$outputStream=$null
        $jpeg=[IO.File]::ReadAllBytes($temporary)
        if ($jpeg.Length -lt 4 -or $jpeg[0] -ne 255 -or $jpeg[1] -ne 216) { throw 'Earth imagery is not JPEG' }
        Move-Item -LiteralPath $temporary -Destination $imagery -Force
    } catch { Write-Warning "Imagery unavailable; using plain globe: $_" }
    finally {
        if ($outputStream) { $outputStream.Dispose() };if ($inputStream) { $inputStream.Dispose() }
        if ($response) { $response.Dispose() };$http.Dispose()
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}
$settings = @{
    POSEIDON_WHOLE_WORLD='1'; POSEIDON_EARTH_LATITUDE=$Latitude.ToString('R',[Globalization.CultureInfo]::InvariantCulture);
    POSEIDON_EARTH_LONGITUDE=$Longitude.ToString('R',[Globalization.CultureInfo]::InvariantCulture);
    POSEIDON_EARTH_CACHE=$CacheDirectory; POSEIDON_EARTH_IMAGERY=$imagery;
    WGR_GRASS='0'; WGR_TERRAIN_RESOURCE_REUSE='1';
    POSEIDON_EARTH_BUILDINGS=$(if($CachedBuildings -or $ContentUrlTemplate){'1'}else{'0'});
    POSEIDON_EARTH_FEATURES=$(if($CachedFeatures -or $ContentUrlTemplate){'1'}else{'0'});
    POSEIDON_EARTH_CONTENT_URL=$ContentUrlTemplate;
    POSEIDON_EARTH_WORLDGEN_MANIFEST=$WorldgenManifest;
    POSEIDON_EARTH_SURFACE=$(if($LocalSurface){'1'}else{'0'})
}
$saved = @{}
foreach ($key in $settings.Keys) { $saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process'); [Environment]::SetEnvironmentVariable($key,$settings[$key],'Process') }
try {
    $argv = @('--render=wgpu','--dev','--width','1600','--height','900','--vd','20000',
        '--test-mission',$mission,'--test-world',$world,'--test-world-freefly','6400','6400',
        $Altitude.ToString('R',[Globalization.CultureInfo]::InvariantCulture),'30',
        $(if($Globe){'-89.9'}else{'-20'}),'--test-world-hour','12')
    if (!$Fullscreen) { $argv += '--window' }
    if ($HarnessPort) { $argv += @('--harness',"$HarnessPort") }
    if ($WorldgenAddonRoot) { $argv += @('--addon-root',$WorldgenAddonRoot) }
    if (!$LogFile) { $LogFile = Join-Path $CacheDirectory 'earth-preview.log' }
    $argv += @('--log-file',$LogFile)
    Write-Host "Earth preview at $Latitude, $Longitude. Freefly only. Cache: $CacheDirectory"
    Push-Location $GameDir
    try { & (Join-Path $GameDir 'OpenPoseidon.exe') @argv; if ($LASTEXITCODE -ne 0) { throw "Game exit: $LASTEXITCODE" } }
    finally { Pop-Location }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
