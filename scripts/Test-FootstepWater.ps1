[CmdletBinding()]
param([string]$Compiler='clang++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root 'build/puddle-foot-audio'
New-Item -ItemType Directory -Force $output | Out-Null
$exe=Join-Path $output 'footstep_water_test.exe'
& $Compiler '-std=c++20' '-Wall' '-Wextra' '-Werror' ('-I'+(Join-Path $root 'engine')) `
    (Join-Path $root 'tests/unit/engine/weather/test_footstep_water.cpp') '-o' $exe
if ($LASTEXITCODE -ne 0) {throw 'Standing-water foot CPU compilation failed.'}
& $exe
if ($LASTEXITCODE -ne 0) {throw 'Standing-water foot CPU checks failed.'}
# Actual event/consumer wiring complements the executed production helpers.
$move=Get-Content (Join-Path $root 'engine/Poseidon/World/Entities/Infantry/SoldierOldMove.cpp') -Raw
foreach ($proof in @('captureSoundSole(true);','captureSoundSole(false);',
    'PositionModelToWorld(AnimatePoint(level, index))','_soundStepSole.Capture')) {
    if (!$move.Contains($proof)) {throw "Actual sole event witness missing: $proof"}
}
$sim=Get-Content (Join-Path $root 'engine/Poseidon/World/Entities/Infantry/SoldierOldSim.cpp') -Raw
$start=$sim.IndexOf('void Man::Sound(');$end=$sim.IndexOf('void Man::UnloadSound()', $start)
if ($start -lt 0 -or $end -lt $start) {throw 'Actual Sound consumer not found.'}
$sound=$sim.Substring($start,$end-$start)
foreach ($proof in @('_soundStepSole.Clear();','sole.Fresh(Glob.time.toInt())',
    'GRainWater().At(foot.X(), foot.Z())','RoadSurfaceY(foot + VUp * 0.5f)',
    'StandingWaterFootstep(', 'ObjIntersectFire','ObjIntersectView',
    'GetEnvSoundExt("water")', 'GetEnvSoundExtRandom(surface)', '_soundStep->Restart();')) {
    if (!$sound.Contains($proof)) {throw "Actual puddle sound consumer witness missing: $proof"}
}
if ($sound -match '\.Advance\(' -or $sound -match 'AddWater\(') {throw 'Sound mutates physical water.'}
Write-Host 'Standing-water event/admission CPU gates and actual audio wiring PASS; installed contact/sound acceptance pending.'
