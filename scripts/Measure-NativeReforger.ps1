param(
 [string]$Label='baseline',
 [ValidateRange(1,25)][int]$Repeats=2,
 [ValidateRange(1,16)][int]$MotionSamples=1,
 [ValidateRange(0.25,5)][double]$MotionInterval=0.5,
 [ValidateRange(30,600)][int]$WarmupSeconds=90,
 [ValidateSet('Village','Coast','Forest','ForestHigh','Church','Reference')][string]$Pose='Village',
 [ValidateRange(-1,23.999)][double]$WorldHour=-1,
 [ValidateRange(0,1)][int]$NativeTextureKeys=1,
 [ValidateRange(-1,1)][int]$LazyBcrTint=-1,
 [hashtable]$Overrides=@{},
 [string]$ReforgerAddons='C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Run through scripts/with-game-lock.sh'}
if (!(Test-Path -LiteralPath $ReforgerAddons -PathType Container)) {throw 'Native Reforger archives not found'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/native-reforger-perf'
$user=Join-Path $out ('user-'+$Pose+'-'+$Label)
New-Item -ItemType Directory -Force $user | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0
$settings=@{
 POSEIDON_USER_DIR=$user
 WGR_NATIVE_TEXTURE_KEYS=[string]$NativeTextureKeys
 WGR_LOD_GOVERNOR_RANGE='1'
 POSEIDON_VSYNC='0'
 POSEIDON_REFORGER_WORLD='worlds/eden'
 POSEIDON_REFORGER_OBJECTS='1'
 POSEIDON_REFORGER_STREAM='1'
 WGR_PASS1_STATS='1'
}
if ($LazyBcrTint -ge 0) {$settings.WGR_LAZY_BCR_TINT=[string]$LazyBcrTint}
foreach ($key in $Overrides.Keys) {$settings[$key]=$Overrides[$key]}
$camera=switch ($Pose) {
 'Village' {@(5092.61,3995.64,18.3,200.9,-13.5)}
 'Coast' {@(3800,7960,4,40,-5)}
 'Forest' {@(4697.60,3998.02,50.23,220.6,-5.0)}
 'ForestHigh' {@(4697.60,3998.02,450.0,220.6,-25.0)}
 'Church' {@(4680.0,6810.0,240.0,28.8,-16.0)}
 'Reference' {@(4665.50,7132.80,192.09,123.7,-11.8)}
}
& "$PSScriptRoot/farfield-bench.ps1" -Label "$Pose-$Label" -Mission 'tests/perf/missions/perf_field.eden' `
 -World $ReforgerAddons -Freefly $camera -WorldHour $WorldHour -Out $out -Env $settings -Repeats $Repeats `
 -Width 1920 -Height 1080 -Windowed -WarmupSeconds $WarmupSeconds -TimeoutSeconds ($WarmupSeconds+210) -RequireAll `
 -MotionSamples $MotionSamples -MotionInterval $MotionInterval
if ($LASTEXITCODE -ne 0) {throw 'Native capture failed; inspect per-run lifecycle metadata'}
for ($repeat=1; $repeat -le $Repeats; $repeat++) {
 $stem=Join-Path (Join-Path $out "$Pose-$Label") ('run-{0:D2}' -f $repeat)
 & "$PSScriptRoot/Assert-CaptureLifecycle.ps1" -MetadataPath "$stem.meta.json" -LogPath "$stem.log"
 $metrics=Get-Content -LiteralPath "$stem.json" -Raw | ConvertFrom-Json
 if ($LazyBcrTint -ne 0 -and !$Overrides.ContainsKey('WGR_LAZY_BCR_TINT') -and
     !(Select-String -LiteralPath "$stem.log" -Pattern 'Wgpu lazy BCR tint:' -Quiet)) {
  throw "No actual lazy BCR source used in $stem"
 }
 if ($metrics.build.render_width -ne 1920 -or $metrics.build.render_height -ne 1080 -or
     $metrics.build.msaa_samples -ne 4 -or $metrics.build.dlss_active) {
  throw "Unexpected rendering profile in $stem; not a native-resolution comparison"
 }
 $terrain=@($metrics.gpu_timings_ms | Where-Object {$_.name -eq 'Terrain: main colour' -and $_.milliseconds -ge 0})
 if (!$metrics.objects.valid -or $metrics.objects.registered_instances -le 0 -or $terrain.Count -ne 1) {
  throw "Incomplete native scene in $stem; not a performance baseline"
 }
 if (Select-String -LiteralPath "$stem.log" -Pattern 'Landscape: skipped .*terrain rect' -Quiet) {
  throw "Rejected terrain pass in $stem; not a performance baseline"
 }
 if ($metrics.streaming.preparer.queued -gt 0 -or $metrics.streaming.preparer.ready -gt 0) {
  throw "Object preparation not settled in $stem; increase warmup before comparing"
 }
}
