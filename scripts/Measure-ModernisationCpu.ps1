param([ValidateSet('Field','Combat','Idle80','Idle320')][string]$Scene='Combat',
      [ValidateRange(1,5)][int]$Repeats=2)
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$label='cpu-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+$Scene
$env:WGR_LOD_GOVERNOR_RANGE='1'
$env:POSEIDON_SIM_VEHICLE_COST='1'
if ($Scene -eq 'Idle80' -or $Scene -eq 'Idle320') {
    $units=if ($Scene -eq 'Idle80') {80} else {320}
    & "$PSScriptRoot/Measure-AIScale.ps1" -Units $units -Repeats $Repeats -Label $label -SampleMemory
    return
}
$out=Join-Path $root 'build/modernisation-cpu'
$user=Join-Path $out ('user-'+$label)
New-Item -ItemType Directory -Force $user | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user
$settings=@{POSEIDON_USER_DIR=$user; POSEIDON_VSYNC='0'; POSEIDON_LOCKSTEP_HZ='0';
 WGR_PASS1_STATS='1'; WGR_LOD_GOVERNOR_RANGE='1'; POSEIDON_SIM_VEHICLE_COST='1'}
$mission=if ($Scene -eq 'Combat') {'tests/perf/missions/perf_combat.eden'} else {'tests/perf/missions/perf_field.eden'}
# Same overhead camera in both Everon fixtures. Field is a sparse control,
# not a substitute for campaign-script or populated-town measurements.
& "$PSScriptRoot/farfield-bench.ps1" -Label $label -Mission $mission -Out $out -Env $settings -Repeats $Repeats -Width 1280 -Height 720 -Windowed -WarmupSeconds 60 -TimeoutSeconds 180 -RequireAll -SampleMemory -ExtraArgs @('--test-world-freefly','6400','6350','300','0','-25')
