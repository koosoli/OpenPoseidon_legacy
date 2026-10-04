# Owner-requested visible original Everon smoke handoff; successful game stays open.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
if(!$env:LOCK_OWNER){throw 'Game lock required'}
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Preserving existing game'}
$stamp=Get-Content -LiteralPath (Join-Path $game 'DEPLOYED-FROM.txt') -Raw
if($stamp -notmatch [regex]::Escape($ExpectedCommit)){throw 'Installed source differs'}
$pair=@{};foreach($name in @('OpenPoseidon.exe','wgpu_renderer.dll')){$pair[$name]=(Get-FileHash -LiteralPath (Join-Path $game $name)).Hash}
$out=Join-Path $root ('build/owner-ragdoll-fog/'+(Get-Date -Format yyyyMMdd-HHmmss));$profile=Join-Path $out 'user'
New-Item -ItemType Directory -Force -Path $profile | Out-Null
$settings=@{POSEIDON_USER_DIR=$profile;WGR_LAYERED_FOG_TRACE='1';WGR_WEATHER_COVER='1';WGR_WEATHER_COVER_FAR='1';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='0.6';WGR_TONEMAP='1';POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0'}
$clear=@('POSEIDON_AUTOMATIC_RAGDOLL','WGR_LAYERED_FOG','WGR_FOG_LAYER0','WGR_FOG_LAYER1','WGR_FOG_TERRAIN','WGR_FOG_PATCH','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','POSEIDON_TEST_RAIN','POSEIDON_SNOW_TEST_RATE','POSEIDON_SNOW_TEST_FLAKES','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')
$saved=@{};foreach($key in @($settings.Keys)+$clear){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
function Send($command){$writer.WriteLine(($command|ConvertTo-Json -Depth 8 -Compress));do{$line=$reader.ReadLine();if(!$line){throw 'Harness disconnected'};$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok);if(!$reply.ok){throw $line};return $reply}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code){$v=([string](Send @{cmd='eval';code=$code}).result).Trim();if($v.StartsWith('"')){return $v.Substring(1,$v.Length-2).Replace('""','"')};return ConvertFrom-Json -InputObject $v -NoEnumerate}
function AimTarget([double]$targetX,[double]$targetY,[double]$targetZ){
 # Reset inherited mouse look through ordinary SDL input, never pin the camera.
 # Read the actual selected muzzle and calibrate each axis before bounded correction.
 $proof=@();foreach($axis in @('pitch','yaw')){
  $first=Send @{cmd='player_weapon_state'}
  $firstAngle=if($axis -eq 'pitch'){[Math]::Asin([Math]::Clamp([double]$first.directionY,-1,1))}else{[Math]::Atan2([double]$first.directionX,[double]$first.directionZ)}
  $null=Send @{cmd='mouse_motion';dx=$(if($axis -eq 'yaw'){8}else{0});dy=$(if($axis -eq 'pitch'){8}else{0})};Start-Sleep -Milliseconds 250
  $next=Send @{cmd='player_weapon_state'}
  $nextAngle=if($axis -eq 'pitch'){[Math]::Asin([Math]::Clamp([double]$next.directionY,-1,1))}else{[Math]::Atan2([double]$next.directionX,[double]$next.directionZ)}
  $change=[Math]::Atan2([Math]::Sin($nextAngle-$firstAngle),[Math]::Cos($nextAngle-$firstAngle));$gain=$change/8
  if([Math]::Abs($gain) -lt .00005 -or [Math]::Abs($gain) -gt .1){throw ('Mouse '+$axis+' calibration unavailable: '+$gain)}
  $aligned=$false
  for($attempt=0;$attempt -lt 16;$attempt++){
   $current=Send @{cmd='player_weapon_state'};$rx=$targetX-[double]$current.muzzleX;$ry=$targetY-[double]$current.muzzleY;$rz=$targetZ-[double]$current.muzzleZ
   $angle=if($axis -eq 'pitch'){[Math]::Asin([Math]::Clamp([double]$current.directionY,-1,1))}else{[Math]::Atan2([double]$current.directionX,[double]$current.directionZ)}
   $wanted=if($axis -eq 'pitch'){[Math]::Atan2($ry,[Math]::Sqrt($rx*$rx+$rz*$rz))}else{[Math]::Atan2($rx,$rz)}
   $difference=[Math]::Atan2([Math]::Sin($wanted-$angle),[Math]::Cos($wanted-$angle))
   if([Math]::Abs($difference) -le .05){$aligned=$true;$proof+=@{axis=$axis;errorRadians=$difference;gainRadiansPerPixel=$gain;attempts=$attempt};break}
   $pixels=[int][Math]::Clamp([Math]::Round($difference/$gain),-120,120)
   $null=Send @{cmd='mouse_motion';dx=$(if($axis -eq 'yaw'){$pixels}else{0});dy=$(if($axis -eq 'pitch'){$pixels}else{0})};Start-Sleep -Milliseconds 250
  }
  if(!$aligned){throw ('Bounded ordinary mouse '+$axis+' alignment failed')}
 }
 return $proof
}
$p=$null;$client=$null;$handoff=$false;$held=$false
try{
 # Current PowerShell preserves null assignments as empty child variables.
 # Remove startup overrides entirely: an empty fog/profile override refuses fog.
 foreach($key in $clear){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue};foreach($key in $settings.Keys){[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
 [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $mission=Join-Path $root 'tests/perf/missions/perf_field.eden';$log=Join-Path $out 'engine.log'
 $argv=@('--render=wgpu','--window','--dev','--width','1600','--height','900','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','16','--log-file',('"'+$log+'"'))
 # Visible window explicitly requested by the owner; this follows Show-CorpseRagdollPrototype's handoff pattern.
 $p=Start-Process -FilePath (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Normal -ArgumentList $argv -PassThru
 $until=[DateTime]::UtcNow.AddSeconds(120)
 do{if($p.HasExited){throw 'Game exited during startup'};$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){if([DateTime]::UtcNow -gt $until){throw 'Startup deadline'};Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 while((Eval 'triSceneReady') -cne 'OK'){if([DateTime]::UtcNow -gt $until){throw 'Scene-ready deadline'};Start-Sleep -Milliseconds 300}
 Exec 'setAccTime 0;player allowDamage false;player setPos [5478,9988,0];player setDir 60;0 setRain 0;0 setOvercast 0;0 setFog 0.25;setDate [1985,6,21,16,0];smokeEast=createCenter east;smokeGroup=createGroup east'
 for($i=0;$i -lt 6;$i++){$x=5500+($i*5);Exec ('"SoldierWB" createUnit [['+$x+',10000,0],smokeGroup,"smokeTarget'+$i+'=this;removeAllWeapons this;this disableAI ""MOVE"";this setCombatMode ""BLUE"";this setBehaviour ""CARELESS"""];smokeTarget'+$i+' setDir 240;smokeTarget'+$i+' setUnitPos "UP"');if((Eval ('alive smokeTarget'+$i)) -ne $true){throw 'Live target missing'}}
 # Preserve the stock kit; explicitly select a loaded primary before input tests.
 Exec 'player addMagazine "M16";player addMagazine "M16";player addMagazine "M16";player selectWeapon "M16";player setUnitPos "UP";player switchCamera "INTERNAL";disableUserInput false;triClearView;player setDir 180;setAccTime 1'
 Start-Sleep -Seconds 3
 $auto=Eval 'triPhysicsShowcase "corpse-auto-status"';if($auto -notmatch '^OK corpse-auto-status enabled=1'){throw 'Automatic ragdoll disabled'}
 $weather=Send @{cmd='weather_visibility'}
 $weather|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $out 'weather-preflight.json')
 if([Math]::Abs([double]$weather.fog-.25) -gt .001){throw ('Requested fog has not settled: '+$weather.fog)}
 if($weather.rendererSky.layeredFog -ne $true){throw ('Default volumetric fog not enabled: '+($weather|ConvertTo-Json -Depth 8 -Compress))}
 if((Eval 'triGetInputContext') -cne 'Infantry' -or [double](Eval 'triGetCameraEffectActive') -ne 0){throw 'Ordinary infantry controls not active'}
 $ammo=[double](Eval 'player ammo "M16"')
 if($ammo -le 0 -or [double](Eval 'triPlayerCurrentMagazineAmmo') -ne $ammo){throw 'Loaded M16 is not selected'}
 Exec 'smokeProbe=true;smokeShots=0;player addEventHandler ["Fired",{smokeShots=smokeShots+1;if (smokeProbe) then {setAccTime 0}}];setAccTime 0.05'
 $null=Send @{cmd='mouse_button';button=1;down=$true};$held=$true
 $until=[DateTime]::UtcNow.AddSeconds(20)
 do{Start-Sleep -Milliseconds 30;$shots=[int](Eval 'smokeShots');if([DateTime]::UtcNow -gt $until){throw 'Ordinary left click did not fire'}}while($shots -lt 1)
 $null=Send @{cmd='mouse_button';button=1;down=$false};$held=$false
 $after=[double](Eval 'player ammo "M16"')
 if($shots -ne 1 -or $after -ne $ammo-1){throw 'Actual shot/ammunition preflight differs'}
 $inputProof=@{context='Infantry';cameraEffect=0;selectedAmmoBefore=$ammo;selectedAmmoAfter=$after;fired=$shots;input='Actual SDL mouse button 1, no script fire'}
 Exec 'smokeProbe=false;player setDir 60;setAccTime 1'
 Start-Sleep -Seconds 4
 $target=Eval 'getPosASL smokeTarget0';$aimProof=AimTarget ([double]$target[0]) ([double]$target[2]+1.15) ([double]$target[1])
 $pose=Eval 'getPosASL player'
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'ready.png')}
 if($p.HasExited){throw 'Game exited before handoff'}
 @{status='left-open-for-owner';pid=$p.Id;installed=$stamp.Trim();pair=$pair;inputProof=$inputProof;aimProof=$aimProof;targets=6;playerASL=$pose;automatic=$auto;weather=$weather;environment=$settings;log=$log;scope='Interactive original Everon smoke scene with actual rifle input preflight; no automatic victim kill or acceptance inference.'}|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $out 'session.json')
 $handoff=$true;Write-Output "Game left open: PID $($p.Id); $out"
}finally{
 if($held -and $client){try{$null=Send @{cmd='mouse_button';button=1;down=$false}}catch{}}
 if(!$handoff -and $p -and !$p.HasExited){if($client){try{$null=Send @{cmd='exit'}}catch{}};if(!$p.WaitForExit(15000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()}
 foreach($key in $saved.Keys){if($null -eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
}
