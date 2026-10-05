<# Installed original Everon player, external-camera orbit and actual walking input. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,[string]$Label="player-orbit",[switch]$NoSkinBake,[double]$Radius=6)
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Game lock required'}
$root=Split-Path $PSScriptRoot -Parent
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Existing game preserved'}
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if(!$stamp.Contains($ExpectedCommit)){throw 'Installed source differs'}
$out=Join-Path $root ('build/player-visibility/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Path $out -Force
$mission=Join-Path $out "orbit.eden";$null=New-Item -ItemType Directory -Path $mission
Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Destination $mission
$receipt=@{installed=$stamp;views=@();passed=$false};$p=$null;$client=$null;$writer=$null
function Require($ok,$message){if(!$ok){throw $message}}
function Send($cmd){$writer.WriteLine(($cmd|ConvertTo-Json -Depth 10 -Compress));do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require $reply.ok $line;$line|Add-Content (Join-Path $out 'harness.jsonl');return $reply}
function Exec($code){$null=Send @{cmd='exec';code=$code}}
try{
 $env:POSEIDON_USER_DIR=Join-Path $out 'user';$null=New-Item -ItemType Directory -Path $env:POSEIDON_USER_DIR
 [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;
msaaSamples=4;
dlssMode=0;
vsync=0;
fpsCap=60;
")
 $env:WGR_LAYERED_FOG='1';$env:WGR_LAYERED_FOG_TRACE='1';$env:WGR_WEATHER_COVER_TRACE='1';$env:WGR_GRASS='0';$env:WGR_TEMPORAL='0';$env:WGR_WET_SOIL_DIAGNOSTIC='1'
 $env:POSEIDON_PLAYER_GROUND_TRACE='1';if($NoSkinBake){$env:WGR_SKIN_BAKE='0'}else{Remove-Item Env:WGR_SKIN_BAKE -ErrorAction SilentlyContinue}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $log=Join-Path $out 'engine.log';$stdout=Join-Path $out 'stdout.log';$stderr=Join-Path $out 'stderr.log'
 $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','8000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','12','--log-file',('"'+$log+'"'))
 $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $args -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(180)
 do{Require (!$p.HasExited) 'Startup exited';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$deadline) 'Startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 while(([string](Send @{cmd='eval';code='triSceneReady'}).result).Trim('"')-cne'OK'){Require ([DateTime]::UtcNow-lt$deadline) 'Scene-ready deadline';Start-Sleep -Milliseconds 300}
 Exec 'player setPos [9300,1900,0];player allowDamage false;0 setRain 0;0 setFog 0;0 setOvercast 0;setDate [1985,6,21,12,0];setAccTime 1;player switchCamera "EXTERNAL"'
 Start-Sleep -Seconds 3
 Exec 'setAccTime 0'
 $pos=([string](Send @{cmd='eval';code='getPosASL player'}).result)|ConvertFrom-Json
 $receipt.player=Send @{cmd='diag_inspect';unit='player'}
 $receipt.skinBakeDisabled=[bool]$NoSkinBake;$receipt.radius=$Radius
 foreach($angle in @(0,45,90,135,180,225,270,315)){
  $r=$angle*[Math]::PI/180;$px=$pos[0]+$Radius*[Math]::Sin($r);$pz=$pos[1]+$Radius*[Math]::Cos($r);$py=$pos[2]+2.3
  $dx=$pos[0]-$px;$dz=$pos[1]-$pz;$dy=-1.2
  $tuple=(@($px,$py,$pz,$dx,$dy,$dz)|ForEach-Object {$_.ToString('R',[Globalization.CultureInfo]::InvariantCulture)})-join ','
  Exec ('triSetView ['+$tuple+']');Start-Sleep -Milliseconds 800
  $path=Join-Path $out ('orbit-'+$angle+'.png');$null=Send @{cmd='screenshot';path=$path}
  $receipt.views+=@{angle=$angle;pose=$tuple;image=$path}
 }
 Exec 'setAccTime 1';$null=Send @{cmd='key';sc=26;hold=$true};Start-Sleep -Seconds 3;$null=Send @{cmd='key_up';sc=26}
 $receipt.walk=Send @{cmd='diag_inspect';unit='player'}
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)-and$p.ExitCode-eq0) 'Normal exit failed'
 Require (!(Select-String -Path @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed' -Quiet)) 'Runtime error'
 Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Installed pair changed'
 $receipt.passed=$true;Write-Host ('Evidence: '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 if($p-and!$p.HasExited){if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}}
 if($client){$client.Dispose()};$receipt|ConvertTo-Json -Depth 15|Set-Content (Join-Path $out 'result.json')
}
