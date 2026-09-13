param([switch]$LegacyClutter, [switch]$VerifyClutter, [switch]$TraceSlowFrames, [switch]$LegacyTableBudget, [switch]$TraceDdsPreparation,
      [switch]$SampleDriverMemory, [switch]$TraceBankTables, [switch]$FrameTrace,
      [ValidateRange(0,1)][int]$NativeTextureKeys=1,
      [ValidateRange(0,1)][int]$AuthoredFar=0,
      [ValidateRange(-1,1)][int]$LazyBcrTint=-1,
      [ValidateRange(0,64)][int]$ExperimentalTableBudget=0,
      [ValidateRange(-1,1)][int]$TintReuse=-1, [switch]$VerifyTintReuse,
      [ValidateRange(0,1)][int]$DdsPrepare=0, [switch]$VerifyDdsPrepare,
      [ValidateRange(-1,2)][int]$Bc7Batch=-1,
      [ValidateRange(0,1)][int]$NormalPrefetch=0,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
      [string]$Archives='C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
if ($LegacyClutter -and $VerifyClutter) {throw 'Verification needs the reuse path enabled'}
if ($LegacyTableBudget -and $ExperimentalTableBudget -gt 0) {throw 'Choose the production table path or the experimental cap, not both'}
if ($VerifyTintReuse -and $TintReuse -eq 0) {throw 'Tint verification needs reuse enabled'}
if ($VerifyDdsPrepare -and $DdsPrepare -ne 1) {throw 'DDS preparation verification needs preparation enabled'}
if (!(Test-Path -LiteralPath $Archives -PathType Container)) {throw 'Native archives missing'}
$root=Split-Path $PSScriptRoot -Parent
$runId=Get-Date -Format 'yyyyMMdd-HHmmss'
$label='cancel-'+$runId
if ($LegacyClutter) {$label+='-legacy-clutter'}
if ($VerifyClutter) {$label+='-verify-clutter'}
if ($TraceSlowFrames) {$label+='-slow-trace'}
if ($LegacyTableBudget) {$label+='-legacy-tables'}
if ($ExperimentalTableBudget -gt 0) {$label+='-experimental-tables-'+$ExperimentalTableBudget}
if ($TraceDdsPreparation) {$label+='-dds-prep'}
if ($TraceBankTables) {$label+='-bank-trace'}
$label+='-texture-keys-'+$NativeTextureKeys
$label+='-authored-far-'+$AuthoredFar
$label+='-lazy-bcr-'+$(if ($LazyBcrTint -lt 0) {'default'} else {$LazyBcrTint})
if ($Bc7Batch -gt 0) {$label+='-bc7-'+$Bc7Batch}
if ($Bc7Batch -lt 0) {$label+='-bc7-default'}
if ($SampleDriverMemory) {$label+='-driver-memory'}
if ($TintReuse -ge 0) {$label+='-tint-reuse-'+$TintReuse}
if ($VerifyTintReuse) {$label+='-verify-tint'}
if ($DdsPrepare -eq 1) {$label+='-async-dds'}
if ($NormalPrefetch -eq 1) {$label+='-normal-prefetch'}
if ($VerifyDdsPrepare) {$label+='-verify-dds'}
$out=Join-Path $root 'build/native-stream-cancellation'
# The engine's legacy file reader cannot open a graphics.cfg beyond MAX_PATH.
# Keep the full experiment label in capture metadata, not the profile path.
$user=Join-Path $out ('user-'+$runId)
New-Item -ItemType Directory -Force $user | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0
$settings=@{
 WGR_NATIVE_TEXTURE_KEYS=[string]$NativeTextureKeys
 WGR_NATIVE_FAR_AUTHORED=[string]$AuthoredFar
 WGR_NATIVE_DDS_PREPARE=[string]$DdsPrepare; WGR_NATIVE_DDS_PREPARE_VERIFY=$(if ($VerifyDdsPrepare) {'1'} else {'0'})
 POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0'; POSEIDON_LOCKSTEP_HZ='0'
 POSEIDON_REFORGER_WORLD='worlds/eden'; POSEIDON_REFORGER_OBJECTS='1'; POSEIDON_REFORGER_STREAM='1'
 WGR_PASS1_STATS='1'; WGR_LOD_GOVERNOR_RANGE='1'; POSEIDON_LOD_TRACE='1'
 WGR_NATIVE_CLUTTER_REUSE='1'; WGR_NATIVE_CLUTTER_VERIFY='0'
 # The count cap delayed coverage in acceptance and is not the shipped default.
 WGR_OBJECT_STREAM_TABLE_BATCH='0'
}
if ($LazyBcrTint -ge 0) {$settings.WGR_LAZY_BCR_TINT=[string]$LazyBcrTint}
if ($LegacyClutter) {$settings.WGR_NATIVE_CLUTTER_REUSE='0'}
if ($VerifyClutter) {$settings.WGR_NATIVE_CLUTTER_VERIFY='1'}
if ($TraceSlowFrames) {$settings.POSEIDON_SLOW_FRAME_TRACE='1'}
if ($LegacyTableBudget) {$settings.WGR_OBJECT_STREAM_TABLE_BATCH='0'}
if ($ExperimentalTableBudget -gt 0) {$settings.WGR_OBJECT_STREAM_TABLE_BATCH=[string]$ExperimentalTableBudget}
if ($TraceDdsPreparation) {$settings.POSEIDON_DDS_PREP_TRACE='1'}
if ($TraceBankTables) {$settings.POSEIDON_BANK_TABLE_TRACE='1'}
if ($FrameTrace) {$settings.POSEIDON_FRAME_TRACE=Join-Path $out ('trace-'+$runId)}
$settings.WGR_NATIVE_NORMAL_PREFETCH=[string]$NormalPrefetch
if ($Bc7Batch -ge 0) {$settings.POSEIDON_DDS_BC7_BATCH=[string]$Bc7Batch}
if ($TintReuse -ge 0) {$settings.POSEIDON_DDS_TINT_REUSE=[string]$TintReuse}
if ($VerifyTintReuse) {$settings.POSEIDON_DDS_TINT_REUSE='1'; $settings.POSEIDON_DDS_TINT_REUSE_VERIFY='1'}
# Short settle intentionally leaves cold work outstanding when movement starts.
# This is a lifecycle smoke test, NOT a settled-pose FPS comparison.
$gpuSampler = $null
try {
if ($SampleDriverMemory) {
    $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if (!$smi) { throw 'Requested NVIDIA driver-memory timeline, but nvidia-smi is unavailable' }
    $captureDir = Join-Path $out $label
    New-Item -ItemType Directory -Force $captureDir | Out-Null
    $gpuCsv = Join-Path $captureDir 'driver-gpu-memory.csv'
    $gpuError = Join-Path $captureDir 'driver-gpu-memory.stderr.txt'
    $gpuSampler = Start-Process -FilePath $smi.Source -ArgumentList @(
        '--query-gpu=timestamp,index,uuid,memory.total,memory.used,utilization.gpu',
        '--format=csv,noheader,nounits','--loop-ms=1000'
    ) -WindowStyle Hidden -PassThru -RedirectStandardOutput $gpuCsv -RedirectStandardError $gpuError
    [IO.File]::WriteAllText((Join-Path $captureDir 'driver-gpu-memory.json'), (@{
        source='nvidia-smi'; scope='whole GPU, includes other applications; not process residency'
        columns=@('timestamp_local','index','uuid','total_MiB','used_MiB','gpu_percent')
        interval_ms=1000; process_id=$gpuSampler.Id
        requirement='Use the same sampler in both comparison arms; no sampling-overhead correction'
    } | ConvertTo-Json -Depth 3))
}
    & "$PSScriptRoot/farfield-movecam.ps1" -Label $label -Mission 'tests/perf/missions/perf_field.eden' -World $Archives -Start 5092.61,3995.64 -StartY 220 -Azimuth 270 -Elevation -15 -Speed 100 -SettleSeconds 20 -StaticSeconds 10 -MoveSeconds 40 -LoadSeconds 20 -CaptureAt 110 -Width 1280 -Height 720 -GameDir $GameDir -Out $out -Env $settings -SampleMemory
    if ($LASTEXITCODE -ne 0) { throw "Moving-camera capture failed with exit code $LASTEXITCODE" }
}
finally {
    if ($gpuSampler) {
        # Only the monitor created above, never another game or profiling process.
        if (!$gpuSampler.HasExited) { $gpuSampler.Kill() }
        $gpuSampler.WaitForExit()
        $gpuSampler.Dispose()
    }
}
if ($SampleDriverMemory) {
    $rows = @(Get-Content -LiteralPath $gpuCsv | Where-Object { $_ -match '^\d{4}/\d{2}/\d{2}' })
    if ($rows.Count -lt 2) { throw "No usable driver-memory timeline; inspect $gpuError" }
    Write-Output "Driver GPU-total memory samples: $($rows.Count), $gpuCsv"
}
$log=Join-Path (Join-Path $out $label) 'run-01.log'
& "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath (Join-Path (Split-Path $log -Parent) 'run-01.meta.json') -LogPath $log
if (!(Select-String -LiteralPath $log -Pattern 'FT M' -Quiet)) {throw 'No moving-phase evidence'}
if (!(Select-String -LiteralPath $log -Pattern 'FFCAM done' -Quiet)) {throw 'Camera route incomplete'}
if (!(Select-String -LiteralPath $log -Pattern 'Object stream prepare:' -Quiet)) {throw 'No preparer evidence'}
if ($AuthoredFar -eq 1) {
    $windows=@(Select-String -LiteralPath $log -Pattern 'Native authored far window:')
    if ($windows.Count -lt 3) {throw 'Far residency did not follow the moving camera'}
    if (!(Select-String -LiteralPath $log -Pattern 'Native authored far progress:' -Quiet)) {
        throw 'No far instances admitted during the route'
    }
}
if ($LazyBcrTint -ne 0 -and !(Select-String -LiteralPath $log -Pattern 'Wgpu lazy BCR tint:' -Quiet)) {throw 'No actual lazy BCR source was used'}
$metrics=Get-Content -LiteralPath (Join-Path (Split-Path $log -Parent) 'run-01.json') -Raw | ConvertFrom-Json
if ($metrics.build.render_width -ne 1280 -or $metrics.build.render_height -ne 720 -or
    $metrics.build.msaa_samples -ne 4 -or $metrics.build.dlss_active -or
    !$metrics.objects.valid -or $metrics.objects.registered_instances -le 0 -or
    !$metrics.streaming.preparer.valid -or $metrics.streaming.preparer.prepared -le 0) {
    throw 'Missing native objects/preparer or unexpected rendering profile'
}
if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|StartAutoTest could not boot|verification: MISMATCH' -Quiet) {throw 'Runtime failure'}
if ($VerifyClutter -and !(Select-String -LiteralPath $log -Pattern 'verification: identical geography' -Quiet)) {throw 'No positive clutter verification'}
if ($VerifyTintReuse -and !(Select-String -LiteralPath $log -Pattern 'DDS tint reuse verification: identical' -Quiet)) {throw 'No positive tint reuse verification'}
if ($VerifyDdsPrepare) {
    $verified = @(Select-String -LiteralPath $log -Pattern 'DDS prepared verification: identical')
    if ($verified.Count -eq 0) {throw 'No prepared DDS source was consumed and verified'}
    Write-Output "Byte-identical prepared DDS sources: $($verified.Count)"
}
$tableRows=Select-String -LiteralPath $log -Pattern 'Object stream bank tables: prepared=(\d+) deferredPlacements=(\d+) limit=(\d+) ms=([\d.]+)'
if (!$tableRows) {throw 'No bank-table preparation evidence'}
foreach ($row in $tableRows) {
    $match=$row.Matches[0]
    $count=[int]$match.Groups[1].Value
    $limit=[int]$match.Groups[3].Value
    if ($limit -gt 0 -and $count -gt $limit) {throw 'Bank-table preparation exceeded its count budget'}
}
Write-Output "Native moving-camera lifecycle evidence: $log"
