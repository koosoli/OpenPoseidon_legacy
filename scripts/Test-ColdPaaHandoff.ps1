param([switch]$Disabled,[switch]$RequireAlphaPeek,[switch]$AlphaTagged,[string]$Label='cold-paa-handoff',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',[string]$Python='C:\Program Files\Python311\python.exe')
$ErrorActionPreference='Stop'
if($Disabled -and $RequireAlphaPeek){throw 'Alpha peek requires the cold handoff option'}
if($Label -notmatch '^[a-zA-Z0-9_-]{1,64}$'){throw 'Invalid bounded label'}
if(!$env:LOCK_OWNER){
 $shell=Join-Path $PSHOME $(if($PSVersionTable.PSEdition -eq 'Core'){'pwsh.exe'}else{'powershell.exe'})
 $args=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$shell.Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python)
 if($Disabled){$args+='-Disabled'}
 if($RequireAlphaPeek){$args+='-RequireAlphaPeek'}
 if($AlphaTagged){$args+='-AlphaTagged'}
 $env:LOCK_OWNER='cold PAA owned handoff fixture'
 try{& 'C:\Program Files\Git\bin\bash.exe' @args;if($LASTEXITCODE){throw "Fixture child failed $LASTEXITCODE"}}finally{Remove-Item Env:LOCK_OWNER}
 return
}
if(Get-Process OpenPoseidon -ErrorAction SilentlyContinue){throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('op-cp-'+[guid]::NewGuid().ToString('N').Substring(0,8))
$profile=Join-Path $out 'user';$mission=Join-Path $out 'cold-paa.eden'
New-Item -ItemType Directory $out,$profile,$mission | Out-Null
$generator=Join-Path $PSScriptRoot 'streaming/build_cold_paa_handoff_fixture.py'
$producerArgs=@($generator,$fixture)
$tagged=[bool]($RequireAlphaPeek -or $AlphaTagged)
if($tagged){$producerArgs+='--alpha-tagged'}
$producerLines=@(& $Python @producerArgs)
if($LASTEXITCODE -or $producerLines.Count -ne 1 -or $producerLines[0].Length -gt 8192){throw 'Original bounded producer failed'}
$producer=$producerLines[0]|ConvertFrom-Json
if($producer.schema -ne 1 -or !$producer.originalGenerated -or $producer.modelCount -ne 2 -or @($producer.placements).Count -ne 2 -or $producer.chainBytes -ne 5456 -or $producer.selectedMipCount -ne 4 -or $producer.selectedChainBytes -ne 5440 -or $producer.memberBytes -gt 16777216){throw 'Producer contract mismatch'}
foreach($field in @('alphaTagged','alphaFlag','alphaTagBytes')){if($producer.PSObject.Properties.Name -notcontains $field -or $null -eq $producer.$field){throw "Missing producer alpha provenance $field"}}
if([bool]$producer.alphaTagged -ne $tagged -or $producer.alphaFlag -ne $(if($tagged){1}else{0}) -or $producer.alphaTagBytes -ne $(if($tagged){16}else{0})){throw 'Actual authored alpha tag provenance mismatch'}
foreach($file in $producer.files){if(!(Test-Path -LiteralPath $file.path) -or (Get-Item -LiteralPath $file.path).Length -ne $file.bytes -or (Get-FileHash -LiteralPath $file.path).Hash -ne $file.sha256){throw 'Producer file hash/size mismatch'}}
$producerLines[0]|Set-Content (Join-Path $out 'producer.json')
@'
version=11;
class Mission {randomSeed=1234;class Intel {year=1985;month=6;day=21;hour=12;minute=0;};
class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=1;class Item0 {
position[]={100,0,100};id=0;side="WEST";vehicle="SoldierWB";player="PLAYER COMMANDER";leader=1;skill=1;
init="this setBehaviour ""CARELESS"";this setCombatMode ""BLUE"";this disableAI ""MOVE""";
};};};};};
class Intro {randomSeed=1;class Intel {};};class OutroWin {randomSeed=2;class Intel {};};class OutroLoose {randomSeed=3;class Intel {};};
'@|Set-Content (Join-Path $mission 'mission.sqm')
[IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$environment=@{
 POSEIDON_USER_DIR=$profile;POSEIDON_MODEL_DDC='0';WGR_SIMULATION_RESIDENCY='0';WGR_SIMULATION_RESIDENCY_TEST='0';WGR_SIMULATION_POSITIVE_COLD='0'
 WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF=$(if($Disabled){'0'}else{'1'});WGR_OBJECT_STREAM_ASYNC='1';WGR_OBJECT_STREAM_ASYNC_ADAPT='1';WGR_OBJECT_STREAM_ASYNC_WORKERS='2'
 WGR_OBJECT_STREAM_WARM_TEXTURES='0';WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS='0';WGR_OBJECT_STREAM_PBO_TEXTURES='0';WGR_OBJECT_STREAM_PBO='0'
 WGR_OBJECT_STREAM_WINDOW_GROWTH='0';WGR_OBJECT_STREAM_RADIUS_CELLS='8';WGR_OBJECT_STREAM_GPU_BUDGET='0';WGR_OBJECT_STREAM_MAX_OBJECTS='20000';WGR_OBJECT_STREAM_TEST_BUDGET='1'
 WGR_NATIVE_DDS_PREPARE='0';WGR_NATIVE_DDS_BC3_ONLY='0';WGR_ADAPTIVE_TEXTURE_DETAIL='0';WGR_WATER_BACKEND='0';WGR_GEOMETRY_PAGE_FIXTURE='0'
 WGR_GPU_MODEL_PARK_REFILL='0';WGR_OBJECT_STREAM_REGISTRATION_QUOTA='0';WGR_CULL_SECTION_REUSE='0';WGR_CULL_LOD_REUSE='0';WGR_PAA_LZO_REPLAY_CACHE='0'
}
$old=@{};Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {$old[$_.Name]=$_.Value}
$p=$null;$client=$null;$normal=$false;$forced=$false;$result=$null;$failure=$null
$exe=Join-Path $GameDir 'OpenPoseidon.exe';$dll=Join-Path $GameDir 'wgpu_renderer.dll';$log=Join-Path $out 'engine.log'
$hashes=@(Get-FileHash -LiteralPath $exe,$dll)
try{
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $environment.Keys){Set-Item ('Env:'+$key) $environment[$key]}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',[string]$port,'--test-world-freefly','100','100','80','45','-25','--test-world',('"'+(Join-Path $fixture 'cold-paa.wrp')+'"'),'--addon-root',('"'+(Join-Path $fixture 'addons')+'"'),'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 @{head=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash $PSCommandPath).Hash;generatorSha256=(Get-FileHash $generator).Hash;installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));hashes=$hashes;environment=$environment;argv=$argv;fixture=$producer;scope='Controlled original two-model primary texture first upload; no pixel/all-pass/FPS acceptance'}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'provenance.json')
 $p=Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $argv
 $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$until=[DateTime]::UtcNow.AddSeconds(90)
 while(!$client.Connected){try{$client.Connect('127.0.0.1',$port)}catch{if($p.HasExited -or [DateTime]::UtcNow -gt $until){throw};Start-Sleep -Milliseconds 250}}
 $stream=$client.GetStream();$stream.ReadTimeout=15000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command){$writer.WriteLine(($command|ConvertTo-Json -Compress));do{$line=$reader.ReadLine();if($null -eq $line){throw 'Harness closed'};$r=$line|ConvertFrom-Json}while($null -eq $r.ok);($command|ConvertTo-Json -Compress)|Add-Content (Join-Path $out 'harness.jsonl');$line|Add-Content (Join-Path $out 'harness.jsonl');if(!$r.ok){throw $line};return $r}
 function Require($r,$names){foreach($name in $names){if($null -eq $r -or $null -eq $r.PSObject.Properties[$name] -or $null -eq $r.$name){throw "Missing field $name"}}}
 function Sample {
  $ids=Send @{cmd='stream_identity_probe';ids=@(1001,1002)};$camera=Send @{cmd='stream_residency'}
  Require $ids @('objects');if(@($ids.objects).Count -ne 2){throw 'Identity count mismatch'}
  foreach($id in $ids.objects){Require $id @('id','present');if($id.present){Require $id @('visualResident','normalVertexBuffers')}}
  if($ids.objects[0].id -ne 1001 -or $ids.objects[1].id -ne 1002){throw 'Identity order mismatch'}
  Require $camera @('valid','pending','resident','desired','required','budget')
  return @{identity=$ids;camera=$camera}
 }
 $simulation=Send @{cmd='stream_simulation_residency'};Require $simulation @('status','ownerPumps')
 if($simulation.status -ne 'Disabled' -or $simulation.ownerPumps -ne 0){throw 'Simulation unexpectedly enabled'}
 $until=[DateTime]::UtcNow.AddSeconds(30)
 do{Start-Sleep -Milliseconds 250;$baseline=Sample}while((!$baseline.camera.valid -or $baseline.camera.pending) -and [DateTime]::UtcNow -lt $until)
 if(@($baseline.identity.objects|Where-Object {$_.present -or $_.visualResident}).Count -or !$baseline.camera.valid -or $baseline.camera.required -ne 0 -or $baseline.camera.pending -or $baseline.camera.desired -ne 0 -or $baseline.camera.resident -ne 0){throw 'Model loaded before camera demand'}
 $archiveBefore=Send @{cmd='archive_source_bindings'};Require $archiveBefore @('enabled','liveBindings','knownCppBytes','wrappedReads','initRetains')
 $archiveBefore|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'archive-before.json')
 $baseline|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'baseline.json')
 $pose=Send @{cmd='eval';code='triFreeFlyPose "1188 1200 18 90 -8"'};Require $pose @('result');if($pose.result.Trim('"') -ne 'OK'){throw 'Pose rejected'}
 $until=[DateTime]::UtcNow.AddSeconds(60)
 do{Start-Sleep -Milliseconds 250;if($p.HasExited){throw 'Game exited before coverage'};$after=Sample}while((@($after.identity.objects|Where-Object {$_.present -and $_.visualResident -and $_.normalVertexBuffers -gt 0}).Count -ne 2 -or $after.camera.pending) -and [DateTime]::UtcNow -lt $until)
 $after|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'coverage.json')
 Require $baseline.camera @('coldAlphaPeeks');Require $after.camera @('coldAlphaPeeks')
 if($Disabled -and ($baseline.camera.coldAlphaPeeks -ne 0 -or $after.camera.coldAlphaPeeks -ne 0)){throw 'OFF performed a cold alpha peek'}
 if($RequireAlphaPeek -and $after.camera.coldAlphaPeeks -le $baseline.camera.coldAlphaPeeks){throw 'NotExercised: no actual operation-local cold alpha classification'}
 if(@($after.identity.objects|Where-Object {$_.present -and $_.visualResident -and $_.normalVertexBuffers -gt 0}).Count -ne 2 -or !$after.camera.valid -or $after.camera.pending -or $after.camera.resident -ne 2 -or $after.camera.desired -ne 2 -or $after.camera.required -ne 0 -or $after.camera.budget -ne 20000){throw 'Original two-model coverage not established'}
 Start-Sleep -Seconds 3
 $archiveAfter=Send @{cmd='archive_source_bindings'};Require $archiveAfter @('enabled','liveBindings','knownCppBytes','wrappedReads','initRetains')
 $archiveAfter|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'archive-after.json')
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'fixture.png')}
 $null=Send @{cmd='exit'}
 if(!$p.WaitForExit(15000) -or $p.ExitCode -ne 0){throw 'Game did not exit normally'};$normal=$true
 $text=Get-Content -LiteralPath $log -Raw
 if($text -match 'Out of memory|Validation Error|panicked at|DeviceLost|UNHANDLED'){throw 'Runtime error log'}
 $rows=@($text -split "`r?`n"|Where-Object {$_ -match 'Cold PAA handoff upload:'})
 $name=[regex]::Escape($producer.sourceName)
 $useful=@($rows|Where-Object {$_ -match ('source='+$name+' bytes=5440 sourceValidated=true blockUploadSucceeded=true operationLocal=true')})
 if($Disabled){if($rows.Count){throw 'OFF emitted a cold handoff upload'}}elseif(!$useful.Count){throw 'NotExercised: no actual source-validated operation-local prepared upload'}
 $afterHashes=@(Get-FileHash -LiteralPath $exe,$dll)
 for($i=0;$i -lt 2;$i++){if($hashes[$i].Hash -ne $afterHashes[$i].Hash){throw 'Installed artifact changed during fixture'}}
 $result=@{passed=$true;disabled=[bool]$Disabled;requireAlphaPeek=[bool]$RequireAlphaPeek;alphaTagged=[bool]$producer.alphaTagged;normalShutdown=$true;forcedTermination=$false;exitCode=$p.ExitCode;baseline=$baseline;after=$after;archiveBefore=$archiveBefore;archiveAfter=$archiveAfter;usefulUploadRows=$useful;allColdUploadRows=$rows;scope='Actual primary archived chain upload and fixed two-model camera coverage; no pixel/all-pass/performance acceptance'}
}catch{$failure=$_}
finally{
 if($p -and !$p.HasExited){try{if($client -and $client.Connected){$null=Send @{cmd='exit'};$normal=$p.WaitForExit(15000) -and $p.ExitCode -eq 0}}catch{};if(!$p.HasExited){Stop-Process -Id $p.Id -Force;$forced=$true;$p.WaitForExit(5000)|Out-Null}}
 if($client){$client.Dispose()}
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $old.Keys){Set-Item ('Env:'+$key) $old[$key]}
 if($failure){@{passed=$false;reason=$failure.Exception.Message;normalShutdown=$normal;forcedTermination=$forced;pid=$(if($p){$p.Id}else{$null})}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'failure.json')}
}
if($failure){throw $failure}
$result|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'result.json')
Write-Host $out
