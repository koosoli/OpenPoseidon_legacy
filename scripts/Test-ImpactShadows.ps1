param([switch]$Stratis, [ValidateRange(0,23)][int]$Hour=12, [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {throw 'Use scripts/with-game-lock.sh'}
$root=Split-Path $PSScriptRoot -Parent
$island=if ($Stratis) {'Stratis'} else {'Intro'}
$output=Join-Path $root "build/impact-shadows/$island-hour$Hour"
# --test-world replaces Intro's terrain; the fixture must retain its registered
# world suffix so legacy mission-directory resolution also finds init.sqs.
$mission=Join-Path $output 'impact.Intro'
$env:POSEIDON_USER_DIR=Join-Path $output 'user'
New-Item -ItemType Directory -Force $mission,$env:POSEIDON_USER_DIR | Out-Null
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'), "qualityPreset=3;`nterrainDetail=4;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$x=if ($Stratis) {2154.65} else {9500}
$z=if ($Stratis) {5096.22} else {3500}
$sx=$x.ToString([Globalization.CultureInfo]::InvariantCulture)
$sz=$z.ToString([Globalization.CultureInfo]::InvariantCulture)
[IO.File]::WriteAllText((Join-Path $mission 'mission.sqm'), @"
version=11;
class Mission {
 randomSeed=1234;
 class Intel { year=1985; month=6; day=21; hour=$Hour; minute=0; startWeather=0.05; forecastWeather=0.05; startFog=0; forecastFog=0; };
 class Groups { items=1; class Item0 { side="WEST";
 class Vehicles { items=1; class Item0 { position[]={$sx,0,$sz}; id=0; side="WEST"; vehicle="SoldierWB"; player="PLAYER COMMANDER"; leader=1; skill=1; }; }; }; };
};
class Intro { randomSeed=1; class Intel {}; };
class OutroWin { randomSeed=2; class Intel {}; };
class OutroLoose { randomSeed=3; class Intel {}; };
"@)
[IO.File]::WriteAllText((Join-Path $mission 'init.sqs'), @"
~1
_x=$sx
_z=$sz
player setPos [_x,_z,0]
_h=(getPosASL player) select 2
logInfo format ["IMPACT-SHADOW terrain %1",_h]
? _h < 1 : exit
player setPos [_x-20,_z-10,0]
_cam="camera" camCreate [_x,_z-10,4]
_cam cameraEffect ["internal","back"]
_cam camSetTarget [_x,_z,0]
_cam camSetPos [_x,_z-10,4]
_cam camCommit 0
~2
#loop
_bullet="BulletSingleW" createVehicle [_x,_z,2]
_bullet setPosASL [_x,_z,_h+2]
_bullet setVelocity [0,0,-100]
~0.25
goto "loop"
"@)
$env:WGR_AUTO_EXPOSURE='0'; $env:WGR_EXPOSURE=if ($Hour -ge 8 -and $Hour -le 17) {'0.15'} else {'1'}
$env:POSEIDON_SMOKE_GROUND_SHADOW_LEGACY='1'
$env:WGR_EARLY_LATE_OPAQUE='0'; $env:WGR_LATE_SURFACE_RAYS='0'
$capture=Join-Path $output 'impact.png'; $log=Join-Path $output 'impact.log'
$worldArgs=@()
if ($Stratis) {
    $world=(Resolve-Path -LiteralPath (Join-Path $root '../../../packages/a3-compat/world/stratis/stratis.wrp')).Path
    $worldArgs=@('--mod','@a3stratis','--test-world',('"'+$world+'"'))
}
$p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList (@('--render','wgpu','--window','--no-dev','--width','1280','--height','720','--test-mission',('"'+$mission+'"'),'--auto-screenshot',('"25s:'+$capture+'"'),'--log-file',('"'+$log+'"'))+$worldArgs)
$null=$p.Handle
try {
    if (!$p.WaitForExit(180000) -or $p.ExitCode -ne 0) {throw 'Impact test failed'}
    if (!(Test-Path -LiteralPath $capture)) {throw 'No impact capture'}
    if (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|StartAutoTest could not boot' -Quiet) {throw 'Invalid renderer/mission run'}
    $ground=Select-String -LiteralPath $log -Pattern 'IMPACT-SHADOW terrain ([0-9.eE+-]+)' | Select-Object -Last 1
    if (!$ground -or [double]::Parse($ground.Matches[0].Groups[1].Value,[Globalization.CultureInfo]::InvariantCulture) -lt 1) {throw 'Not dry terrain'}
    $samples=@(Select-String -LiteralPath $log -Pattern 'ground shadow: legacy=true strength=([0-9.]+) cloudlets=([0-9]+) blobs=([0-9]+) total=([0-9]+)' | Select-Object -Last 3)
    if ($samples.Count -lt 3) {throw 'Shadow pass not exercised'}
    foreach ($s in $samples) {
        $g=$s.Matches[0].Groups
        if ([double]::Parse($g[1].Value,[Globalization.CultureInfo]::InvariantCulture) -le 0 -or [int]$g[2].Value -le 0 -or [int]$g[3].Value -ne 0 -or [int]$g[4].Value -ne 0) {throw "Unexpected dust shadow: $($s.Line)"}
        Write-Host $s.Line
    }
    Write-Host $capture
} finally {if (!$p.HasExited) {$p.Kill()}}
