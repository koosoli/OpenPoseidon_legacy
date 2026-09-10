param(
 [string]$Label='baseline',
 [ValidateRange(1,25)][int]$Repeats=2,
 [ValidateSet('Village','Coast','Forest')][string]$Pose='Village',
 [hashtable]$Overrides=@{},
 [string]$ReforgerAddons='C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons'
)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Run through scripts/with-game-lock.sh'}
if (!(Test-Path -LiteralPath $ReforgerAddons -PathType Container)) {throw 'Native Reforger archives not found'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/native-reforger-perf'
$settings=@{
 POSEIDON_USER_DIR=(Join-Path $out 'user')
 POSEIDON_VSYNC='0'
 POSEIDON_REFORGER_WORLD='worlds/eden'
 POSEIDON_REFORGER_OBJECTS='1'
 POSEIDON_REFORGER_STREAM='1'
 WGR_PASS1_STATS='1'
}
foreach ($key in $Overrides.Keys) {$settings[$key]=$Overrides[$key]}
$camera=switch ($Pose) {
 'Village' {@(5092.61,3995.64,18.3,200.9,-13.5)}
 'Coast' {@(3800,7960,4,40,-5)}
 'Forest' {@(4697.60,3998.02,50.23,220.6,-5.0)}
}
& "$PSScriptRoot/farfield-bench.ps1" -Label "$Pose-$Label" -Mission 'tests/perf/missions/perf_field.eden' `
 -World $ReforgerAddons -Freefly $camera -Out $out -Env $settings -Repeats $Repeats `
 -Width 1920 -Height 1080 -Windowed -WarmupSeconds 90 -TimeoutSeconds 300 -RequireAll
for ($repeat=1; $repeat -le $Repeats; $repeat++) {
 $stem=Join-Path (Join-Path $out "$Pose-$Label") ('run-{0:D2}' -f $repeat)
 $metrics=Get-Content -LiteralPath "$stem.json" -Raw | ConvertFrom-Json
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
