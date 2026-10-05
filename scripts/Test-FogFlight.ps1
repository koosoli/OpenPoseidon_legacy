<# Installed original Kolgujev, actual freefly camera movement, isolated profile. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,[string]$Label='fog-flight',[string]$Patch='',[switch]$Compact,[switch]$Wide,
 [switch]$EveronReport,[switch]$EveronRoadReport,[switch]$EveronHighReport,[switch]$EveronCloudReport,[switch]$LegacyFog,[switch]$Valley,[switch]$Temporal,[switch]$TwoLayers,[switch]$Grass,[switch]$TopDown,[double]$FogAmount=-1)
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Game lock required'}
$root=Split-Path $PSScriptRoot -Parent
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Existing game preserved'}
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if(!$stamp.Contains($ExpectedCommit)){throw 'Installed source differs'}
$out=Join-Path $root ('build/fog-flight/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Path $out -Force
$mission=Join-Path $out $(if($EveronReport -or $EveronRoadReport -or $EveronHighReport -or $EveronCloudReport){'flight.eden'}else{'flight.cain'});$null=New-Item -ItemType Directory -Path $mission
Copy-Item -LiteralPath (Join-Path $root 'tests/perf/missions/perf_field.eden/mission.sqm') -Destination $mission
$receipt=@{installed=$stamp;views=@();passed=$false};$p=$null;$client=$null;$writer=$null
function Require($ok,$message){if(!$ok){throw $message}}
function Send($cmd){$writer.WriteLine(($cmd|ConvertTo-Json -Depth 10 -Compress));do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require $reply.ok $line;$line|Add-Content (Join-Path $out 'harness.jsonl');return $reply}
function Exec($code){$null=Send @{cmd='exec';code=$code}}
try{
 $env:POSEIDON_USER_DIR=Join-Path $out 'user';$null=New-Item -ItemType Directory -Path $env:POSEIDON_USER_DIR
 [IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
 $env:WGR_LAYERED_FOG='1';$env:WGR_LAYERED_FOG_TRACE='1';$env:WGR_WEATHER_COVER_TRACE='1';$env:WGR_GRASS='0';$env:WGR_TEMPORAL='0';$env:WGR_WET_SOIL_DIAGNOSTIC='1'
 if($LegacyFog){$env:WGR_LAYERED_FOG='0'}
 if($Temporal){$env:WGR_TEMPORAL='1'}
 if($Grass){$env:WGR_GRASS='1'}
 if($TwoLayers){$env:WGR_FOG_LAYER1='70,100,5,0.003'}
 if($Patch){$env:WGR_FOG_PATCH=$Patch}
 if($Compact){$env:WGR_FOG_LAYER0='1,5,0.35,0.018';$env:WGR_FOG_LAYER1='35,50,3,0'}
 if($Wide){$env:WGR_FOG_LAYER0='0,750,5,0.01';$env:WGR_FOG_LAYER1='800,900,5,0';$env:WGR_FOG_TERRAIN='0,0,0,150';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='.6';$env:WGR_TONEMAP='1'}
 if($Valley){$env:WGR_FOG_LAYER0='-100,100,8,0.008';$env:WGR_FOG_LAYER1='200,250,5,0';$env:WGR_FOG_TERRAIN='0,0,1,150'}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $log=Join-Path $out 'engine.log';$stdout=Join-Path $out 'stdout.log';$stderr=Join-Path $out 'stderr.log'
 $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','8000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world-hour','12','--log-file',('"'+$log+'"'))
 $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $args -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(180)
 do{Require (!$p.HasExited) 'Startup exited';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$deadline) 'Startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 while(([string](Send @{cmd='eval';code='triSceneReady'}).result).Trim('"')-cne'OK'){Require ([DateTime]::UtcNow-lt$deadline) 'Scene-ready deadline';Start-Sleep -Milliseconds 300}
 Exec 'player allowDamage false;0 setRain 0;0 setOvercast .15;0 setFog .6;setDate [1985,6,21,12,0];setAccTime 1'
 if($EveronReport -or $EveronRoadReport){Exec '0 setFog .78'}
 if($EveronCloudReport){Exec '0 setOvercast 1;0 setFog 0'}
 if($Valley){Exec '0 setFog .15'}
 if($Wide){Exec '0 setFog .15'}
 if($FogAmount-ge0){Exec ('0 setFog '+$FogAmount.ToString('R',[Globalization.CultureInfo]::InvariantCulture))}
 Start-Sleep -Seconds 3;Exec 'setAccTime 0'
 $sites=@(@{name='coast';x=5448.73;z=7011.85;y=590.95;az=111.1;el=-24.1},@{name='ocean';x=7400;z=8500;y=900;az=120;el=-22})
 if($EveronReport -or $EveronRoadReport){$sites=@(@{name='everon-report';x=8131.43;z=4086.37;y=191.04;az=229.1;el=-11.3})}
 if($EveronRoadReport){$sites=@(@{name='everon-road';x=9182.32;z=2053.96;y=77.15;az=316.6;el=-11.9})}
 if($EveronHighReport){$sites=@(@{name='everon-high';x=4526.06;z=3977.46;y=1197.58;az=198.3;el=-28.1})}
 if($EveronCloudReport){$sites=@(@{name='everon-cloud';x=6759.20;z=3483.95;y=1764.24;az=330.9;el=-47.9})}
 if($TopDown){foreach($site in $sites){$site.y=250;$site.el=-70}}
 foreach($site in $sites){
  foreach($step in 0..12){
   $x=$site.x+$step*$(if($EveronReport -or $EveronRoadReport){-4}else{8});$z=$site.z+$step*5
   Exec ('triFreeFlyPose "'+$x+' '+$z+' '+$site.y+' '+$site.az+' '+$site.el+'"')
   Start-Sleep -Milliseconds $(if($step-eq0){3000}else{700})
   $state=Send @{cmd='weather_visibility'};$camera=Send @{cmd='dev_wet_soil_diagnostic';action='state'}
   Require ([Math]::Abs($camera.camera.position[0]-$x)-lt.02 -and [Math]::Abs($camera.camera.position[1]-$site.y)-lt.02 -and [Math]::Abs($camera.camera.position[2]-$z)-lt.02) 'Actual freefly camera differs'
   $path=Join-Path $out ($site.name+'-'+$step.ToString('00')+'.png');$null=Send @{cmd='screenshot';path=$path}
   $receipt.views+=@{name=$site.name;step=$step;requested=@($x,$z,$site.y,$site.az,$site.el);camera=$camera;weather=$state;image=$path}
  }
 }
 $source=Send @{cmd='dev_cave_editor';action='state';x=$sites[0].x;y=$sites[0].y;z=$sites[0].z};$worldPattern=if($EveronReport -or $EveronRoadReport -or $EveronHighReport -or $EveronCloudReport){'(^|/)eden\.wrp$'}else{'(^|/)cain\.wrp$'};Require ($source.worldName.Replace('\','/').ToLowerInvariant()-match$worldPattern) 'Not requested original world';$receipt.native=$source
 $receipt.timings=@();foreach($sample in 1..12){Start-Sleep -Milliseconds 250;$receipt.timings+=Send @{cmd='frame_performance'}}
 $receipt.performance=Send @{cmd='frame_performance'}
 $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000)-and$p.ExitCode-eq0) 'Normal exit failed'
 Require (!(Select-String -Path @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed' -Quiet)) 'Runtime error'
 Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Installed pair changed'
 $receipt.passed=$true;Write-Host ('Evidence: '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 if($p-and!$p.HasExited){if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}}
 if($client){$client.Dispose()};$receipt|ConvertTo-Json -Depth 15|Set-Content (Join-Path $out 'result.json')
}
