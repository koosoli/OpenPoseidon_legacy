<# Installed-game acceptance: actual freefly crosses rolling-window boundaries,
   returns to the start, validates the upload receipts, captures rendered frames.
   Wrap in scripts/with-game-lock.sh. Stops only the process this script starts. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,[switch]$Offline)
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Game lock required'}
$repo=Split-Path -Parent $PSScriptRoot
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Existing game preserved'}
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if(!$stamp.Contains($ExpectedCommit)){throw 'Installed source differs'}
$out=Join-Path $repo ('build/earth-streaming/'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Force -Path $out
$cache=Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'
$world=& "$PSScriptRoot/Launch-WholeWorld.ps1" -PrepareOnly -CacheDirectory $cache
$mission=Join-Path $cache 'fixture/earth-preview.abel'
$settings=@{POSEIDON_WHOLE_WORLD='1';POSEIDON_EARTH_LATITUDE='46.6';POSEIDON_EARTH_LONGITUDE='8.1';POSEIDON_EARTH_CACHE=$cache;WGR_GRASS='0';WGR_TERRAIN_RESOURCE_REUSE='1';WGR_WET_SOIL_DIAGNOSTIC='1';POSEIDON_USER_DIR=(Join-Path $out 'user')}
if($Offline){$settings.HTTPS_PROXY='http://127.0.0.1:9';$settings.HTTP_PROXY='http://127.0.0.1:9';$settings.ALL_PROXY='http://127.0.0.1:9';$settings.NO_PROXY=''}
$saved=@{};foreach($key in $settings.Keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
$null=New-Item -ItemType Directory -Force -Path $env:POSEIDON_USER_DIR
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$receipt=@{installed=$stamp;offline=[bool]$Offline;views=@();passed=$false};$p=$null;$client=$null;$writer=$null
function Require($ok,$message){if(!$ok){throw $message}}
function Send($cmd){$writer.WriteLine(($cmd|ConvertTo-Json -Depth 10 -Compress));do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require $reply.ok $line;return $reply}
try{
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $log=Join-Path $out 'engine.log'
 $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','20000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$world+'"'),'--test-world-freefly','6400','6400','4500','30','-20','--test-world-hour','12','--log-file',('"'+$log+'"'))
 $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $argv -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError (Join-Path $out 'stderr.log') -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(150)
 do{Require (!$p.HasExited) 'Startup exited';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$deadline) 'Startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 while(([string](Send @{cmd='eval';code='triSceneReady'}).result).Trim('"')-cne'OK'){Require ([DateTime]::UtcNow-lt$deadline) 'Scene-ready deadline';Start-Sleep -Milliseconds 250}
 Require (!(Select-String -Path $log -Pattern 'Cannot load --test-world|Critical error' -Quiet)) 'Test world failed to load'
 $null=Send @{cmd='exec';code='player allowDamage false;0 setRain 0;0 setOvercast .15;0 setFog .02;setDate [1985,6,21,12,0]'}
 foreach($x in @(6400,9600,16000,32000,6400)){
  $before=@(Select-String -Path $log -Pattern 'Earth terrain patch ').Count
  $null=Send @{cmd='exec';code=('triFreeFlyPose "'+$x+' 6400 4500 30 -20"')}
  $origin=$x-12800
  $pattern='origin \['+$origin+',-6400\]'
  $deadline=[DateTime]::UtcNow.AddSeconds(90)
  do{
   Require (!$p.HasExited) 'Game exited during streaming'
   $rows=@(Select-String -Path $log -Pattern 'Earth terrain patch ')
   $last=if($rows.Count){$rows[-1].Line}else{''}
   if($last -match $pattern -and ($x-eq6400 -and $receipt.views.Count-eq0 -or $rows.Count-gt$before)){break}
   Require ([DateTime]::UtcNow-lt$deadline) ('Patch deadline at x='+$x+'; last='+$last)
   Start-Sleep -Milliseconds 300
  }while($true)
  Start-Sleep -Milliseconds 500
  $camera=Send @{cmd='dev_wet_soil_diagnostic';action='state'}
  Require ([Math]::Abs($camera.camera.position[0]-$x)-lt.1) 'Actual freefly position differs'
  $path=Join-Path $out ('earth-'+$receipt.views.Count+'.png');$null=Send @{cmd='screenshot';path=$path}
  $receipt.views+=@{x=$x;upload=$last;screenshot=$path;performance=(Send @{cmd='frame_performance'})}
 }
 $receipt.tiles=@(Get-ChildItem (Join-Path $cache 'terrarium-z10') -Filter '*.png').Count
 Require ($receipt.tiles-le256) 'Disk cache exceeds its bound'
 if($Offline){
  Require (!(Select-String -Path $log -Pattern 'downloads [1-9]' -Quiet)) 'Offline traversal unexpectedly downloaded data'
  $rows=@(Select-String -Path $log -Pattern 'Earth terrain patch ').Count
  $null=Send @{cmd='exec';code='triFreeFlyPose "100000 6400 4500 30 -20"'}
  $deadline=[DateTime]::UtcNow.AddSeconds(20)
  do{
   $failed=Select-String -Path (Join-Path $out 'stderr.log') -Pattern 'retaining last completed patch' -Quiet
   if($failed){break}
   Require ([DateTime]::UtcNow-lt$deadline) 'Offline missing-tile failure not observed'
   Start-Sleep -Milliseconds 250
  }while($true)
  Require (@(Select-String -Path $log -Pattern 'Earth terrain patch ').Count-eq$rows) 'Incomplete/offline window was published'
  $receipt.offlineFailureObserved=$true
  $receipt.afterFailure=Send @{cmd='frame_performance'}
 }
 Require (!(Select-String -Path @($log,(Join-Path $out 'stderr.log')) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error' -Quiet)) 'Runtime error'
 Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Installed build changed'
 $receipt.passed=$true
 Write-Host ('Earth streaming acceptance PASS: '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 if($p-and!$p.HasExited){if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}}
 if($client){$client.Dispose()}
 foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
 $receipt|ConvertTo-Json -Depth 15|Set-Content (Join-Path $out 'result.json')
}
