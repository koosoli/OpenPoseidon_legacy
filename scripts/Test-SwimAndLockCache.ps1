param([Parameter(Mandatory=$true)][ValidatePattern('^[a-fA-F0-9]{8,40}$')][string]$ExpectedCommit)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
$mod='C:/Users/mail/AppData/Local/OpenPoseidon/LocalMods/@OP_SwimLocalTest'
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
Require ([bool]$env:LOCK_OWNER) 'Game lock required'
Require (!(Get-Process OpenPoseidon -ErrorAction SilentlyContinue)) 'Another game is running'
$stamp=(Get-Content -LiteralPath (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
Require ($stamp -match [regex]::Escape($ExpectedCommit)) 'Wrong installed build'
$pair=@{};foreach($name in @('OpenPoseidon.exe','wgpu_renderer.dll')){$pair[$name]=(Get-FileHash -LiteralPath (Join-Path $game $name)).Hash}
$out=Join-Path $root ('build/local-swim-stress/'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Path $out -Force | Out-Null
$result=[ordered]@{passed=$false;installed=$stamp;pair=$pair;scriptSha256=(Get-FileHash $PSCommandPath).Hash;arms=@();scope='Installed optional swim player movement, model/animation readback, shore return, actual stress mission repeated terrain flushes and normal exit; no broad swim animation or Eastern ragdoll qualification.'}
$saved=@{};$keys=@('POSEIDON_USER_DIR','POSEIDON_SWIM','POSEIDON_SWIM_LOG','POSEIDON_AUTOMATIC_RAGDOLL','POSEIDON_SNOWLINE','POSEIDON_SNOW_TEST_DEPTH','WGR_GRASS','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_TEMPORAL','POSEIDON_REFORGER_WORLD','POSEIDON_REFORGER_OBJECTS','POSEIDON_REFORGER_STREAM','WGR_TERRAIN_PUDDLE_WETNESS','WGR_TERRAIN_PUDDLE_FIXTURE')
foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
$p=$null;$client=$null;$writer=$null
function Send($command){
 Require (!$p.HasExited) 'Owned game exited unexpectedly'
 $wire=$command|ConvertTo-Json -Depth 12 -Compress;$wire|Add-Content -LiteralPath (Join-Path $armOut 'harness.jsonl');$writer.WriteLine($wire)
 do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
 $line|Add-Content -LiteralPath (Join-Path $armOut 'harness.jsonl');Require $reply.ok $line;return $reply
}
function Eval([string]$code){$v=([string](Send @{cmd='eval';code=$code}).result).Trim();if($v.StartsWith('"')){return $v.Substring(1,$v.Length-2).Replace('""','"')};return ConvertFrom-Json -InputObject $v -NoEnumerate}
function Exec([string]$code){$null=Send @{cmd='exec';code=$code}}
function Capture([string]$label){
 $path=Join-Path $armOut ($label+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(15)
 while(!(Test-Path -LiteralPath $path)){Require ([DateTime]::UtcNow -lt $until) 'Screenshot deadline';Start-Sleep -Milliseconds 100}
 $arm.captures[$label]=$path
}
try {
 $env:POSEIDON_SNOWLINE='off';$env:POSEIDON_SNOW_TEST_DEPTH='0';$env:WGR_GRASS='0';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='.6';$env:WGR_TEMPORAL='0'
 foreach($fixture in @(
  @{name='swim';mission=(Join-Path $mod 'Missions/OpenPoseidon/swim-bay.Eden');extra=@('--mod',('"'+$mod+'"'))},
  @{name='stress';mission=(Join-Path $game 'Mods/@63units/63MAX.west_ls2');extra=@('--addon-root',('"'+(Join-Path $game 'Mods/@63units')+'"'))}
 )){
  $armOut=Join-Path $out $fixture.name;New-Item -ItemType Directory -Path $armOut -Force | Out-Null
  $profile=Join-Path $armOut 'user';New-Item -ItemType Directory -Path $profile -Force | Out-Null;$env:POSEIDON_USER_DIR=$profile
  [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
  if($fixture.name -eq 'swim'){$env:POSEIDON_SWIM='1';$env:POSEIDON_SWIM_LOG='1'}else{Remove-Item Env:POSEIDON_SWIM,Env:POSEIDON_SWIM_LOG -ErrorAction SilentlyContinue}
  $log=Join-Path $armOut 'engine.log';$arm=@{name=$fixture.name;passed=$false;log=$log;captures=@{}};$result.arms+=,$arm
  $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
  $args=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','1500','--harness',"$port",'--test-mission',('"'+$fixture.mission+'"'),'--test-world-hour','12','--log-file',('"'+$log+'"'))+$fixture.extra
  $arm.arguments=$args
  $p=Start-Process -FilePath (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $args -PassThru;$null=$p.Handle;$arm.pid=$p.Id
  $until=[DateTime]::UtcNow.AddSeconds(120)
  do{
   Require (!$p.HasExited) 'Startup exited';$client=[Net.Sockets.TcpClient]::new()
   try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null}
   if(!$client){Require ([DateTime]::UtcNow -lt $until) 'Startup deadline';Start-Sleep -Milliseconds 250}
  }while(!$client)
  $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
  while((Eval 'triSceneReady') -cne 'OK'){Require ([DateTime]::UtcNow -lt $until) 'Scene-ready deadline';Start-Sleep -Milliseconds 250}
  Start-Sleep -Seconds 8
  Exec 'player allowDamage false;0 setRain 0;0 setFog 0;0 setOvercast 0'
  $arm.player=Send @{cmd='diag_inspect';unit='player'}
  Require ($arm.player.cls -ceq 'SoldierWB') 'Not the expected US player'
  if($fixture.name -eq 'swim'){
   $arm.tread=Send @{cmd='diag_inspect';unit='player'}
   Require ($arm.tread.man.move -match '^Swim') ('Swimming did not activate: '+($arm.tread|ConvertTo-Json -Compress -Depth 5))
   Capture 'tread'
   $before=Eval 'getPosASL player';$null=Send @{cmd='key';sc=26;hold=$true};Start-Sleep -Seconds 6
   $arm.stroke=Send @{cmd='diag_inspect';unit='player'};$after=Eval 'getPosASL player';$null=Send @{cmd='key_up';sc=26}
   $arm.movement=@{before=$before;after=$after;distance=[Math]::Sqrt([Math]::Pow(($after[0]-$before[0]),2)+[Math]::Pow(($after[1]-$before[1]),2));input='Actual SDL W held six seconds'}
   Require ($arm.stroke.man.move -match '^Swim' -and $arm.movement.distance -gt 1) 'Actual swim input did not move the player'
   Capture 'stroke';Start-Sleep -Seconds 3;$arm.returnTread=Send @{cmd='diag_inspect';unit='player'}
   Require ($arm.returnTread.man.move -match '^Swim') 'Water release transition missing'
   Exec 'player setPos [9300,1900,0]';Start-Sleep -Seconds 5;$arm.land=Send @{cmd='diag_inspect';unit='player'}
   Require ($arm.land.man.move -notmatch '^Swim' -and $arm.land.pos[2] -gt 1) 'Land return failed'
   Capture 'land';Require ([bool](Select-String -LiteralPath $log -SimpleMatch 'Swim: level=' -Quiet)) 'Actual swimmer diagnostic absent'
  }else{
   $arm.groupsBefore=Send @{cmd='diag_groups'}
   $arm.unitCount=($arm.groupsBefore.groups|Measure-Object -Property units -Sum).Sum
   Require ($arm.unitCount -eq 708) ('Stress census differs: '+$arm.unitCount)
   $arm.near=Send @{cmd='diag_near';pos=@(10825,6955);r=40}
   $arm.easternShapes=@($arm.near.objects|Where-Object {$_.cls -in @('SoldierEB','OfficerE')}|Select-Object cls,shape)
   $state=Send @{cmd='dev_terrain_brush';action='state';x=10825.;z=6955.};$arm.before=$state;$arm.edits=@()
   foreach($i in 1..4){
    $edit=Send @{cmd='dev_terrain_brush';action='paint';x=(10825.+$i*8);z=6955.;radius=32.;delta=-.15}
    Require ($edit.heightRevision -gt $state.heightRevision -and $edit.baseline) 'Real terrain edit did not advance';$state=$edit;$arm.edits+=,$edit;Start-Sleep -Seconds 2
   }
   Exec 'cacheMover = [10825,6955,0] nearestObject "SoldierEB";cacheMover doMove [10890,7000,0]'
   Start-Sleep -Seconds 8;$arm.mover=Send @{cmd='diag_inspect';unit='cacheMover'}
   $arm.restored=Send @{cmd='dev_terrain_brush';action='restore';x=10825.;z=6955.};Start-Sleep -Seconds 8
   $arm.groupsAfter=Send @{cmd='diag_groups'};Capture 'stress-after-edits'
  }
  $null=Send @{cmd='exit'};Require ($p.WaitForExit(20000) -and $p.ExitCode -eq 0) 'Normal game exit failed'
  Require ([bool](Select-String -LiteralPath $log -SimpleMatch 'Shutdown complete' -Quiet)) 'Normal shutdown receipt absent'
  Require (!(Select-String -LiteralPath $log -Pattern 'Cannot unlock field that was not locked|UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed' -Quiet)) 'Cache or GPU failure in log'
  $arm.exitCode=$p.ExitCode;$arm.passed=$true;$p=$null;$client.Dispose();$client=$null;$writer=$null
  Write-Host ($fixture.name+' runtime PASS')
 }
 foreach($name in $pair.Keys){Require ((Get-FileHash -LiteralPath (Join-Path $game $name)).Hash -ceq $pair[$name]) 'Installed pair changed'}
 $result.passed=$true
}catch{$result.error=$_.Exception.Message;throw}
finally{
 if($p -and !$p.HasExited -and $writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(20000)}catch{$result.cleanupError=$_.Exception.Message}}
 if($p -and !$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000);$result.cleanupForced=$true}
 if($client){$client.Dispose()}
 foreach($key in $saved.Keys){if($null -eq $saved[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
 $receipt=Join-Path $out 'runtime.json';$result|ConvertTo-Json -Depth 18|Set-Content -LiteralPath $receipt -Encoding utf8;Write-Host ('Receipt: '+$receipt)
}
