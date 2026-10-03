# Installed stock-Noe physical hover; never injects a rotor/water event.
[CmdletBinding()]
param([switch]$SelfTest,
 [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Label='stock-sea-rotor',
 [ValidateSet('CurrentOP','TidewaterOff','TidewaterOn')][string[]]$Arms=@('CurrentOP','TidewaterOff','TidewaterOn'),
 [ValidatePattern('^[0-9a-f]{7,40}$')][string]$ExpectedCommit,
 [double]$SeaX=1750,[double]$SeaZ=3100,[double]$DryX=2475,[double]$DryZ=5075,
 [ValidateRange(12,24)][double]$HoverHeight=18,
 [ValidateRange(4,12)][int]$ObserveSeconds=6,
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop';$root=Split-Path -Parent $PSScriptRoot
$culture=[Globalization.CultureInfo]::InvariantCulture
function Require([bool]$c,[string]$m) {if(!$c){throw $m}}
function Finite($v) {return $null -ne $v -and $v -is [ValueType] -and $v -isnot [bool] -and ![double]::IsNaN([double]$v) -and ![double]::IsInfinity([double]$v)}
function Number([double]$v) {Require (Finite $v) 'Nonfinite fixture coordinate.';return $v.ToString('R',$culture)}
function Sea-View([double]$x,[double]$z,[double]$sea,[string]$angle) {
 if($angle -ceq 'top'){$view=@($x,($sea+55),($z-5));$delta=@(0,-55,5)}
 else{$view=@(($x+28),($sea+10),($z-35));$delta=@(-28,-10,35)}
 $length=[Math]::Sqrt($delta[0]*$delta[0]+$delta[1]*$delta[1]+$delta[2]*$delta[2])
 return @{view=$view;pose=($view+@($delta|ForEach-Object {$_/$length}))}
}
function Decode-Eval([string]$v) {
 $v=$v.Trim();if($v.StartsWith('"')){return $v.Substring(1,$v.Length-2).Replace('""','"')}
 if($v.StartsWith('[') -or $v -cmatch '^(true|false|null|-?\d+(\.\d+)?([eE][+-]?\d+)?)$'){return ConvertFrom-Json -InputObject $v -NoEnumerate};return $v
}
function Assert-State($s) {
 foreach($k in @('timeMs','object','x','y','z','groundY','rpm','upY','speedX','speedY','speedZ','damage','cameraDistance')) {Require (Finite $s.$k) ('Missing/nonfinite actual rotor '+$k)}
 foreach($k in @('readonly','engineOn','destroyed','local','airborne','driverPresent','driverBrain','driverAlive','driverIsPlayer','playerManual','playerSuspended','cameraEffect','hoverStateAvailable')) {Require ($s.$k -is [bool]) ('Missing actual rotor authority '+$k)}
 if($s.hoverStateAvailable){Require ($s.hoveringAutopilot -is [bool] -and $s.pilotHeightHelper -is [bool] -and (Finite $s.pilotWantedHeight)) 'Missing actual hover helper state.'}
 Require ($s.readonly -and $s.class -ceq 'UH60' -and $s.model -ieq 'data3d\uh-60.p3d' -and $s.rpm -ge 0 -and $s.rpm -le 1.001) 'Unaudited actual helicopter snapshot.'
}
function Assert-Pilot($s){
 Assert-State $s
 Require (!$s.destroyed -and $s.damage -eq 0 -and $s.local -and $s.driverPresent -and $s.driverBrain -and $s.driverAlive -and $s.driverIsPlayer -and $s.playerManual -and !$s.playerSuspended -and !$s.cameraEffect) 'Actual healthy controlled helicopter authority absent.'
}
function Assert-Supported($s,[double]$x,[double]$z,[bool]$heldDown=$false){
 Assert-Pilot $s
 Require (!$s.airborne -and $s.y-$s.groundY -ge 0 -and $s.y-$s.groundY -lt 6 -and [Math]::Abs($s.speedY) -le .25 -and [Math]::Abs($s.x-$x) -le 8 -and [Math]::Abs($s.z-$z) -le 8) 'Actual supported landing is absent.'
 if($heldDown){Require ((Finite $s.moveDown) -and $s.moveDown -gt .5 -and $s.hoverStateAvailable -and $s.pilotHeightHelper -and $s.pilotWantedHeight -le 1.001 -and $s.pilotWantedHeight -ge .999) 'Ordinary collective-down did not reach its actual stock landing target.'}
}
function Assert-StoppedSea($s,[double]$x,[double]$z,[double]$sea){
 Assert-Pilot $s
 Require (!$s.engineOn -and $s.rpm -le .02 -and (Finite $s.fuel) -and $s.fuel -eq 0 -and $s.upY -ge .85 -and $s.groundY -lt $sea-.03 -and [Math]::Abs($s.x-$x) -le 8 -and [Math]::Abs($s.z-$z) -le 8) 'Engine-off sea control is not a real healthy stopped rotor over water.'
 Require ($s.y-$sea -ge 8 -and $s.y-$sea -le 28) 'Engine-off sea control is outside the matched low-altitude source window.'
}
function Assert-Hover($s,[double]$x,[double]$z,[double]$surface,[bool]$high=$false) {
 Assert-State $s
 Require ($s.engineOn -and !$s.destroyed -and $s.local -and $s.driverPresent -and $s.driverBrain -and $s.driverAlive -and $s.driverIsPlayer -and $s.playerManual -and !$s.playerSuspended -and !$s.cameraEffect -and $s.hoverStateAvailable -and $s.hoveringAutopilot) 'Physical occupied stock auto-hover authority absent.'
 Require ($s.rpm -ge .65 -and $s.upY -ge .85 -and [Math]::Abs($s.x-$x) -le 8 -and [Math]::Abs($s.z-$z) -le 8) 'Actual rotor RPM/tilt/location leaves the bounded hover fixture.'
 $alt=$s.y-$surface
 if($high){Require ($alt -gt 35) 'High control entered the water rotor altitude window.'}else{Require ($alt -ge 8 -and $alt -le 28) 'Actual hover lacks safe sea-relative water admission altitude.'}
}
function Parse-Receipt([string]$line) {
 Require ($line -cmatch '^TW_ROTOR_UBO (\{.*\})$') 'Unknown actual private-UBO receipt.'
 $r=$Matches[1]|ConvertFrom-Json
 Require ($r.readonly -is [bool] -and $r.readonly -eq $true -and (Finite $r.time) -and (Finite $r.phaseTime) -and (Finite $r.count) -and $r.count -eq [Math]::Floor($r.count) -and $r.count -ge 0 -and $r.count -le 8) 'Malformed copied-UBO receipt.'
 Require ($r.domain.Count -eq 4 -and $r.sources.Count -eq $r.count) 'Malformed actual source/domain dimensions.'
 foreach($v in $r.domain){Require (Finite $v) 'Nonfinite actual UBO domain.'}
 foreach($s in $r.sources){Require ($s.Count -eq 4) 'Malformed actual rotor descriptor.';foreach($v in $s){Require (Finite $v) 'Nonfinite actual rotor descriptor.'};Require ($s[2] -ge 3 -and $s[2] -le 15 -and $s[3] -gt 0 -and $s[3] -le 1.10001) 'Actual rotor radius/strength outside producer bounds.'}
 return $r
}
function Receipt-Matches($r,$s,$view,[bool]$positive) {
 if([Math]::Abs($r.time-$s.timeMs/1000.0) -gt .03){return $false}
 if($r.domain[2] -ne 256 -or [Math]::Abs($r.phaseTime-$r.time) -gt .001){return $false}
 $ox=[Math]::Floor(($view[0]-128)/4)*4;$oz=[Math]::Floor(($view[2]-128)/4)*4
 if([Math]::Abs($r.domain[0]-$ox) -gt .001 -or [Math]::Abs($r.domain[1]-$oz) -gt .001){return $false}
 if(!$positive){return $r.count -eq 0}
 if($r.count -lt 1){return $false}
 foreach($p in $r.sources){if([Math]::Abs($p[0]-$s.x) -le .02 -and [Math]::Abs($p[1]-$s.z) -le .02 -and [Math]::Abs($p[2]-(3+12*$s.rpm)) -le .0002 -and [Math]::Abs($p[3]-1.1*$s.rpm*$s.rpm) -le .0002){return $true}}
 return $false
}
if($SelfTest){
 $ground=@{readonly=$true;timeMs=20;object=1;class='UH60';model='data3d\uh-60.p3d';x=2475;y=26.4;z=5075;groundY=24;rpm=1;upY=1;speedX=0;speedY=0;speedZ=0;damage=0;cameraDistance=20;engineOn=$true;destroyed=$false;local=$true;airborne=$false;driverPresent=$true;driverBrain=$true;driverAlive=$true;driverIsPlayer=$true;playerManual=$true;playerSuspended=$false;cameraEffect=$false;hoverStateAvailable=$true;hoveringAutopilot=$true;pilotHeightHelper=$true;pilotWantedHeight=1;moveDown=1;fuel=1000}
 Assert-Supported $ground 2475 5075 $true
 foreach($change in @(@{airborne=$true},@{y=31},@{speedY=2.14},@{pilotWantedHeight=20.898},@{moveDown=0},@{driverIsPlayer=$false})){$bad=$ground.Clone();foreach($k in $change.Keys){$bad[$k]=$change[$k]};$failed=$false;try{Assert-Supported $bad 2475 5075 $true}catch{$failed=$true};Require $failed 'Unsupported/spinning-climb surrogate accepted as actual landing.'}
 $stopped=$ground.Clone();$stopped.x=1750;$stopped.z=3100;$stopped.y=18;$stopped.groundY=-2;$stopped.engineOn=$false;$stopped.rpm=0;$stopped.fuel=0;$stopped.airborne=$true
 Assert-StoppedSea $stopped 1750 3100 0
 foreach($change in @(@{x=2475;z=5075},@{rpm=.77},@{engineOn=$true},@{groundY=2},@{y=42},@{fuel=1000})){$bad=$stopped.Clone();foreach($k in $change.Keys){$bad[$k]=$change[$k]};$failed=$false;try{Assert-StoppedSea $bad 1750 3100 0}catch{$failed=$true};Require $failed 'Engine-off sea negative admitted a location/altitude/dry/spinning confound.'}
 foreach($angle in @('top','close')) {
  $camera=Sea-View 1750 3100 0 $angle
  Require ($camera.view.Count -eq 3 -and $camera.pose.Count -eq 6) 'Actual scalar sea camera tuple malformed.'
  $expected=if($angle -ceq 'top'){@(1750,55,3095)}else{@(1778,10,3065)}
  for($i=0;$i -lt 3;$i++){Require ($camera.view[$i] -eq $expected[$i]) 'Actual scalar camera offset changed.'}
 }
 $row='TW_ROTOR_UBO {"time":20,"phaseTime":20,"domain":[1620,2940,256,0.00390625],"count":1,"sources":[[1750,3100,15,1.1]],"readonly":true}'
 $r=Parse-Receipt $row;$s=@{timeMs=20000;x=1750;z=3100;rpm=1};$v=@(1750,55,3070)
 Require (Receipt-Matches $r $s $v $true) 'Actual packet/domain positive refused.'
 Require (!(Receipt-Matches $r @{timeMs=21000;x=1750;z=3100;rpm=1} $v $true)) 'Stale receipt admitted.'
 Require (!(Receipt-Matches $r $s @(1850,55,3070) $true)) 'Different camera domain admitted.'
 foreach($bad in @($row.Replace('"count":1','"count":9'),$row.Replace('"readonly":true','"readonly":false'),$row.Replace('"readonly":true','"readonly":"true"'),$row.Replace('15,1.1','16,1.1'),$row.Replace('"time":20','"time":"20"'),$row.Replace('[1750,3100,15,1.1]','[1750,3100,15,"NaN"]'))){$failed=$false;try{$null=Parse-Receipt $bad}catch{$failed=$true};Require $failed 'Malformed source receipt admitted.'}
 Require ((Decode-Eval '[1750,3100,18]').Count -eq 3) 'Native SQF coordinate ordering lost.'
 Write-Host 'PASS copied-UBO receipt schema/source/clock/domain falsifiers. No game.';return
}
Require (![string]::IsNullOrWhiteSpace($ExpectedCommit)) 'Expected matched installed commit is required.'
Require (![string]::IsNullOrWhiteSpace($env:LOCK_OWNER)) 'Run through with-game-lock.sh.'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve the running game.'
Require (($Arms|Select-Object -Unique).Count -eq $Arms.Count) 'Duplicate comparison arms.'
function Pair {
 $stamp=[IO.File]::ReadAllText((Join-Path $GameDir 'DEPLOYED-FROM.txt')).Trim();$parts=$stamp -split '\s+'
 Require ($parts.Count -ge 3 -and ($parts[1].StartsWith($ExpectedCommit) -or $ExpectedCommit.StartsWith($parts[1]))) 'Installed provenance does not match expected candidate.'
 return @{stamp=$stamp;files=@(@('OpenPoseidon.exe','wgpu_renderer.dll')|ForEach-Object {$f=Get-Item -LiteralPath (Join-Path $GameDir $_);@{name=$_;sha256=(Get-FileHash -LiteralPath $f.FullName).Hash;bytes=$f.Length;writtenUtc=$f.LastWriteTimeUtc.ToString('o')}})}
}
function Same-Pair($a,$b){Require ($a.stamp -ceq $b.stamp) 'Installed provenance changed.';for($i=0;$i -lt 2;$i++){Require ($a.files[$i].sha256 -ceq $b.files[$i].sha256 -and $a.files[$i].writtenUtc -ceq $b.files[$i].writtenUtc) 'Installed binary pair changed.'}}
$before=Pair;$output=Join-Path $root ('build/rotor-water/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Force $output|Out-Null
$result=[ordered]@{status='running';installed=$before;sourceHead=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash;arms=@();limits='One stock occupied UH60, Noe native sea, eight-source/256m renderer domain. OP arm establishes physical admission and captures, not a copied GPU impulse receipt. Appearance/cost require review.'}
$keys=@('POSEIDON_USER_DIR','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_RAIN_TRACE','POSEIDON_SNOW_TEST_DEPTH','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','POSEIDON_SNOWLINE','POSEIDON_ROTOR_LAND','POSEIDON_ROTOR_LAND_TRACE','POSEIDON_WIND_OVERRIDE','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE','WGR_WATER_BACKEND','WGR_WATER_SOAK','WGR_TW_ROTOR_WASH','WGR_TW_ROTOR_TRACE','WGR_AUTO_EXPOSURE')
$saved=@{};foreach($k in $keys){$saved[$k]=[Environment]::GetEnvironmentVariable($k,'Process')}
$p=$null;$client=$null;$script:heldLook=$false;$script:heldUp=$false;$script:heldDown=$false;$script:object=$null;$script:lastOwnershipCheck=[DateTime]::MinValue
function Health {
 Require ($p -and !$p.HasExited) 'Owned game exited unexpectedly.'
 if(([DateTime]::UtcNow-$script:lastOwnershipCheck).TotalSeconds -ge 5){
  Require (@(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue|Where-Object {$_.Id -ne $p.Id}).Count -eq 0) 'Foreign game appeared; preserve it and fail the comparison.'
  $script:lastOwnershipCheck=[DateTime]::UtcNow
 }
 if((Test-Path $log) -and (Select-String -LiteralPath $log -Pattern 'Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION|Unbekannter Operator' -Quiet)){throw 'Installed validation/script failure.'}
}
function Send($cmd){Health;$line=$cmd|ConvertTo-Json -Compress;$line|Add-Content -LiteralPath $harness;$writer.WriteLine($line);do{$line=$reader.ReadLine();Require ($null -ne $line) 'Harness closed.';$line|Add-Content -LiteralPath $harness;$r=$line|ConvertFrom-Json}while($null -eq $r.ok);Require $r.ok $line;return $r}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code){return Decode-Eval ([string](Send @{cmd='eval';code=$code}).result)}
function State([string]$stage){$pos=Eval 'getPosASL rotorHeli';Require ($pos.Count -eq 3) 'Actual helicopter locator missing.';$s=Send @{cmd='rotor_state';x=[double]$pos[0];z=[double]$pos[1];radius=16};Assert-State $s;if($null -eq $script:object){$script:object=$s.object};Require ($s.object -eq $script:object) 'Actual aircraft identity switched.';$arm.states+=@{stage=$stage;state=$s};return $s}
function Height([double]$x,[double]$z){$a=(Send @{cmd='water_bathymetry';x=$x;z=$z}).samples;$c=@($a|Where-Object {[Math]::Abs($_[0]-$x) -lt .001 -and [Math]::Abs($_[1]-$z) -lt .001});Require ($c.Count -eq 1) 'Native bathymetry centre missing.';return [double]$c[0][2]}
function Backend {
 $lines=@(Get-Content -LiteralPath $log|Where-Object {$_ -match 'Water backend: (initial |.* -> )(Current OP|Tidewater Native)'})
 Require ($lines.Count -gt 0) 'No actual backend initialization/switch receipt.'
 $last=$lines[-1];Require ($last -match ('Water backend: (initial |.* -> )'+[regex]::Escape($backendName)+'(?:\s|$)')) 'Actual water backend differs from requested arm.';$arm.backend=$lines
}
function Receipt($state,$view,[bool]$positive){
 if($arm.name -ceq 'CurrentOP'){return @{status='physical-source-only-existing-OP-path';state=$state}}
 $deadline=[DateTime]::UtcNow.AddSeconds(8)
 do{Health;$rows=@();if(Test-Path $stderr){foreach($line in Get-Content -LiteralPath $stderr){if($line.StartsWith('TW_ROTOR_UBO ')){$rows+=Parse-Receipt $line}}}
  $found=@($rows|Where-Object {Receipt-Matches $_ $state $view $positive})
  if($found.Count){$r=$found[-1];$arm.receipts+=,$r;return $r}
  Require ([DateTime]::UtcNow -lt $deadline) 'No fresh matching actual copied rotor UBO receipt.';Start-Sleep -Milliseconds 100
 }while($true)
}
function Capture([string]$stage,[string]$angle,[bool]$positive){
 $null=Eval 'triClearView';$beforeState=State ($stage+'-'+$angle+'-before');$time=[double](Eval 'time')
 $camera=Sea-View $SeaX $SeaZ $script:seaLevel $angle;$view=$camera.view;$pose=$camera.pose
 Require ((Eval ('triSetView ['+(($pose|ForEach-Object {Number $_}) -join ',')+']')) -ceq 'OK') 'Render-only sea camera refused.'
 Start-Sleep -Milliseconds 250;$s=State ($stage+'-'+$angle+'-view');$receipt=Receipt $s $view $positive
 $file=Join-Path $armDir ($stage+'-'+$angle+'.png');$null=Send @{cmd='screenshot';path=$file};$deadline=[DateTime]::UtcNow.AddSeconds(10)
 while(!(Test-Path $file)){Health;Require ([DateTime]::UtcNow -lt $deadline) 'Screenshot did not complete.';Start-Sleep -Milliseconds 100}
 Require ((Get-Item $file).Length -gt 100) 'Screenshot empty.'
 $after=State ($stage+'-'+$angle+'-after');Require ([Math]::Abs([double](Eval 'time')-$time) -lt .0001 -and $beforeState.timeMs -eq $after.timeMs) 'Paused capture advanced simulation.'
 foreach($k in @('x','y','z','rpm')){Require ([Math]::Abs($beforeState.$k-$after.$k) -lt .001) 'Render-only capture moved actual rotor.'}
 $arm.captures[$stage+'-'+$angle]=@{path=$file;sha256=(Get-FileHash -LiteralPath $file).Hash;state=$s;receipt=$receipt;view=$view;simulationTime=$time}
 Require ((Eval 'triClearView') -ceq 'OK') 'Render camera lease did not clear.'
}
function Wait-Physical([string]$stage,[double]$x,[double]$z,[double]$surface,[bool]$high=$false){
 $until=[DateTime]::UtcNow.AddSeconds(60);Exec 'setAccTime 1';$settled=0
 do{Start-Sleep -Milliseconds 400;$s=State ($stage+'-wait');$good=$true;try{Assert-Hover $s $x $z $surface $high}catch{$good=$false};if($good){$settled++}else{$settled=0};Require (!$s.destroyed -and $s.damage -eq 0) 'Actual physical aircraft damaged during hover.';Require ([DateTime]::UtcNow -lt $until) 'Actual stock auto-hover unavailable; no pinning fallback.'}while($settled -lt 3)
 Exec 'setAccTime 0';return State ($stage+'-ready')
}
function Place([string]$stage,[double]$x,[double]$z,[double]$surface,[double]$height,[bool]$high=$false){
 Require ((Eval 'triClearView') -ceq 'OK') 'Relocation camera lease remained active.'
 Exec ('rotorHeli setPosASL ['+(Number $x)+','+(Number $z)+','+(Number ($surface+$height))+']; rotorHeli flyInHeight '+(Number $height))
 $null=Send @{cmd='key';sc=20;hold=$true};$script:heldUp=$true;Exec 'setAccTime 1';Start-Sleep -Milliseconds 250;$null=Send @{cmd='key_up';sc=20};$script:heldUp=$false;Exec 'setAccTime 0'
 return Wait-Physical $stage $x $z $surface $high
}
function Observe([string]$stage,[double]$x,[double]$z,[double]$surface,[bool]$high=$false){
 $start=[double](Eval 'time');$deadline=[DateTime]::UtcNow.AddSeconds(3*$ObserveSeconds+15);$states=@();Exec 'setAccTime 1'
 do{Start-Sleep -Milliseconds 400;$s=State ($stage+'-observed');Assert-Hover $s $x $z $surface $high;$states+=,$s;$now=[double](Eval 'time');Require ([DateTime]::UtcNow -lt $deadline) 'Actual physics observation timed out.'}while($now-$start -lt $ObserveSeconds)
 Exec 'setAccTime 0';$arm.gates[$stage]=@{start=$start;end=$now;states=$states};Backend
}
function Land-Owned {
 Require ((Eval 'triClearView') -ceq 'OK') 'Landing retained a render camera lease.'
 $start=State 'landing-start';Assert-Hover $start $DryX $DryZ $dry
 $null=Send @{cmd='key';sc=29;hold=$true};$script:heldDown=$true;Exec 'setAccTime 1'
 $until=[DateTime]::UtcNow.AddSeconds(60);$settled=0;$states=@()
 do{
  Start-Sleep -Milliseconds 250;$s=State 'ordinary-Z-landing';Assert-Pilot $s
  Require ($s.engineOn -and $s.rpm -ge .65 -and [Math]::Abs($s.x-$DryX) -le 8 -and [Math]::Abs($s.z-$DryZ) -le 8) 'Actual powered landing lost the controlled aircraft.'
  Require ((Finite $s.moveDown) -and $s.moveDown -gt .5) 'Stock Z did not resolve to actual helicopter collective down.'
  $good=$true;try{Assert-Supported $s $DryX $DryZ $true}catch{$good=$false}
  if($good){$settled++}else{$settled=0};$states+=,$s
  Require ([DateTime]::UtcNow -lt $until) 'Ordinary stock collective-down did not produce supported landing.'
 }while($settled -lt 3)
 Exec 'setAccTime 0';$landed=State 'ordinary-Z-landed';Assert-Supported $landed $DryX $DryZ $true
 $arm.gates.landing=@{start=$start;landed=$landed;states=$states;input='Ordinary SDL Z collective down held through powered supported landing and subsequent spool-down; no ground teleport or helper setter'}
 return $landed
}
function Close-Owned {
 if($script:heldDown){$null=Send @{cmd='key_up';sc=29};$script:heldDown=$false}
 if($script:heldUp){$null=Send @{cmd='key_up';sc=20};$script:heldUp=$false};if($script:heldLook){$null=Send @{cmd='key_up';sc=226};$script:heldLook=$false}
 Exec 'setAccTime 1';$null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Owned game failed normal exit zero.'
 Require (Select-String -LiteralPath $log -Pattern 'Shutdown complete' -Quiet) 'Normal shutdown marker absent.';$arm.exitCode=$p.ExitCode
}
try{
 foreach($name in $Arms){
  Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Preserve a game between comparison arms.';Same-Pair $before (Pair)
  foreach($k in $keys){[Environment]::SetEnvironmentVariable($k,$null,'Process')}
  $armDir=Join-Path $output $name;$profile=Join-Path $armDir 'user';New-Item -ItemType Directory -Force $profile|Out-Null
  $log=Join-Path $armDir 'engine.log';$stderr=Join-Path $armDir 'stderr.txt';$harness=Join-Path $armDir 'harness.jsonl';$script:object=$null
  $backendName=if($name -ceq 'CurrentOP'){'Current OP'}else{'Tidewater Native'}
  $arm=[ordered]@{name=$name;status='running';backend=@();states=@();receipts=@();gates=@{};captures=@{};directory=$armDir};$result.arms+=,$arm
  $env:POSEIDON_USER_DIR=$profile;$env:POSEIDON_SNOWLINE='off';$env:POSEIDON_WIND_OVERRIDE='0 90 0';$env:POSEIDON_ROTOR_LAND='0';$env:WGR_AUTO_EXPOSURE='0'
  $env:WGR_WATER_BACKEND=if($name -ceq 'CurrentOP'){'0'}else{'1'};$env:WGR_TW_ROTOR_WASH=if($name -ceq 'TidewaterOff'){'0'}else{'1'};$env:WGR_TW_ROTOR_TRACE='1'
  [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`nbrightness=1;`n")
  $mission=Join-Path $root 'tests/perf/missions/perf_sand.noe';Require (Test-Path $mission) 'Original Noe player mission missing.'
  $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
  $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
  $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $args -RedirectStandardOutput (Join-Path $armDir 'stdout.txt') -RedirectStandardError $stderr;$null=$p.Handle;$arm.pid=$p.Id
  $until=[DateTime]::UtcNow.AddSeconds(120)
  do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Harness unavailable.';Start-Sleep -Milliseconds 250}}while(!$client)
  $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
  Start-Sleep -Seconds 12;Require ((Eval 'triSceneReady') -ceq 'OK') 'Original terrain scene not ready.';Backend
  Exec 'setAccTime 0;player allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0;setDate [1985,6,21,16,0]';$null=Send @{cmd='dev_snow';action='disable'};$null=Send @{cmd='weather_particles';action='set';mode='particle';snowflakes=$false}
  $datum=Send @{cmd='dev_rain_water';action='fine_state'};Require (Finite $datum.seaLevel) 'Native sea datum missing.';$script:seaLevel=[double]$datum.seaLevel
  $bath=(Send @{cmd='water_bathymetry';x=$SeaX;z=$SeaZ}).samples;$near=@($bath|Where-Object {[Math]::Abs($_[0]-$SeaX) -le 32 -and [Math]::Abs($_[1]-$SeaZ) -le 32})
  Require ($near.Count -eq 25 -and @($near|Where-Object {$_[2] -ge $script:seaLevel-.03}).Count -eq 0) 'Audited native sea footprint no longer underwater.';$arm.gates.sea=@{datum=$script:seaLevel;nativeSamples=$near;x=$SeaX;z=$SeaZ}
  $dry=Height $DryX $DryZ;Require ($dry -gt $script:seaLevel+1) 'Dry ground stage is sea.'
  $xz=(Number $DryX)+','+(Number $DryZ)
  Exec ('rotorHeli="UH60" createVehicle ['+$xz+',0];rotorHeli allowDamage false;rotorHeli setFuel 0;rotorHeli engineOn false;rotorPilot=player;rotorPilot moveInDriver rotorHeli;rotorHeli setPos ['+$xz+',0];rotorPilot action ["AUTOHOVER",rotorHeli];rotorHeli switchCamera "EXTERNAL"')
  Require ((Eval 'typeOf driver rotorHeli') -ceq 'SoldierWB' -and (Eval 'format ["%1",player]') -ceq (Eval 'format ["%1",rotorPilot]')) 'Actual stock controlled pilot changed.'
  $initial=State 'grounded-engine-off';Require (!$initial.engineOn -and $initial.rpm -le .02) 'Initial rotor is spinning.';$arm.gates.initialEngineOff=$initial
  Exec 'rotorHeli setFuel 1;rotorHeli engineOn true';$until=[DateTime]::UtcNow.AddSeconds(40);Exec 'setAccTime 1'
  do{Start-Sleep -Milliseconds 400;$s=State 'grounded-spool';Require (!$s.destroyed -and $s.damage -eq 0 -and $s.y-$s.groundY -lt 6) 'Grounded spool lost support.';Require ([DateTime]::UtcNow -lt $until) 'Grounded actual RPM unavailable.'}while($s.rpm -lt .85)
  Exec 'setAccTime 0';$null=Send @{cmd='key';sc=226;hold=$true};$script:heldLook=$true
  $null=Place 'sea-low' $SeaX $SeaZ $script:seaLevel $HoverHeight;Observe 'sea-low' $SeaX $SeaZ $script:seaLevel
  $positive=$name -ceq 'TidewaterOn';Capture 'sea-low' 'close' $positive;Capture 'sea-low' 'top' $positive
  $paused=State 'pause-before';$t=[double](Eval 'time');Start-Sleep -Seconds 3;$held=State 'pause-after';Require ($paused.timeMs -eq $held.timeMs -and [Math]::Abs([double](Eval 'time')-$t) -lt .0001) 'Pause advanced physical clock.';foreach($k in @('x','y','z','rpm')){Require ([Math]::Abs($paused.$k-$held.$k) -lt .001) 'Paused rotor changed.'};$arm.gates.pause=@{before=$paused;after=$held}
  $null=Place 'sea-high' $SeaX $SeaZ $script:seaLevel 42 $true;Observe 'sea-high' $SeaX $SeaZ $script:seaLevel $true;Capture 'sea-high' 'close' $false
  $null=Place 'sea-return' $SeaX $SeaZ $script:seaLevel $HoverHeight;Observe 'sea-return' $SeaX $SeaZ $script:seaLevel;Capture 'sea-return' 'close' $positive
  $null=Place 'dry-hover' $DryX $DryZ $dry $HoverHeight;Observe 'dry-hover' $DryX $DryZ $dry;Capture 'dry-hover' 'top' $false
  # Land through the real keyboard collective. A ground teleport leaves the
  # previous manual height target intact and makes the residual rotor climb.
  $null=Land-Owned
  Exec 'rotorHeli engineOn false;rotorHeli setFuel 0;setAccTime 1';$until=[DateTime]::UtcNow.AddSeconds(60)
  do{Start-Sleep -Milliseconds 400;$s=State 'grounded-spool-down';Assert-Supported $s $DryX $DryZ $true;Require (!$s.engineOn) 'Supported shutdown restarted its engine.';Require ([DateTime]::UtcNow -lt $until) 'Actual stopped engine retained spinning rotor.'}while($s.rpm -gt .02)
  Exec 'setAccTime 0';$arm.gates.groundedEngineOff=State 'grounded-engine-off-ready'
  $null=Send @{cmd='key_up';sc=29};$script:heldDown=$false
  # One relocation, followed by real gravity, isolates engine/RPM off inside
  # the same sea/altitude/camera domain. This is not an engine-off hover.
  Exec ('rotorHeli setPosASL ['+(Number $SeaX)+','+(Number $SeaZ)+','+(Number ($script:seaLevel+$HoverHeight))+'];setAccTime 1')
  $seaStart=State 'engine-off-sea-gravity-start';Assert-StoppedSea $seaStart $SeaX $SeaZ $script:seaLevel
  Start-Sleep -Milliseconds 400;$falling=State 'engine-off-sea-gravity';Assert-StoppedSea $falling $SeaX $SeaZ $script:seaLevel
  Require ($falling.timeMs -gt $seaStart.timeMs -and $falling.y -lt $seaStart.y -and $falling.speedY -lt -.1) 'Stopped sea aircraft did not evolve under actual gravity.'
  Exec 'setAccTime 0';$arm.gates.finalEngineOff=State 'final-engine-off-sea';Assert-StoppedSea $arm.gates.finalEngineOff $SeaX $SeaZ $script:seaLevel
  $arm.gates.engineOffGravity=@{before=$seaStart;after=$falling;scope='Single low-sea relocation and ordinary gravity; paused for safe source/camera negative, no velocity/RPM pin'}
  Capture 'engine-off-return-sea-view' 'close' $false
  Close-Owned;Same-Pair $before (Pair);$client.Dispose();$client=$null;$p=$null;$arm.status='physical-source-clock-lifecycle-passed-visual-cost-review-pending'
 }
 $result.status='bounded-comparison-captured-visual-cost-review-pending'
}catch{$result.status='failed';$result.error=$_.Exception.Message;$result.errorSource=@{line=$_.InvocationInfo.ScriptLineNumber;position=$_.InvocationInfo.PositionMessage;stack=$_.ScriptStackTrace}}
finally{
 if($p -and !$p.HasExited -and $client){try{Close-Owned}catch{$result.cleanupError=$_.Exception.Message;$result.status='failed'}}
 if($client){$client.Dispose()};if($p -and !$p.HasExited){$result.status='failed';$result.forcedCleanup=$true;$p.Kill();$null=$p.WaitForExit(10000)}
 foreach($k in $keys){[Environment]::SetEnvironmentVariable($k,$saved[$k],'Process')}
 try{$result.after=Pair;Same-Pair $before $result.after}catch{$result.status='failed';$result.provenanceError=$_.Exception.Message}
 $result|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $output 'result.json');Write-Host "Actual sea rotor evidence: $output"
 if($result.status -eq 'failed'){throw ($result.error+' '+$result.cleanupError+' '+$result.provenanceError)}
}
