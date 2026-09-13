param([ValidateSet(80,160,320)][int]$Units=80,
      [ValidateSet('Near','Away')][string]$Pose='Near',
      [ValidateRange(1,5)][int]$Repeats=2,
      [string]$Label='baseline',
      [switch]$SampleMemory,
      [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build/ai-scale'
$mission=Join-Path $out "idle-$Units.Intro"
New-Item -ItemType Directory -Force $mission | Out-Null
$text=[Text.StringBuilder]::new()
$null=$text.AppendLine('version=11; class Mission { randomSeed=1234; class Intel {year=1985;month=6;day=21;hour=12;minute=0;startWeather=0;forecastWeather=0;startFog=0;forecastFog=0;}; class Groups {')
$null=$text.AppendLine("items=$($Units/8+1);")
$null=$text.AppendLine('class Item0 {side="WEST";class Vehicles {items=1;class Item0 {position[]={9700,0,3500};id=0;side="WEST";vehicle="SoldierWB";player="PLAYER COMMANDER";leader=1;skill=0.5;};};};')
for ($g=0; $g -lt $Units/8; $g++) {
    $null=$text.AppendLine("class Item$($g+1) {side=`"WEST`";class Vehicles {items=8;")
    for ($u=0; $u -lt 8; $u++) {
        $x=9640+($g%5)*28+($u%4)*5
        $z=3600+[Math]::Floor($g/5)*18+[Math]::Floor($u/4)*5
        $leader=if ($u -eq 0) {1} else {0}
        $null=$text.AppendLine("class Item$u {position[]={$x,0,$z};azimut=180;id=$($g*8+$u+1);side=`"WEST`";vehicle=`"SoldierWB`";leader=$leader;skill=0.5;};")
    }
    $null=$text.AppendLine('};};')
}
$null=$text.AppendLine('};};')
[IO.File]::WriteAllText((Join-Path $mission 'mission.sqm'),$text.ToString())
$user=Join-Path $out "user-$Label-$Units-$Pose"
New-Item -ItemType Directory -Force $user | Out-Null
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user
# Rotate in place so the counterfactual does not move the live player or alter
# AI distance-based scheduling. A true distant-camera test is a separate arm.
$camera=if ($Pose -eq 'Near') {@(9700,3545,45,0,-12)} else {@(9700,3545,45,180,-12)}
# Real-time pacing, not lockstep: lockstep FPS is not playable performance.
$settings=@{POSEIDON_USER_DIR=$user;POSEIDON_VSYNC='0';POSEIDON_LOCKSTEP_HZ='0';WGR_PASS1_STATS='1'}
foreach ($key in @('WGR_RENDER_THREAD','WGR_RENDER_OVERLAP','WGR_SKIN_BAKE','WGR_PALETTE_UPLOAD_BATCH','WGR_PALETTE_UPLOAD_TRACE','WGR_LOD_GOVERNOR_RANGE','WGR_VB_UPDATE_STATS','WGR_SHADOW_MAPS','POSEIDON_SIM_VEHICLE_COST')) {
    $value=[Environment]::GetEnvironmentVariable($key)
    if ($null -ne $value) {$settings[$key]=$value}
}
$cameraArgs=@('--test-world-freefly') + @($camera | ForEach-Object {$_.ToString([Globalization.CultureInfo]::InvariantCulture)})
# The application supports camera-only positioning. Do not replace the stock
# landscape: the benchmark helper's -Freefly convenience option requires -World.
& "$PSScriptRoot/farfield-bench.ps1" -Label "$Label-idle-$Units-$Pose" -Mission $mission `
 -GameDir $GameDir -ExtraArgs $cameraArgs `
 -Out $out -Env $settings -Repeats $Repeats -Width 1280 -Height 720 `
 -Windowed -WarmupSeconds 60 -TimeoutSeconds 180 -RequireAll -SampleMemory:$SampleMemory
for ($r=1; $r -le $Repeats; $r++) {
    $log=Join-Path (Join-Path $out "$Label-idle-$Units-$Pose") ('run-{0:D2}.log' -f $r)
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|StartAutoTest could not boot|Cannot load --test-world' -Quiet) {throw 'Invalid run'}
    if (!(Select-String -LiteralPath $log -Pattern '--test-world-freefly: camera at' -Quiet)) {throw 'Camera was not applied'}
    $countLine=Select-String -LiteralPath $log -Pattern 'AI counts/tick.*unitThinks=' | Select-Object -Last 1
    if (!$countLine -or $countLine.Line -notmatch 'unitThinks=([0-9.]+)' -or
        [Math]::Abs([double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)-($Units+1)) -gt 0.01) {
        throw 'Idle actors missing or inactive; reject scale measurement'
    }
    Write-Host "Last settled simulation samples for $log"
    Select-String -LiteralPath $log -Pattern 'Sim stage ms/tick|AI ms/tick|AI unit think ms/tick' | Select-Object -Last 9 | ForEach-Object {$_.Line}
}
Write-Host 'Idle scale diagnostic only: confirm actor count/capture before interpretation; combat and vanilla comparison remain separate.'
