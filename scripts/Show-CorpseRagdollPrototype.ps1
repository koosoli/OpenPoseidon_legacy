# Interactive owner handoff. Run under with-game-lock.sh; the successful game stays open.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit,
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Run through with-game-lock.sh.'}
if(Get-Process OpenPoseidon -ErrorAction SilentlyContinue){throw 'Preserving the existing game session.'}
$root=Split-Path -Parent $PSScriptRoot
$stamp=Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw
if($stamp -notmatch ('\b'+[regex]::Escape($ExpectedCommit)+'[a-fA-F0-9]*\b')){throw 'Installed commit differs.'}
$output=Join-Path $root ('build/corpse-demo/'+(Get-Date -Format yyyyMMdd-HHmmss))
$profile=Join-Path $output 'user';New-Item -ItemType Directory -Force -Path $profile | Out-Null
$mission=Join-Path $root 'tests/perf/missions/perf_field.eden'
$addons='C:/Program Files (x86)/Steam/steamapps/common/Arma Reforger/addons'
foreach($path in @($mission,$addons)){if(!(Test-Path -LiteralPath $path)){throw "Missing fixture: $path"}}
$settings=@{POSEIDON_USER_DIR=$profile;POSEIDON_REFORGER_WORLD='worlds/eden';POSEIDON_REFORGER_OBJECTS='1';POSEIDON_REFORGER_STREAM='1';WGR_TEMPORAL='0';WGR_AUTO_EXPOSURE='0';WGR_EXPOSURE='1';WGR_GRASS='0';POSEIDON_WIND_OVERRIDE='0 90 0';WGR_CLOUD_COVERAGE='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_TERRAIN_JITTER='0';POSEIDON_TEST_RAIN=$null;POSEIDON_SNOWLINE='off';POSEIDON_SNOW_TEST_DEPTH='0';WGR_SCREENSHOT_POST_OPTICS=$null}
$saved=@{};foreach($key in $settings.Keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');if($null -eq $settings[$key]){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}}
$p=$null;$client=$null;$handoff=$false
function Send($command){
 $writer.WriteLine(($command|ConvertTo-Json -Compress));$deadline=[DateTime]::UtcNow.AddSeconds(30)
 do{
  if([DateTime]::UtcNow -gt $deadline){throw 'Harness response deadline exceeded.'}
  $line=$reader.ReadLine()
  if(!$line){throw 'Game harness disconnected.'};$reply=$line|ConvertFrom-Json
 }while($null -eq $reply.ok)
 if(!$reply.ok){throw $line};return $reply
}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Eval([string]$code){
 $text=([string](Send @{cmd='eval';code=$code}).result).Trim()
 if($text.StartsWith('"')){return $text.Substring(1,$text.Length-2).Replace('""','"')}
 return ConvertFrom-Json -InputObject $text -NoEnumerate
}
function Query([string]$command){return Eval ('triPhysicsShowcase "'+$command+'"')}
function Position-DemoCamera{
 $position=Eval 'getPosASL ragdollDemoCorpse'
 $pose=@([double]$position[0],([double]$position[1]-4),([double]$position[2]+2.7),0,-34)
 $coords=($pose|ForEach-Object{$_.ToString('R',[Globalization.CultureInfo]::InvariantCulture)}) -join ' '
 if((Eval ('triFreeFlyPose "'+$coords+'"')) -cne 'OK'){throw 'Demo camera refused.'}
}
try{
 [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $log=Join-Path $output 'engine.log'
 $arguments=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$addons+'"'),'--test-world-hour','16','--test-world-freefly','6532','6466','100','0','-30','--log-file',('"'+$log+'"'))
 # The owner requested a visible interactive game.
 $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Normal -PassThru -ArgumentList $arguments
 $until=[DateTime]::UtcNow.AddSeconds(120)
 do{
  if($p.HasExited){throw "Game exited: $($p.ExitCode)"}
  $client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null}
  if(!$client){if([DateTime]::UtcNow -gt $until){throw 'Harness startup deadline.'};Start-Sleep -Milliseconds 250}
 }while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000
 $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 Start-Sleep -Seconds 12
 if((Eval 'triSceneReady') -cne 'OK'){throw 'Mission is not ready.'}
 Exec 'player allowDamage false;0 setFog 0;0 setOvercast 0;0 setRain 0;setDate [1985,6,21,16,0];setAccTime 1'
 Exec '"SoldierWB" createUnit [getPos player,group player,"ragdollDemoCorpse=this;removeAllWeapons this;this disableAI ""MOVE"";this setCombatMode ""BLUE"""];ragdollDemoCorpse setDir 0;ragdollDemoCorpse setDammage 1'
 Position-DemoCamera
 $until=[DateTime]::UtcNow.AddSeconds(40)
 do{$capture=Query 'corpse-capture';if($capture -notmatch '^OK corpse-capture '){if($capture -notmatch 'settled-death-pose-required|dry-ground-contact-required|no-nearby-corpse' -or [DateTime]::UtcNow -gt $until){throw $capture};Start-Sleep -Milliseconds 250}}while($capture -notmatch '^OK corpse-capture ')
 Exec 'setAccTime 0'
 Position-DemoCamera
 foreach($command in @('corpse-prepare','corpse-articulate-hinges','corpse-impulse')){$reply=Query $command;if($reply -notmatch '^OK '){throw $reply}}
 Exec 'setAccTime 1';$start=[double](Eval 'time');$until=[DateTime]::UtcNow.AddSeconds(15)
 do{Start-Sleep -Milliseconds 100;if($p.HasExited -or [DateTime]::UtcNow -gt $until){throw 'Prototype motion deadline.'};$now=[double](Eval 'time')}while($now-$start -lt .65)
 Exec 'setAccTime 0';$motion=Query 'corpse-articulation-status';$frozen=Query 'corpse-freeze'
 if($frozen -notmatch '^OK '){throw $frozen}
 $physics=Query 'corpse-physics-status';if($physics -notmatch 'articulatedBodies=0 articulatedJoints=0$'){throw $physics}
 Exec 'ragdollDemoBase=getPos player;"SoldierWB" createUnit [[(ragdollDemoBase select 0)+4,(ragdollDemoBase select 1)+4,0],group player,"removeAllWeapons this;this disableAI ""MOVE"";this setCombatMode ""BLUE"""];"SoldierWB" createUnit [[(ragdollDemoBase select 0)-4,(ragdollDemoBase select 1)+6,0],group player,"removeAllWeapons this;this disableAI ""MOVE"";this setCombatMode ""BLUE"""]'
 Exec 'setAccTime 1'
 $null=Send @{cmd='screenshot';path=(Join-Path $output 'ready.png')}
 @{status='handed-to-owner';pid=$p.Id;installed=$stamp.Trim();motion=$motion;frozen=$frozen;physics=$physics;scope='One controlled stock corpse, small test impulse and retained pose; normal deaths and grenade impulses remain authored.';controls='Ctrl+backtick > Physics: Clear, Capture, Prepare corpse ground, Start jointed corpse, Push, Freeze. Game/freefly throw uses G but is not connected to this rig.'}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $output 'session.json')
 $handoff=$true;Write-Host "Prototype open for owner: PID $($p.Id), $output"
}finally{
 if(!$handoff -and $p -and !$p.HasExited){if($client){try{$null=Send @{cmd='exit'}}catch{}};if(!$p.WaitForExit(15000)){Stop-Process -Id $p.Id -Force}}
 if($client){$client.Dispose()}
 foreach($key in $saved.Keys){if($null -eq $saved[$key]){Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
}
