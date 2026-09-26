param([string]$Label='dayz-positive-warming',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if (!$env:LOCK_OWNER) {
 $shellName=if($PSVersionTable.PSEdition -eq 'Core'){'pwsh.exe'}else{'powershell.exe'}
 $env:LOCK_OWNER='DayZ positive loaded warming'
 try { & 'C:/Program Files/Git/bin/bash.exe' (Join-Path $PSScriptRoot 'with-game-lock.sh') ((Join-Path $PSHOME $shellName).Replace('\','/')) -NoProfile -File $PSCommandPath -Label $Label -GameDir $GameDir; if($LASTEXITCODE){throw "DayZ warming exited $LASTEXITCODE"} }
 finally {Remove-Item Env:LOCK_OWNER}
 return
}
if(Get-Process OpenPoseidon -ErrorAction SilentlyContinue){throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$profile=Join-Path $out 'user'; $mission=Join-Path $out 'positive-warming.eden'
New-Item -ItemType Directory -Path $profile,$mission -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
@'
version=11;
class Mission {randomSeed=1234;
 class Intel {year=1985;month=6;day=21;hour=10;minute=0;};
 class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=1;class Item0 {
  position[]={5704.58,0,2523.37};id=0;side="WEST";vehicle="SoldierWB";player="PLAYER COMMANDER";
  text="streamFixtureActor";leader=1;skill=1;
  init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""; this disableAI ""TARGET""";
 };};};};
};
class Intro {randomSeed=1;class Intel {};};
class OutroWin {randomSeed=2;class Intel {};};
class OutroLoose {randomSeed=3;class Intel {};};
'@ | Set-Content (Join-Path $mission 'mission.sqm')
$world=(Resolve-Path -LiteralPath (Join-Path $root '../../../packages/dayz-compat/world/chernarusplus/chernarusplus.wrp')).Path
$log=Join-Path $out 'engine.log'; $exe=Join-Path $GameDir 'OpenPoseidon.exe'
$environment=@{POSEIDON_USER_DIR=$profile;WGR_SIMULATION_RESIDENCY='1';WGR_SIMULATION_RESIDENCY_TEST='1';WGR_OBJECT_STREAM_TEST_BUDGET='1';WGR_OBJECT_STREAM_MAX_OBJECTS='20000';WGR_OBJECT_STREAM_GPU_BUDGET='0';WGR_GEO_POOL_GROWTH='2.0';WGR_GEO_POOL_PRESSURE_GROWTH='0';WGR_OBJECT_STREAM_PBO='0';WGR_OBJECT_STREAM_PBO_TEXTURES='0';WGR_NATIVE_DDS_PREPARE='0';WGR_NATIVE_DDS_BC3_ONLY='0';WGR_ADAPTIVE_TEXTURE_DETAIL='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_SKY_VOLUME_INCREMENTAL='1'}
$clearNames=@((Get-ChildItem Env: | Where-Object {$_.Name -like 'WGR_*' -or $_.Name -like 'POSEIDON_REFORGER_*'} | ForEach-Object {$_.Name}))+@('POSEIDON_USER_DIR','POSEIDON_TEST_RAIN')
$savedEnvironment=@{}; foreach($name in @($clearNames+@($environment.Keys) | Select-Object -Unique)){$savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name)}
$p=$null;$client=$null
try {
 foreach($name in $clearNames){Remove-Item ('Env:'+$name) -ErrorAction SilentlyContinue}
 foreach($name in $environment.Keys){[Environment]::SetEnvironmentVariable($name,$environment[$name])}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 @{head=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash $PSCommandPath).Hash;installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));hashes=@(Get-FileHash $exe,(Join-Path $GameDir 'wgpu_renderer.dll'));world=$world;pose=@(5704.58,2523.37,73.74,260.9,-3.8);environment=$environment;scope='Already loaded positive actor leases at 20000-object camera budget; no cold source/factory admission, complete-query readiness or performance acceptance.'} | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $out 'provenance.json')
 $p=Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',[string]$port,'--capture-metrics',('"'+(Join-Path $out 'capture.json')+'"'),'--test-world-freefly','5704.58','2523.37','73.74','260.9','-3.8','--test-world',('"'+$world+'"'),'--addon-root','"D:\SteamLibrary\steamapps\common\DayZ\Addons"','--test-world-hour','10','--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(120)
 while(!$client.Connected){try{$client.Connect('127.0.0.1',$port)}catch{if($p.HasExited -or [DateTime]::UtcNow -gt $deadline){throw};Start-Sleep -Milliseconds 250}}
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command){
  $writer.WriteLine(($command | ConvertTo-Json -Compress));do{$line=$reader.ReadLine();if($null -eq $line){throw 'Harness closed'};$reply=$line | ConvertFrom-Json}while($null -eq $reply.ok)
  ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl');$line | Add-Content (Join-Path $out 'harness.jsonl');if(!$reply.ok){throw $line};return $reply
 }
 function Sample($phase){
  if($p.HasExited){throw 'Owned game exited'}
  $simulation=Send @{cmd='stream_simulation_residency'};$camera=Send @{cmd='stream_residency'}
  foreach($field in @('sourceEnvelopeBytes','sourceEnvelopeScans','sourceEnvelopePublished','sourceEnvelopeMs')){if($camera.$field -ne 0){throw "Positive loaded stage performed source audit work: $field"}}
  if($simulation.preparedModels -ne 0 -or $simulation.activeModels -ne 0 -or $simulation.created -ne 0 -or $camera.required -ne 0){throw 'Positive loaded stage expanded cold model/instance admission or pins'}
  $sample=@{utc=[DateTime]::UtcNow.ToString('o');phase=$phase;simulation=$simulation;camera=$camera};$sample | ConvertTo-Json -Depth 6 -Compress | Add-Content (Join-Path $out 'samples.jsonl');return $sample
 }
 function Query($action){Send @{cmd='stream_simulation_query';actor='streamFixtureActor';action=$action;from=@(5704.58,73.74,2523.37);to=@(5720.58,73.74,2523.37);radius=0.01;purpose=1}}
 $deadline=[DateTime]::UtcNow.AddSeconds(240)
 do{Start-Sleep -Seconds 2;$loaded=Sample 'loaded'}while((!$loaded.simulation.metadataComplete -or $loaded.camera.pending -or !$loaded.camera.valid -or $loaded.simulation.positiveLeasedPlacements -eq 0 -or $loaded.simulation.positiveScannedRegions -eq 0) -and [DateTime]::UtcNow -lt $deadline)
 if(!$loaded.simulation.metadataComplete -or $loaded.simulation.status -ne 'CapacityExceeded' -or $loaded.camera.pending -or !$loaded.camera.valid -or $loaded.simulation.positiveLeasedPlacements -eq 0 -or $loaded.simulation.positiveScannedRegions -eq 0){throw 'Real DayZ did not complete positive loaded warming at unchanged camera coverage'}
 if($loaded.camera.resident -ne 19923 -or $loaded.camera.desired -ne 19923 -or $loaded.camera.budget -ne 20000){throw 'Dense DayZ baseline camera coverage changed'}
 $loadedQueryQueued=Query 'queue';$loadedQuery=Query 'status';if($loadedQuery.status -ne 'CapacityExceeded'){throw 'Partial DayZ query became ready'}
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'loaded.png')}
 $null=Send @{cmd='eval';code='triFreeFlyPose "9704.58 2523.37 73.74 260.9 -3.8"'}
 $deadline=[DateTime]::UtcNow.AddSeconds(180)
 do{Start-Sleep -Seconds 2;$away=Sample 'camera-away'}while(($away.camera.pending -or $away.simulation.positiveScannedRegions -eq 0) -and [DateTime]::UtcNow -lt $deadline)
 $awayCollision=Query 'collide'
 if($away.camera.pending -or $away.simulation.positiveLeasedPlacements -eq 0 -or $away.simulation.status -ne 'CapacityExceeded' -or $awayCollision.status -ne 'CapacityExceeded' -or @($awayCollision.contacts).Count -ne 0){throw 'Camera-away DayZ lost positive leases or changed exact query status'}
 $null=Send @{cmd='exec';code='streamFixtureActor setPos [12000,12000,0]'}
 $deadline=[DateTime]::UtcNow.AddSeconds(60)
 do{Start-Sleep -Seconds 1;$released=Sample 'actor-away'}while(($released.simulation.leasedPlacements -ne 0 -or $released.simulation.positiveScannedRegions -eq 0) -and [DateTime]::UtcNow -lt $deadline)
 if($released.simulation.leasedPlacements -ne 0 -or $released.simulation.status -ne 'CapacityExceeded'){throw 'DayZ actor replacement failed to release transient positive leases'}
 $null=Send @{cmd='exit'};if(!$p.WaitForExit(15000) -or $p.ExitCode){throw 'Game failed clean exit'}
 if(Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet){throw 'Renderer error'}
 @{passed=$true;loaded=$loaded;loadedQueryQueued=$loadedQueryQueued;loadedQuery=$loadedQuery;away=$away;awayCollision=$awayCollision;released=$released;performanceMeasured=$false} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
 Write-Output "DayZ positive warming evidence: $out"
}finally{
 if($client){$client.Dispose()};if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}
 foreach($name in $savedEnvironment.Keys){[Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name])}
}
