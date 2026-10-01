# Installed real-player muzzle lighting captures. No synthetic light/shot command.
[CmdletBinding()]
param([switch]$SelfTest,
      [switch]$OriginalFlash,
      [ValidateSet('body','frontBody','surfaces')][string]$CaptureVisibleFlash,
      [ValidateSet('Both','Night','Roof')][string]$Scenario='Both',
      [ValidateSet('Both','On','Off')][string]$Arm='Both',
      [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='standard-us',
      [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$condition,[string]$message) {if(!$condition){throw $message}}
function Decode-Eval([string]$display) {
    $display=$display.Trim()
    if($display.StartsWith('"')) {
        Require ($display.Length -ge 2 -and $display.EndsWith('"')) 'Malformed SQF string.'
        $inner=$display.Substring(1,$display.Length-2);$decoded=[Text.StringBuilder]::new()
        for($i=0;$i -lt $inner.Length;++$i) {
            if($inner[$i] -eq '"') {Require ($i+1 -lt $inner.Length -and $inner[$i+1] -eq '"') 'Malformed SQF doubled quote.';++$i}
            $null=$decoded.Append($inner[$i])
        }
        return $decoded.ToString()
    }
    # Object values use an unquoted debug display (for example WEST Alpha
    # Schwarz:1 (mail)); they are not JSON. Decode only actual JSON-shaped
    # scalar/array values, retaining the custom object representation literally.
    if($display.StartsWith('[') -or $display -cmatch '^(?:true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)$') {
        return ConvertFrom-Json -InputObject $display -NoEnumerate
    }
    return $display
}
function Parse-MuzzleTrace([string]$line) {
    $number='[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?'
    $pattern='MUZZLE_LIGHT event=bullet timeMs=(\d+) pos=('+ $number+'),('+ $number+'),('+ $number+') lifeMs=110 core=0\.70 reach=8\.40 daylight=true immediate=true active=true\s*$'
    Require ($line -cmatch $pattern) 'Missing/unknown actual active bullet-light telemetry.'
    $fields=@($Matches[1],$Matches[2],$Matches[3],$Matches[4])
    $position=@($fields[1..3]|ForEach-Object {[double]::Parse($_,$culture)})
    foreach($value in $position) {Require (![double]::IsNaN($value) -and ![double]::IsInfinity($value) -and [Math]::Abs($value) -lt 1000000) 'Invalid world muzzle coordinate.'}
    return @{line=$line;timeMs=[long]::Parse($fields[0],$culture);positionEngineXYZ=$position;lifeMs=110;core=.7;reach=8.4}
}
function Assert-Peak($trace,[long]$timeMs,$playerAsl) {
    $age=$timeMs-$trace.timeMs;Require ($age -ge 0 -and $age -le 20) 'Captured actual pulse is outside its 20ms peak.'
    # Trace is engine XYZ; SQF getPosASL is XZY.
    $distance=[Math]::Sqrt([Math]::Pow($trace.positionEngineXYZ[0]-$playerAsl[0],2)+[Math]::Pow($trace.positionEngineXYZ[2]-$playerAsl[1],2)+[Math]::Pow($trace.positionEngineXYZ[1]-$playerAsl[2],2))
    Require ($distance -le 3) 'Actual muzzle is not close to the controlled shooter.'
    return @{ageMs=$age;muzzleDistance=$distance}
}
function New-MuzzleViews([double]$X,[double]$Z,[double]$SoleHeight) {
    # Comma binds before arithmetic in PowerShell array expressions. Every
    # calculated coordinate must be parenthesized rather than subtracting an array.
    return [ordered]@{
        body=@{camera=@(($X-3.0),($Z-2.5),($SoleHeight+1.65));target=@($X,($Z+.4),($SoleHeight+1.05))}
        frontBody=@{camera=@(($X-2.5),($Z+3.0),($SoleHeight+1.65));target=@($X,($Z+.4),($SoleHeight+1.25))}
        surfaces=@{camera=@(($X-3.0),($Z-3.0),($SoleHeight+1.2));target=@($X,($Z+1.5),($SoleHeight+2.8))}
    }
}
function Render-ViewVector($scene) {
    # Saved fixtures use SQF X,Z,Y ASL. triSetView uses absolute engine X,Y,Z.
    $direction=@(($scene.target[0]-$scene.camera[0]),($scene.target[2]-$scene.camera[2]),($scene.target[1]-$scene.camera[1]))
    $length=[Math]::Sqrt($direction[0]*$direction[0]+$direction[1]*$direction[1]+$direction[2]*$direction[2])
    Require ([double]::IsFinite($length) -and $length -gt .000001) 'Fixed view has no finite direction.'
    return ,(@($scene.camera[0],$scene.camera[2],$scene.camera[1])+@($direction | ForEach-Object {$_/$length}))
}
function Queued-PngPath([string]$reply,[string]$directory,[string]$label) {
    Require ($label -cmatch '^[A-Za-z0-9]{1,80}$') 'Unsafe capture label.'
    Require ($reply.StartsWith('OK:')) ('First-draw capture refused: '+$reply)
    $prefix=$reply.Substring(3)
    Require ([IO.Path]::IsPathFullyQualified($prefix)) 'Capture reply is not an absolute path.'
    $full=[IO.Path]::GetFullPath($prefix)
    Require ([IO.Path]::GetDirectoryName($full) -ieq [IO.Path]::GetFullPath($directory).TrimEnd('/','\')) 'Capture escaped its isolated output directory.'
    Require ([IO.Path]::GetFileName($full) -cmatch ('^[0-9]+_'+[regex]::Escape($label)+'$')) 'Capture reply does not identify the requested label.'
    return $full+'.png'
}
if($SelfTest) {
    $views=New-MuzzleViews 4975 4675 20
    $expected=@{body=@{camera=@(4972,4672.5,21.65);target=@(4975,4675.4,21.05)};frontBody=@{camera=@(4972.5,4678,21.65);target=@(4975,4675.4,21.25)};surfaces=@{camera=@(4972,4672,21.2);target=@(4975,4676.5,22.8)}}
    foreach($view in $views.Keys){foreach($field in @('camera','target')){Require ($views[$view][$field].Count -eq 3) 'Camera vector dimension changed.';for($axis=0;$axis -lt 3;++$axis){Require ([Math]::Abs($views[$view][$field][$axis]-$expected[$view][$field][$axis]) -lt .000001) 'Actual fixture coordinate arithmetic failed.'}}}
    $sample='[info] MUZZLE_LIGHT event=bullet timeMs=100012 pos=4975.100,21.400,4675.800 lifeMs=110 core=0.70 reach=8.40 daylight=true immediate=true active=true'
    $raw=Render-ViewVector $views.frontBody
    Require ($raw.Count -eq 6 -and $raw[0] -eq 4972.5 -and $raw[1] -eq 21.65 -and $raw[2] -eq 4678) 'Render-only camera axis conversion failed.'
    Require ([Math]::Abs($raw[3]*$raw[3]+$raw[4]*$raw[4]+$raw[5]*$raw[5]-1) -lt .000001 -and $raw[5] -lt 0) 'Render-only view direction changed.'
    $directory=[IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) 'muzzleSelfTest'))
    $label='MuzzleNightfrontBodyOn123';$prefix=Join-Path $directory ('7_'+$label)
    Require ((Queued-PngPath ('OK:'+$prefix) $directory $label) -ceq ($prefix+'.png')) 'Actual queued path decode failed.'
    foreach($bad in @(('OK:relative/7_'+$label),('OK:'+(Join-Path ([IO.Path]::GetTempPath()) ('7_'+$label))),('OK:'+(Join-Path $directory '7_other')),'FAIL:no_engine')) {
        $failed=$false;try{$null=Queued-PngPath $bad $directory $label}catch{$failed=$true};Require $failed 'Unowned or refused queue path accepted.'
    }
    foreach($badLabel in @('',('../escape'),('a'*81),'space label','é')) {
        $failed=$false;try{$null=Queued-PngPath ('OK:'+$prefix) $directory $badLabel}catch{$failed=$true};Require $failed 'Unsafe capture label accepted.'
    }
    $trace=Parse-MuzzleTrace $sample;$null=Assert-Peak $trace 100012 @(4975,4675,20)
    $null=Assert-Peak $trace 100032 @(4975,4675,20)
    foreach($bad in @($sample.Replace('active=true','active=false'),$sample.Replace('110','111'),$sample.Replace('21.400','NaN'),$sample.Replace('21.400','1e309'),$sample.Replace('21.400','1000001'),($sample+' unknown=1'))) {
        $failed=$false;try{$null=Parse-MuzzleTrace $bad}catch{$failed=$true};Require $failed 'Malformed telemetry was accepted.'
    }
    foreach($age in @(-1,21,110)) {$failed=$false;try{$null=Assert-Peak $trace (100012+$age) @(4975,4675,20)}catch{$failed=$true};Require $failed 'Nonpeak pulse accepted.'}
    $failed=$false;try{$null=Assert-Peak $trace 100012 @(4900,4675,20)}catch{$failed=$true};Require $failed 'Wrong-owner position accepted.'
    Require ((Decode-Eval '"data3d\mc vojakw2.p3d"') -ceq 'data3d\mc vojakw2.p3d') 'Literal model path changed.'
    Require ((Decode-Eval '"a ""quoted"" value"') -ceq 'a "quoted" value') 'SQF quotes changed.'
    Require ((Decode-Eval '[1,2,3]').Count -eq 3) 'SQF vector decode failed.'
    Require ((Decode-Eval 'WEST Alpha Schwarz:1 (mail)') -ceq 'WEST Alpha Schwarz:1 (mail)') 'Unquoted SQF object representation changed.'
    Require ((Decode-Eval 'true') -is [bool] -and (Decode-Eval '1.25') -eq 1.25) 'Typed scalar decode failed.'
    Write-Host 'PASS muzzle runtime camera arithmetic, render-only direction, queue path/label falsifiers, trace, peak/owner admission and SQF decoder helper checks (no game).';return
}
Require ([bool]$env:LOCK_OWNER) 'Run through scripts/with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve the running game.'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'TerrainPuddleStockRoof.ps1')
$output=Join-Path $root ('build/muzzle-light/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$null=New-Item -ItemType Directory -Force $output
$result=[ordered]@{status='running';arms=@{};acceptance='captures-only; actual surface illumination requires review';limitations=@('Roof scenario is a lifted stock CampEastC shelter, not an enclosed building.','Before/after animation recoil can change pixels; compare matched On/Off peak ages and material regions, excluding the muzzle sprite.','AI, prone/burst, deletion/unload and arbitrary enclosed interiors require additional runtime cases.')}
function Pair {
    $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim();Require (![string]::IsNullOrWhiteSpace($stamp)) 'Deployment provenance missing.'
    return @{stamp=$stamp;exe=(Get-FileHash -LiteralPath (Join-Path $GameDir 'OpenPoseidon.exe')).Hash;dll=(Get-FileHash -LiteralPath (Join-Path $GameDir 'wgpu_renderer.dll')).Hash}
}
$result.before=Pair
$keys=@('WGR_MUZZLE_FLASH_APPEARANCE','POSEIDON_MUZZLE_FLASH_TRACE','POSEIDON_USER_DIR','POSEIDON_MUZZLE_LIGHT','POSEIDON_MUZZLE_LIGHT_TRACE','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOWLINE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_UNIFORM_WET','WGR_OBJECT_SNOW','WGR_TEMPORAL','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_GRASS','WGR_CLOUD_COVERAGE','POSEIDON_WIND_OVERRIDE')
$keys+=@('TRI_OUTPUT_DIR')
$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$p=$null;$client=$null;$held=$false;$armResult=$null
function Health {
    Require (!$p.HasExited) 'Owned game exited unexpectedly.'
    if((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator|StartAutoTest could not boot' -Quiet)){throw 'Installed game validation/script failure.'}
}
function Send($command) {
    Health;$line=$command|ConvertTo-Json -Compress;$line|Add-Content -LiteralPath (Join-Path $armDir 'harness.jsonl');$writer.WriteLine($line)
    do{$replyLine=$reader.ReadLine();Require ($null -ne $replyLine) 'Harness closed.';$replyLine|Add-Content -LiteralPath (Join-Path $armDir 'harness.jsonl');$reply=$replyLine|ConvertFrom-Json}while($null -eq $reply.ok)
    Require ($reply.ok -eq $true) $replyLine;return $reply
}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code){$reply=Send @{cmd='eval';code=$code};Require ($null -ne $reply.result) "No evaluator value: $code";return Decode-Eval ([string]$reply.result)}
function Clock { $state=Send @{cmd='query';what='play_state'};Require ($null -ne $state.time_ms -and $state.has_player -eq $true -and $state.player_active -eq $true) 'Actual player/time query missing.';return [long]$state.time_ms }
function Render-View($scene) {
    Require ([double](Eval 'triGetCameraEffectActive') -eq 0) 'A scripted camera would suspend ordinary firing controls.'
    $raw=Render-ViewVector $scene
    Require ((Eval ('triSetView ['+(($raw | ForEach-Object {$_.ToString('R',$culture)}) -join ',')+']')) -ceq 'OK') 'Fixed render-only flash view refused.'
    Require ([double](Eval 'triGetCameraEffectActive') -eq 0) 'Render-only override suspended player controls.'
}
function Clear-RenderView {Require ((Eval 'triClearView') -ceq 'OK') 'Render-only view did not release.'}
function View($scene) {
    # Original camera vector commands consume terrain-relative height. Stored
    # fixture views are ASL; subtract actual support at each distinct point.
    $cameraGround=Get-TerrainPuddleFixtureHeight ${function:Send} $scene.camera[0] $scene.camera[1]
    $targetGround=Get-TerrainPuddleFixtureHeight ${function:Send} $scene.target[0] $scene.target[1]
    $x=$scene.camera[0].ToString('R',$culture);$z=$scene.camera[1].ToString('R',$culture);$y=([double]$scene.camera[2]-$cameraGround).ToString('R',$culture)
    $tx=$scene.target[0].ToString('R',$culture);$tz=$scene.target[1].ToString('R',$culture);$ty=([double]$scene.target[2]-$targetGround).ToString('R',$culture)
    Exec ('muzzleView="camera" camCreate ['+$x+','+$z+',0]; muzzleView camSetPos ['+$x+','+$z+','+$y+']; muzzleView cameraEffect ["internal","back"]; muzzleView camSetTarget ['+$tx+','+$tz+','+$ty+']; muzzleView camSetFov 0.8; muzzleView camCommit 0; showCinemaBorder false')
    $actual=Eval 'getPosASL muzzleView'
    for($i=0;$i -lt 3;++$i){Require ([Math]::Abs([double]$actual[$i]-[double]$scene.camera[$i]) -lt .05) 'Actual camera does not match the ASL fixture.'}
}
function Unview {Exec 'muzzleView cameraEffect ["terminate","back"]; deleteVehicle muzzleView; player switchCamera "EXTERNAL"'}
function Capture([string]$name) {
    Start-Sleep -Milliseconds 500;$time=Clock;$path=Join-Path $armDir ($name+'.png');$null=Send @{cmd='screenshot';path=$path}
    $until=[DateTime]::UtcNow.AddSeconds(10)
    while(!(Test-Path -LiteralPath $path)){Health;Require ([DateTime]::UtcNow -lt $until) 'Screenshot missing.';Start-Sleep -Milliseconds 100}
    Require ((Clock) -eq $time) 'Paused simulation moved during capture.'
    $bytes=[IO.File]::ReadAllBytes($path);Require ($bytes.Length -ge 24 -and [BitConverter]::ToString($bytes[0..7]) -eq '89-50-4E-47-0D-0A-1A-0A') 'Screenshot is not PNG.'
    return @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=$bytes.Length;timeMs=$time}
}
function Capture-Views($case,[string]$stage) {
    foreach($view in $case.views.GetEnumerator()) {
        View $view.Value;$case.captures[$stage+'-'+$view.Key]=Capture ($case.scenario+'-'+$stage+'-'+$view.Key);Unview
    }
}
function Capture-QueuedFlash([string]$reply,[string]$label,[long]$time) {
    # Use the actual verb reply; never infer its shared sequence counter.
    $path=Queued-PngPath $reply $armDir $label
    $until=[DateTime]::UtcNow.AddSeconds(10)
    while(!(Test-Path -LiteralPath $path)){Health;Require ([DateTime]::UtcNow -lt $until) 'Fired-queued first-draw PNG missing.';Start-Sleep -Milliseconds 50}
    Require ((Clock) -eq $time) 'Simulation advanced after the actual Fired capture.'
    $bytes=[IO.File]::ReadAllBytes($path);Require ($bytes.Length -ge 24 -and [BitConverter]::ToString($bytes[0..7]) -eq '89-50-4E-47-0D-0A-1A-0A') 'First-draw capture is not PNG.'
    $queueRows=@(Select-String -LiteralPath $log -SimpleMatch ('triScreenshotNext queued='+$reply.Substring(3)+' uiTime='))
    Require ($queueRows.Count -eq 1) 'Actual first-draw queue telemetry missing or duplicated.'
    Require ($queueRows[0].Line -match ' uiTime=([0-9]+\.[0-9]+)$') 'Actual UI queue time missing.'
    $uiTime=[double]::Parse($Matches[1],$culture);Require ([double]::IsFinite($uiTime)) 'Actual UI queue time is not finite.'
    return @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;bytes=$bytes.Length;timeMs=$time;queueUiTimeSeconds=$uiTime;queueTrace=$queueRows[0].Line;reply=$reply;label=$label;route='Fired EH -> Screenshot request -> next rendered frame; no synchronous flush';visualAcceptance='requires visible flash pixel review'}
}
function CloseOwned {
    if($held){$null=Send @{cmd='mouse_button';button=1;down=$false};$script:held=$false}
    $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)) 'Owned game did not quit normally.';Require ($p.ExitCode -eq 0) 'Owned game exit not zero.'
    Require (Select-String -LiteralPath $log -SimpleMatch 'Shutdown complete' -Quiet) 'Normal shutdown missing.';$armResult.exitCode=$p.ExitCode
}
try {
    foreach($key in $keys){Remove-Item ('Env:'+$key) -ErrorAction SilentlyContinue}
    $env:WGR_MUZZLE_FLASH_APPEARANCE=if($OriginalFlash){'0'}else{'1'};$env:POSEIDON_MUZZLE_FLASH_TRACE='1'
    $result.flashAppearance=$env:WGR_MUZZLE_FLASH_APPEARANCE
    $result.visibleFlashView=$CaptureVisibleFlash
    $env:POSEIDON_MUZZLE_LIGHT_TRACE='1';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='1';$env:WGR_TEMPORAL='0';$env:WGR_GRASS='0';$env:WGR_CLOUD_COVERAGE='0';$env:POSEIDON_WIND_OVERRIDE='0 90 0';$env:WGR_OBJECT_SNOW='0';$env:POSEIDON_SNOWLINE='off';$env:POSEIDON_SNOW_TEST_DEPTH='0'
    $arms=if($Arm -eq 'Both'){@('On','Off')}else{@($Arm)}
    foreach($name in $arms) {
        Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Another game started between serial arms.'
        $armDir=Join-Path $output $name;$profile=Join-Path $armDir 'user';$null=New-Item -ItemType Directory -Force $profile
        $env:TRI_OUTPUT_DIR=$armDir
        $env:POSEIDON_USER_DIR=$profile;$env:POSEIDON_MUZZLE_LIGHT=if($name -eq 'On'){'1'}else{'0'}
        [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
        $log=Join-Path $armDir 'engine.log';$armResult=[ordered]@{status='running';muzzleLight=$env:POSEIDON_MUZZLE_LIGHT;scenarios=@{}};$result.arms[$name]=$armResult
        $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
        $mission=Join-Path $root 'tests/perf/missions/perf_sand.noe'
        $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','0','--log-file',('"'+$log+'"'))
        $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $armDir 'stdout.txt') -RedirectStandardError (Join-Path $armDir 'stderr.txt');$null=$p.Handle
        $until=[DateTime]::UtcNow.AddSeconds(120)
        do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}}while(!$client)
        $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
        Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Stock mission not ready.';Require ((Eval 'typeOf player') -ceq 'SoldierWB') 'Player is not standard US SoldierWB.'
        Require ((Eval 'triCheatInfiniteAmmo false') -ceq 'OK' -and [double](Eval 'triCheatInfiniteAmmoActive') -eq 0) 'Infinite ammunition must be disabled.'
        Exec 'player allowDamage false; 0 setRain 0; 0 setFog 0; 0 setOvercast 0; removeAllWeapons player; player addMagazine "M16"; player addWeapon "M16"; player selectWeapon "M16"; player setUnitPos "UP"; muzzleShots=0; muzzleShotTime=-1; muzzleCaptureLabel=""; muzzleCaptureReply=""; player addEventHandler ["Fired",{muzzleShots=muzzleShots+1; muzzleShotTime=time; if(muzzleCaptureLabel!="")then{muzzleCaptureReply=triScreenshotNext muzzleCaptureLabel}; setAccTime 0}]; hint ""; setAccTime 1'
        $null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='off';density=0;snowflakes=$false}
        Start-Sleep -Seconds 2
        $scenarios=if($Scenario -eq 'Both'){@('Night','Roof')}else{@($Scenario)}
        foreach($kind in $scenarios) {
            $x=4975.0;$z=4675.0
            # Query actual stock terrain, never infer floor altitude from the fixture name.
            $ground=Get-TerrainPuddleFixtureHeight ${function:Send} $x $z
            $hour=if($kind -eq 'Night'){0}else{16}
            Exec ('setAccTime 1; setDate [1985,6,21,'+$hour+',0]; player setPos [4975,4675,0]; player setDir 0; player setUnitPos "UP"; player switchCamera "EXTERNAL"')
            $roof=$null
            if($kind -eq 'Roof'){$roof=New-TerrainPuddleStockRoof ${function:Send} -X $x -Z ($z+1.5) -RoofLift 1.7}
            Start-Sleep -Seconds 2;Exec 'setAccTime 0'
            Require ((Eval 'triSetSimTime 100') -ceq 'OK:100000') 'Matched actual simulation clock refused.'
            Require ((Eval 'triSetBrightness 1') -ceq 'OK:1.000') 'Fixed brightness refused.'
            $position=Eval 'getPosASL player';Require ([Math]::Abs($position[0]-$x) -lt .3 -and [Math]::Abs($position[1]-$z) -lt .3) 'Roof collision displaced the actual shooter.'
            $views=New-MuzzleViews $x $z ([double]$position[2])
            $case=[ordered]@{scenario=$kind;source=(Eval 'format ["%1",player]');class=(Eval 'typeOf player');weapons=(Eval 'weapons player');position=$position;actualGroundHeight=$ground;requestedDate=@(1985,6,21,$hour,0);actualDayTime=(Eval 'dayTime');weather=(Send @{cmd='weather_visibility'});views=$views;roof=$roof;captures=@{}};$armResult.scenarios[$kind]=$case
            Require ($case.weapons -contains 'M16') 'Actual player has no M16.'
            Require ([Math]::Abs([double]$case.actualDayTime-$hour) -lt .02) 'Actual world hour differs from the scenario.'
            Require ($null -ne $case.weather.rain -and [double]$case.weather.rain -lt .001 -and $null -ne $case.weather.fog -and [double]$case.weather.fog -lt .001) 'Actual dry/clear weather control failed.'
            Capture-Views $case 'before'
            if($CaptureVisibleFlash) {
                $case.visibleFlash=@{view=$CaptureVisibleFlash;scene=$views[$CaptureVisibleFlash];captureLabel=('Muzzle'+$kind+$CaptureVisibleFlash+$name+[guid]::NewGuid().ToString('N'));appearance=$env:WGR_MUZZLE_FLASH_APPEARANCE;acceptance='first-draw captures require actual flash pixel review'}
                Render-View $case.visibleFlash.scene
                $case.captures['visible-before']=Capture ($kind+'-visible-before-'+$CaptureVisibleFlash)
                Exec ('muzzleCaptureReply=""; muzzleCaptureLabel="'+$case.visibleFlash.captureLabel+'"')
            }
            $ammoBefore=[double](Eval 'player ammo "M16"');$shotsBefore=[int](Eval 'muzzleShots');$traceCount=if(Test-Path -LiteralPath $log){@(Select-String -LiteralPath $log -SimpleMatch 'MUZZLE_LIGHT event=bullet').Count}else{0}
            Require ($ammoBefore -gt 0) 'No loaded rounds for ordinary input.'
            Require ([double](Eval 'triPlayerCurrentMagazineAmmo') -eq $ammoBefore) 'Selected magazine differs from the queried rifle.'
            Exec 'setAccTime 0.02';$null=Send @{cmd='mouse_button';button=1;down=$true};$held=$true
            $until=[DateTime]::UtcNow.AddSeconds(20)
            do{Start-Sleep -Milliseconds 40;$shots=[int](Eval 'muzzleShots');Require ([DateTime]::UtcNow -lt $until) 'Ordinary mouse input produced no Fired event.'}while($shots -eq $shotsBefore)
            $null=Send @{cmd='mouse_button';button=1;down=$false};$held=$false
            $ammoAfter=[double](Eval 'player ammo "M16"');Require ($shots -eq $shotsBefore+1 -and $ammoAfter -eq $ammoBefore-1) 'Actual single bullet event/ammo decrement did not match.'
            $now=Clock;$shotTime=[double](Eval 'muzzleShotTime');Require ([Math]::Abs($now-$shotTime*1000) -le 1.1) 'Fired pause did not preserve actual simulation time.'
            $case.shot=@{ammoBefore=$ammoBefore;ammoAfter=$ammoAfter;firedBefore=$shotsBefore;firedAfter=$shots;eventTime=$shotTime;peakTimeMs=$now;input='SDL mouse button 1; Fired event pauses actual emitted effects; accTime=0.02 before trigger'}
            if($CaptureVisibleFlash) {
                $case.captures['visible-first-draw']=Capture-QueuedFlash (Eval 'muzzleCaptureReply') $case.visibleFlash.captureLabel $now
                Exec 'muzzleCaptureLabel=""';Clear-RenderView
            }
            $traces=@(Select-String -LiteralPath $log -SimpleMatch 'MUZZLE_LIGHT event=bullet')
            if($name -eq 'On') {
                Require ($traces.Count -eq $traceCount+1) 'Exactly one new actual muzzle pulse trace required.'
                $trace=Parse-MuzzleTrace $traces[-1].Line;$case.shot.trace=$trace;$case.shot.peak=Assert-Peak $trace $now (Eval 'getPosASL player')
            }else{Require ($traces.Count -eq 0) 'Legacy ablation unexpectedly emitted the new pulse.'}
            Capture-Views $case 'peak'
            if (!$OriginalFlash) {
                Require (Select-String -LiteralPath $log -Pattern 'MUZZLE_FLASH_SHEET.*enabled=true remapped=true' -Quiet) 'Actual stock flash sheet remap missing.'
                Require (Select-String -LiteralPath $log -Pattern 'MUZZLE_FLASH_MATERIAL.*enabled=true.*encoded=-1.0' -Quiet) 'Actual stock flash material emission admission missing.'
            }
            # Advance real simulation beyond both 110ms pulse and old 150ms cleanup.
            Exec 'setAccTime 0.02';$until=[DateTime]::UtcNow.AddSeconds(20)
            do{Start-Sleep -Milliseconds 100;$expired=Clock;Require ([DateTime]::UtcNow -lt $until) 'Actual clock did not advance for expiry.'}while($expired-$now -lt 200)
            Exec 'setAccTime 0';$case.expiredAgeMs=(Clock)-$now
            Require ([int](Eval 'muzzleShots') -eq $shots) 'Expiry caused another shot.'
            Capture-Views $case 'expired'
            if($CaptureVisibleFlash) {
                Render-View $case.visibleFlash.scene
                $case.captures['visible-expired']=Capture ($kind+'-visible-expired-'+$CaptureVisibleFlash)
                Clear-RenderView
            }
            if($roof){Remove-TerrainPuddleStockRoof ${function:Send}}
            $case.status='actual-shot-peak-and-expiry-captured-surface-review-required'
        }
        CloseOwned;$armResult.status='functional-gates-passed-captures-not-visually-accepted';$client.Dispose();$client=$null;$p=$null
    }
    if($Arm -eq 'Both') {
        $result.matchedControls=@{}
        foreach($kind in $result.arms.On.scenarios.Keys) {
            $on=$result.arms.On.scenarios[$kind];$off=$result.arms.Off.scenarios[$kind]
            Require ($off.shot.ammoBefore -eq $on.shot.ammoBefore -and $off.shot.ammoAfter -eq $on.shot.ammoAfter) 'Ablation ammunition history differs.'
            for($axis=0;$axis -lt 3;++$axis){Require ([Math]::Abs($on.position[$axis]-$off.position[$axis]) -le .05) 'Ablation shooter position differs.'}
            foreach($view in $on.views.Keys) {
                for($axis=0;$axis -lt 3;++$axis){Require ([Math]::Abs($on.views[$view].camera[$axis]-$off.views[$view].camera[$axis]) -le .05 -and [Math]::Abs($on.views[$view].target[$axis]-$off.views[$view].target[$axis]) -le .05) 'Ablation capture geometry differs.'}
                Require ($on.captures['before-'+$view].timeMs -eq 100000 -and $off.captures['before-'+$view].timeMs -eq 100000) 'Ablation initial clocks differ.'
            }
            $result.matchedControls[$kind]=@{positionTolerance=.05;fixedInitialTimeMs=100000;eventPeakPaused=$true;surfaceAcceptance='pending visual body/gun/ground/roof review, excluding flash sprite'}
        }
    }
    $result.status='functional-gates-passed-surface-illumination-review-required'
}catch{
    $result.status='failed';$result.error=$_.Exception.Message
    $result.errorSource=@{file=$_.InvocationInfo.ScriptName;line=$_.InvocationInfo.ScriptLineNumber;position=$_.InvocationInfo.PositionMessage;stack=$_.ScriptStackTrace}
    throw
}
finally {
    if($CaptureVisibleFlash -and $p -and !$p.HasExited -and $client){try{Clear-RenderView}catch{$result.viewCleanupError=$_.Exception.Message}}
    if($p -and !$p.HasExited){try{CloseOwned}catch{$result.cleanupError=$_.Exception.Message;if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(5000);$result.forcedCleanup=$true}}}
    if($client){$client.Dispose()}
    foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
    try{$result.after=Pair;Require ($result.before.stamp -ceq $result.after.stamp -and $result.before.exe -ceq $result.after.exe -and $result.before.dll -ceq $result.after.dll) 'Installed pair changed during test.'}catch{$result.status='failed';$result.provenanceError=$_.Exception.Message}
    $result|ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $output 'result.json')
    Write-Host "Muzzle-light actual-shot evidence: $output"
    if($result.status -eq 'failed'){throw ($result.error+' '+$result.provenanceError)}
}
