param([ValidateSet('Original','Albedo','Full')][string]$Arm = 'Full',
      [ValidateSet('Road','Sand','Gravel')][string]$Surface = 'Road',
      [ValidateRange(0,23.999)][double]$Hour = 14,
      [ValidateSet(0,1)][int]$DetailNormals = 1,
      [ValidateSet('Native','DLSS')][string]$Upscaler = 'Native',
      [switch]$Closeup)
$ErrorActionPreference = 'Stop'
if (!$env:LOCK_OWNER) { throw 'Use scripts/with-game-lock.sh' }
if ($Closeup -and $Surface -ne 'Sand') { throw 'Closeup currently has a verified camera only for Sand' }
$root = Split-Path $PSScriptRoot -Parent
$hourText = $Hour.ToString('0.###', [Globalization.CultureInfo]::InvariantCulture)
$label = 'material-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Surface + '-' + $Arm + '-h' + $hourText + '-d' + $DetailNormals
if ($Closeup) { $label += '-close' }
if ($Upscaler -eq 'DLSS') { $label += '-dlss' }
$out = Join-Path $root 'build/default-content/acceptance'
$user = Join-Path $out ('user-' + $label)
New-Item -ItemType Directory -Force $user | Out-Null
$dlssMode = if ($Upscaler -eq 'DLSS') {1} else {0}
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode $dlssMode
$settings = @{POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0'; WGR_LOD_GOVERNOR_RANGE='1';
    POSEIDON_VISUAL_UPGRADE_DETAIL_NORMALS="$DetailNormals";
    POSEIDON_VISUAL_UPGRADE_NORMALS=$(if ($Arm -eq 'Albedo') {'0'} else {'1'})}
$extra = @('--test-world-freefly','6378.7','5672.5','74.4','120','-12')
if ($Surface -eq 'Sand') {
    $extra = @('--test-world-freefly','5800','3400','13.0','90','-20')
    if ($Closeup) { $extra = @('--test-world-freefly','5800','3400','11.1','90','-60') }
}
if ($Surface -eq 'Gravel') {
    $extra = @('--test-world-freefly','2221.7','4788.8','40.2','230','-16')
}
$extra += @('--test-world-hour', $hourText)
if ($Arm -eq 'Original') { $extra += '--no-visual-upgrade' }
& "$PSScriptRoot/farfield-bench.ps1" -Label $label -Mission 'tests/perf/missions/perf_field.eden' `
    -Out $out -Env $settings -Repeats 1 -Width 1280 -Height 720 -Windowed -WarmupSeconds 20 `
    -TimeoutSeconds 120 -RequireAll -SampleMemory -ExtraArgs $extra
if ($LASTEXITCODE -ne 0) { throw "Material capture failed with exit code $LASTEXITCODE" }
$captureDir = Join-Path $out $label
& "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath (Join-Path $captureDir 'run-01.meta.json') -LogPath (Join-Path $captureDir 'run-01.log')
$capture = Get-Content -LiteralPath (Join-Path $captureDir 'run-01.json') -Raw | ConvertFrom-Json
if ($Upscaler -eq 'DLSS' -and $capture.build.dlss_active -ne $true) {
    throw "Requested DLSS was not active: $($capture.build.dlss_reason)"
}
if ($Upscaler -eq 'Native' -and ($capture.build.upscaler -ne 'native' -or
    $capture.build.render_width -ne $capture.build.output_width -or
    $capture.build.render_height -ne $capture.build.output_height)) {
    throw 'Requested native-resolution material capture used an upscaler or render scale'
}
