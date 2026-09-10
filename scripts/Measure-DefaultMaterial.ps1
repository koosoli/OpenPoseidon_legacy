param([ValidateSet('Original','Albedo','Full')][string]$Arm = 'Full',
      [ValidateSet('Road','Sand','Gravel')][string]$Surface = 'Road')
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Use scripts/with-game-lock.sh' }
$root = Split-Path $PSScriptRoot -Parent
$label = 'material-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Surface + '-' + $Arm
$out = Join-Path $root 'build/default-content/acceptance'
$user = Join-Path $out ('user-' + $label)
New-Item -ItemType Directory -Force $user | Out-Null
[IO.File]::WriteAllText((Join-Path $user 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=0;`n")
$settings = @{POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0'; WGR_LOD_GOVERNOR_RANGE='1';
    POSEIDON_VISUAL_UPGRADE_NORMALS=$(if ($Arm -eq 'Albedo') {'0'} else {'1'})}
$extra = @('--test-world-freefly','6378.7','5672.5','74.4','120','-12','--test-world-hour','14')
if ($Surface -eq 'Sand') {
    $extra = @('--test-world-freefly','5800','3400','13.0','90','-20','--test-world-hour','14')
}
if ($Surface -eq 'Gravel') {
    $extra = @('--test-world-freefly','2221.7','4788.8','40.2','230','-16','--test-world-hour','14')
}
if ($Arm -eq 'Original') { $extra += '--no-visual-upgrade' }
& "$PSScriptRoot/farfield-bench.ps1" -Label $label -Mission 'tests/perf/missions/perf_field.eden' `
    -Out $out -Env $settings -Repeats 1 -Width 1280 -Height 720 -Windowed -WarmupSeconds 20 `
    -TimeoutSeconds 120 -RequireAll -SampleMemory -ExtraArgs $extra
if ($LASTEXITCODE -ne 0) { throw "Material capture failed with exit code $LASTEXITCODE" }
$captureDir = Join-Path $out $label
& "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath (Join-Path $captureDir 'run-01.meta.json') -LogPath (Join-Path $captureDir 'run-01.log')
