param([Parameter(Mandatory)][string]$UserDir,
      [ValidateSet(-1,0,1,2)][int]$DlssMode = 0,
      [ValidateSet(1,2,4,8)][int]$MsaaSamples = 4)
$ErrorActionPreference = 'Stop'
$path = Join-Path $UserDir 'graphics.cfg'
if ([IO.Path]::GetFullPath($path).Length -ge 260) {
    throw 'Benchmark graphics.cfg exceeds the legacy engine path limit; use a shorter isolated UserDir'
}
if (!(Test-Path -LiteralPath $UserDir -PathType Container)) { throw 'Create the isolated benchmark user directory first' }
if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite an existing graphics profile: $path" }
# Match the previously migrated Ultra image explicitly, but preserve Unlimited.
# GraphicsConfig::kVersion is 3. An unversioned fpsCap=0 migrates to monitor Hz.
$lines = @('version=3;', 'qualityPreset=3;', 'terrainDetail=4;', 'objectLod=4;',
    'shadowQuality=4;', 'particlesQuality=4;', 'grassQuality=4;', "msaaSamples=$(if ($MsaaSamples -eq 1) {0} else {$MsaaSamples});",
    'alphaToCoverage=1;', 'renderScale=1;', 'brightness=1.6;', 'gamma=1.2;',
    "dlssMode=$DlssMode;", 'upscalerQuality=67;', 'vsync=0;', 'fpsCap=0;')
[IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"))
