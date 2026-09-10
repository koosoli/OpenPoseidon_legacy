param([switch]$LegacyClutter, [switch]$VerifyClutter, [switch]$TraceSlowFrames, [switch]$LegacyTableBudget,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
      [string]$Archives='C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($LegacyClutter -and $VerifyClutter) {throw 'Verification needs the reuse path enabled'}
if (!(Test-Path -LiteralPath $Archives -PathType Container)) {throw 'Native archives missing'}
$root=Split-Path $PSScriptRoot -Parent
$label='cancel-'+(Get-Date -Format 'yyyyMMdd-HHmmss')
if ($LegacyClutter) {$label+='-legacy-clutter'}
if ($VerifyClutter) {$label+='-verify-clutter'}
if ($TraceSlowFrames) {$label+='-slow-trace'}
if ($LegacyTableBudget) {$label+='-legacy-tables'}
$out=Join-Path $root 'build/native-stream-cancellation'
$user=Join-Path $out ('user-'+$label)
New-Item -ItemType Directory -Force $user | Out-Null
[IO.File]::WriteAllText((Join-Path $user 'graphics.cfg'), "qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=0;`n")
$settings=@{
 POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0'; POSEIDON_LOCKSTEP_HZ='0'
 POSEIDON_REFORGER_WORLD='worlds/eden'; POSEIDON_REFORGER_OBJECTS='1'; POSEIDON_REFORGER_STREAM='1'
 WGR_PASS1_STATS='1'; WGR_LOD_GOVERNOR_RANGE='1'; POSEIDON_LOD_TRACE='1'
 WGR_NATIVE_CLUTTER_REUSE='1'; WGR_NATIVE_CLUTTER_VERIFY='0'
 WGR_OBJECT_STREAM_TABLE_BATCH='2'
}
if ($LegacyClutter) {$settings.WGR_NATIVE_CLUTTER_REUSE='0'}
if ($VerifyClutter) {$settings.WGR_NATIVE_CLUTTER_VERIFY='1'}
if ($TraceSlowFrames) {$settings.POSEIDON_SLOW_FRAME_TRACE='1'}
if ($LegacyTableBudget) {$settings.WGR_OBJECT_STREAM_TABLE_BATCH='0'}
# Short settle intentionally leaves cold work outstanding when movement starts.
# This is a lifecycle smoke test, NOT a settled-pose FPS comparison.
& "$PSScriptRoot/farfield-movecam.ps1" -Label $label -Mission 'tests/perf/missions/perf_field.eden' -World $Archives -Start 5092.61,3995.64 -StartY 220 -Azimuth 270 -Elevation -15 -Speed 100 -SettleSeconds 20 -StaticSeconds 10 -MoveSeconds 40 -LoadSeconds 20 -CaptureAt 110 -Width 1280 -Height 720 -GameDir $GameDir -Out $out -Env $settings -SampleMemory
$log=Join-Path (Join-Path $out $label) 'run-01.log'
if (!(Select-String -LiteralPath $log -Pattern 'FT M' -Quiet)) {throw 'No moving-phase evidence'}
if (!(Select-String -LiteralPath $log -Pattern 'FFCAM done' -Quiet)) {throw 'Camera route incomplete'}
if (!(Select-String -LiteralPath $log -Pattern 'Object stream prepare:' -Quiet)) {throw 'No preparer evidence'}
$metrics=Get-Content -LiteralPath (Join-Path (Split-Path $log -Parent) 'run-01.json') -Raw | ConvertFrom-Json
if ($metrics.build.render_width -ne 1280 -or $metrics.build.render_height -ne 720 -or
    $metrics.build.msaa_samples -ne 4 -or $metrics.build.dlss_active -or
    !$metrics.objects.valid -or $metrics.objects.registered_instances -le 0 -or
    !$metrics.streaming.preparer.valid -or $metrics.streaming.preparer.prepared -le 0) {
    throw 'Missing native objects/preparer or unexpected rendering profile'
}
if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|StartAutoTest could not boot|verification: MISMATCH' -Quiet) {throw 'Runtime failure'}
if ($VerifyClutter -and !(Select-String -LiteralPath $log -Pattern 'verification: identical geography' -Quiet)) {throw 'No positive clutter verification'}
$tableRows=Select-String -LiteralPath $log -Pattern 'Object stream bank tables: prepared=(\d+) deferredPlacements=(\d+) limit=(\d+) ms=([\d.]+)'
if (!$tableRows) {throw 'No bank-table preparation evidence'}
foreach ($row in $tableRows) {
    $match=$row.Matches[0]
    $count=[int]$match.Groups[1].Value
    $limit=[int]$match.Groups[3].Value
    if ($limit -gt 0 -and $count -gt $limit) {throw 'Bank-table preparation exceeded its count budget'}
}
Write-Output "Native moving-camera lifecycle evidence: $log"
