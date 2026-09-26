param([switch]$Lifecycle,[switch]$SaveRoundTrip,[switch]$FreshReload,[switch]$PBOTextures,[switch]$RequirePreparedAlphaFacts,[switch]$RequireCapacitySkip,[string]$Label='dayz-cold-positive',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault')
$ErrorActionPreference='Stop'
if($FreshReload){$SaveRoundTrip=$true}
if($SaveRoundTrip){$Lifecycle=$true}
if($RequireCapacitySkip){$RequirePreparedAlphaFacts=$true}
if($RequirePreparedAlphaFacts){$PBOTextures=$true}
if (!$env:LOCK_OWNER) {
 $shellName=if($PSVersionTable.PSEdition -eq 'Core'){'pwsh.exe'}else{'powershell.exe'}
 $env:LOCK_OWNER='DayZ source-bound cold actor admission'
 try { $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),((Join-Path $PSHOME $shellName).Replace('\','/')),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir);if($Lifecycle){$argv+='-Lifecycle'};if($SaveRoundTrip){$argv+='-SaveRoundTrip'};if($FreshReload){$argv+='-FreshReload'};if($PBOTextures){$argv+='-PBOTextures'};if($RequirePreparedAlphaFacts){$argv+='-RequirePreparedAlphaFacts'};if($RequireCapacitySkip){$argv+='-RequireCapacitySkip'}; & 'C:/Program Files/Git/bin/bash.exe' @argv; if($LASTEXITCODE){throw "DayZ cold test exited $LASTEXITCODE"} }
 finally {Remove-Item Env:LOCK_OWNER}
 return
}
if(Get-Process OpenPoseidon -ErrorAction SilentlyContinue){throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$profile=Join-Path $out 'user'; $mission=Join-Path $out 'cold-positive.eden'
New-Item -ItemType Directory -Path $profile,$mission -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
@'
version=11;
class Mission {randomSeed=1234;
 class Intel {year=1985;month=6;day=21;hour=10;minute=0;};
 class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=2;
  class Item0 {position[]={10000,0,10000};id=0;side="WEST";vehicle="SoldierWB";player="PLAYER COMMANDER";leader=1;skill=1;
   init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""; this disableAI ""TARGET""";};
  class Item1 {position[]={10002,0,10000};id=1;side="WEST";vehicle="SoldierWB";text="streamColdActor";skill=1;
   init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""; this disableAI ""TARGET""";};
 };};};
};
class Intro {randomSeed=1;class Intel {};};
class OutroWin {randomSeed=2;class Intel {};};
class OutroLoose {randomSeed=3;class Intel {};};
'@ | Set-Content (Join-Path $mission 'mission.sqm')
$world=(Resolve-Path -LiteralPath (Join-Path $root '../../../packages/dayz-compat/world/chernarusplus/chernarusplus.wrp')).Path
$log=Join-Path $out 'engine.log'; $exe=Join-Path $GameDir 'OpenPoseidon.exe'
$environment=@{POSEIDON_USER_DIR=$profile;WGR_SIMULATION_RESIDENCY='1';WGR_SIMULATION_RESIDENCY_TEST='1';WGR_SIMULATION_POSITIVE_COLD='1';WGR_OBJECT_STREAM_TEST_BUDGET='1';WGR_OBJECT_STREAM_MAX_OBJECTS='20000';WGR_OBJECT_STREAM_GPU_BUDGET='0';WGR_GEO_POOL_GROWTH='2.0';WGR_GEO_POOL_PRESSURE_GROWTH='0';WGR_OBJECT_STREAM_PBO='1';POSEIDON_MODEL_DDC='0';WGR_OBJECT_STREAM_PBO_TEXTURES='0';WGR_NATIVE_DDS_PREPARE='0';WGR_NATIVE_DDS_BC3_ONLY='0';WGR_ADAPTIVE_TEXTURE_DETAIL='0';WGR_LOD_GOVERNOR_RANGE='1';WGR_SKY_VOLUME_INCREMENTAL='1'}
if($PBOTextures){$environment.WGR_OBJECT_STREAM_PBO_TEXTURES='1'}
$clearNames=@((Get-ChildItem Env: | Where-Object {$_.Name -like 'WGR_*' -or $_.Name -like 'POSEIDON_REFORGER_*'} | ForEach-Object {$_.Name}))+@('POSEIDON_USER_DIR','POSEIDON_TEST_RAIN')
$savedEnvironment=@{}; foreach($name in @($clearNames+@($environment.Keys) | Select-Object -Unique)){$savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name)}
$p=$null;$client=$null;$reloadLog=$null
try {
 foreach($name in $clearNames){Remove-Item ('Env:'+$name) -ErrorAction SilentlyContinue}
 foreach($name in $environment.Keys){[Environment]::SetEnvironmentVariable($name,$environment[$name])}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 @{head=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash $PSCommandPath).Hash;installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));hashes=@(Get-FileHash $exe,(Join-Path $GameDir 'wgpu_renderer.dll'));world=$world;environment=$environment;scope='Actual inventory83912800 cold actor admission, unchanged20000 camera budget; no object getter, source diagnostic, asset whitelist or full-query readiness.'} | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $out 'provenance.json')
 $p=Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList @('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',[string]$port,'--capture-metrics',('"'+(Join-Path $out 'capture.json')+'"'),'--test-world-freefly','10000','10000','450','45','-10','--test-world',('"'+$world+'"'),'--addon-root','"D:\SteamLibrary\steamapps\common\DayZ\Addons"','--test-world-hour','10','--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(120)
 while(!$client.Connected){try{$client.Connect('127.0.0.1',$port)}catch{if($p.HasExited -or [DateTime]::UtcNow -gt $deadline){throw};Start-Sleep -Milliseconds 250}}
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command){
  $writer.WriteLine(($command | ConvertTo-Json -Compress));do{$line=$reader.ReadLine();if($null -eq $line){throw 'Harness closed'};$reply=$line | ConvertFrom-Json}while($null -eq $reply.ok)
  ($command | ConvertTo-Json -Compress) | Add-Content (Join-Path $out 'harness.jsonl');$line | Add-Content (Join-Path $out 'harness.jsonl');if(!$reply.ok){throw $line};return $reply
 }
 function Sample($phase){
  if($p.HasExited){throw 'Owned game exited'}
  $sample=@{utc=[DateTime]::UtcNow.ToString('o');phase=$phase;simulation=(Send @{cmd='stream_simulation_residency'});camera=(Send @{cmd='stream_residency'});identity=(Send @{cmd='stream_identity_probe';ids=@(83912800)})}
  $sample | ConvertTo-Json -Depth 6 -Compress | Add-Content (Join-Path $out 'samples.jsonl');return $sample
 }
 $deadline=[DateTime]::UtcNow.AddSeconds(240)
 do{Start-Sleep -Seconds 2;$before=Sample 'before'}while((!$before.simulation.metadataComplete -or $before.camera.pending -or !$before.camera.valid) -and [DateTime]::UtcNow -lt $deadline)
 if(!$before.simulation.metadataComplete -or $before.camera.pending -or !$before.camera.valid -or $before.identity.objects[0].present -or $before.camera.budget -ne 20000){throw 'Actual DayZ cold baseline invalid'}
 $null=Send @{cmd='exec';code='streamColdActor setPos [5711.061,2512.391,0]'}
 $deadline=[DateTime]::UtcNow.AddSeconds(120)
 do{Start-Sleep -Seconds 1;$cold=Sample 'actor-cold'}while(!$cold.identity.objects[0].present -and [DateTime]::UtcNow -lt $deadline)
 $target=$cold.identity.objects[0]
 if(!$target.present -or $target.visualResident -or !$target.shapeVisualDeferred -or $target.normalVertexBuffers -ne 0 -or !$target.hasGeometry -or !$target.hasFireGeometry -or !$target.hasViewGeometry -or $target.model -ne 'dz\structures\residential\platforms\platform1_stairs_30_wallr.p3d' -or $cold.simulation.positiveColdCreated -le $before.simulation.positiveColdCreated){throw 'Real DayZ actor failed source-bound CPU-only cold admission'}
 if($cold.simulation.status -ne 'CapacityExceeded' -or $cold.simulation.readyRegions -ne 0 -or $cold.camera.required -ne 0){throw 'Partial production admission changed query readiness or pins'}
 if($RequirePreparedAlphaFacts -and ($cold.camera.preparedAlphaFactsPuts -le 0 -or $cold.camera.preparedAlphaFactsHits -le 0)){throw 'Actual DayZ did not publish and consume any prepared alpha facts before upload'}
 if($RequireCapacitySkip -and ($cold.camera.preparedAlphaFactsCapacitySkipped -le 0)){throw 'Actual DayZ did not exercise advisory alpha capacity skips'}
 $fire=Send @{cmd='stream_fire_probe';from=@(5705.061035,68.4318695,2512.391357);to=@(5717.061035,68.4318695,2512.391357)}
 if(!@($fire.contacts | Where-Object {$_.id -eq 83912800}).Count){throw 'Cold target failed known real fire ray'}
 $query=@{cmd='stream_simulation_query';actor='streamColdActor';action='queue';from=@(5705.061035,68.4318695,2512.391357);to=@(5717.061035,68.4318695,2512.391357);radius=0.01;purpose=1}
 $queued=Send $query;$query.action='collide';$collision=Send $query
 if($collision.status -ne 'CapacityExceeded' -or @($collision.contacts).Count){throw 'Partial production query fabricated complete contacts'}
 $promoted=$null;$demoted=$null;$damage=$null
 if($Lifecycle){
  $null=Send @{cmd='eval';code='triFreeFlyPose "5704.58 2523.37 73.74 260.9 -3.8"'}
  $deadline=[DateTime]::UtcNow.AddSeconds(240)
  do{Start-Sleep -Seconds 2;$promoted=Sample 'camera-promoted'}while(($promoted.camera.pending -or !$promoted.identity.objects[0].visualResident -or $promoted.identity.objects[0].normalVertexBuffers -eq 0) -and [DateTime]::UtcNow -lt $deadline)
  if($promoted.camera.pending -or !$promoted.identity.objects[0].present -or !$promoted.identity.objects[0].visualResident -or $promoted.identity.objects[0].shapeVisualDeferred -or $promoted.identity.objects[0].normalVertexBuffers -eq 0 -or $promoted.camera.required -ne 0){throw 'Cold actor target failed normal camera promotion'}
  $promotedFire=Send @{cmd='stream_fire_probe';from=@(5705.061035,68.4318695,2512.391357);to=@(5717.061035,68.4318695,2512.391357)}
  if(!@($promotedFire.contacts|Where-Object {$_.id -eq 83912800}).Count){throw 'Camera promotion lost target physical contact'}
  $null=Send @{cmd='eval';code='triFreeFlyPose "10000 10000 450 45 -10"'}
  $deadline=[DateTime]::UtcNow.AddSeconds(240)
  do{Start-Sleep -Seconds 2;$demoted=Sample 'camera-demoted'}while(($demoted.camera.pending -or $demoted.identity.objects[0].visualResident) -and [DateTime]::UtcNow -lt $deadline)
  # Camera demotion removes the instance from rendering. The established bank
  # keeps promoted model buffers; this is not proof of model-level GPU eviction.
  if($demoted.camera.pending -or !$demoted.identity.objects[0].present -or $demoted.identity.objects[0].visualResident){throw 'Camera eviction lost actor target or retained visual instance'}
  $damage=Send @{cmd='stream_existing_damage';id=83912800;damage=0.25}
  if(!$damage.mustBeSaved -or [Math]::Abs($damage.rawDamage-0.25) -gt 0.001){throw 'Actual cold target mutation not retained'}
 }
 $null=Send @{cmd='exec';code='streamColdActor setPos [10002,10000,0]'}
 $deadline=[DateTime]::UtcNow.AddSeconds(90)
 do{Start-Sleep -Seconds 1;$released=Sample 'actor-away'}while((($released.identity.objects[0].present -and !$Lifecycle) -or $released.simulation.positiveColdWorkingModels -ne 0) -and [DateTime]::UtcNow -lt $deadline)
 if($released.camera.required -ne 0 -or $released.simulation.positiveColdWorkingModels -ne 0 -or $released.simulation.positiveColdFactsBytes -ne 0 -or $released.simulation.payloadCapacityCharge -ne 0 -or $released.simulation.shapeSchedulingCharge -ne 0){throw 'Transient DayZ cold job debt failed retirement'}
 if($Lifecycle){$saved=$released.identity.objects[0];if(!$saved.present -or !$saved.mustBeSaved -or [Math]::Abs($saved.rawDamage-0.25) -gt 0.001 -or $saved.visualResident){throw 'Actor retirement lost saved target damage or retained visual instance'}}
 elseif($released.identity.objects[0].present){throw 'Transient DayZ cold target failed retirement'}
 $saveEvidence=$null
 if($SaveRoundTrip){
  # Binary save/reload of a genuinely admitted, mutated object in the same
  # authored world; FreshReload additionally destroys the first live process.
  # No wall object getter or script reference supplies a permanent pin.
  $saveLabel='cold-positive-away'
  $stream.ReadTimeout=120000
  try{$saveReply=Send @{cmd='eval';code=('triSaveGame "'+$saveLabel+'"')}}finally{$stream.ReadTimeout=30000}
  if($saveReply.result.Trim('"') -ne 'OK'){throw 'Cold mutated-object binary save failed'}
  # The private profile contains the actual engine user directory (Users/name),
  # rather than placing Saved/Tmp immediately at POSEIDON_USER_DIR.
  $saveFiles=@(Get-ChildItem -LiteralPath $profile -Recurse -File -Filter ($saveLabel+'.fps') | Where-Object {$_.Directory.Name -eq 'Tmp' -and $_.Directory.Parent.Name -eq 'Saved'})
  if($saveFiles.Count -ne 1 -or $saveFiles[0].Length -le 0){throw 'Binary save missing or ambiguous in the private profile Saved/Tmp directory'}
  $saveHash=Get-FileHash -LiteralPath $saveFiles[0].FullName -Algorithm SHA256
  $controlDamage=Send @{cmd='stream_existing_damage';id=83912800;damage=0.75}
  $control=Sample 'save-control-mutation'
  if(!$control.identity.objects[0].present -or [Math]::Abs($control.identity.objects[0].rawDamage-0.75) -gt 0.001 -or !$control.identity.objects[0].mustBeSaved -or $control.camera.required -ne 0){throw 'Existing cold mutation control failed before binary reload'}
  $freshBefore=$null;$reloadProvenance=$null
  if($FreshReload){
   if(!$saved.PSObject.Properties['frame'] -or @($saved.frame).Count -ne 12 -or !$target.PSObject.Properties['frame'] -or @($target.frame).Count -ne 12){throw 'Fresh saved-object verification requires the readonly full-frame identity probe'}
   # Frame layout: aside/up/forward/translation, each xyz. Compare actual cold
   # admission with the damaged save, allowing only 1e-6 absolute float drift.
   for($i=0;$i -lt 12;$i++){if([Math]::Abs([double]$saved.frame[$i]-[double]$target.frame[$i]) -gt 0.000001){throw "Lifecycle changed original cold target frame coefficient $i"}}
   $firstProcessId=$p.Id;$firstProcessStart=$p.StartTime.ToUniversalTime().ToString('o')
   $reloadLog=Join-Path $out 'reload-engine.log'
   $reloadArgs=$p.StartInfo.Arguments.Replace($log,$reloadLog).Replace((Join-Path $out 'capture.json'),(Join-Path $out 'reload-capture.json'))
   $null=Send @{cmd='exit'}
   if(!$p.WaitForExit(15000) -or $p.ExitCode){throw 'Save process failed clean exit before fresh reload'}
   if(Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet){throw 'Save process renderer error'}
   $client.Dispose();$client=$null
   if((Get-FileHash -LiteralPath $saveFiles[0].FullName -Algorithm SHA256).Hash -ne $saveHash.Hash){throw 'Saved binary changed before fresh-process restoration'}
   # Same private profile, exact mission/world/addons/knobs. Both actors begin
   # inland far away; no remote actor demand or object getter reconstructs target.
   $p=Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $reloadArgs
   $null=$p.Handle;$client=[Net.Sockets.TcpClient]::new();$deadline=[DateTime]::UtcNow.AddSeconds(120)
   while(!$client.Connected){try{$client.Connect('127.0.0.1',$port)}catch{if($p.HasExited -or [DateTime]::UtcNow -gt $deadline){throw};Start-Sleep -Milliseconds 250}}
   $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
   $deadline=[DateTime]::UtcNow.AddSeconds(240)
   do{Start-Sleep -Seconds 2;$freshBefore=Sample 'fresh-before-load'}while((!$freshBefore.simulation.metadataComplete -or !$freshBefore.camera.valid -or $freshBefore.camera.pending) -and [DateTime]::UtcNow -lt $deadline)
   if(!$freshBefore.simulation.metadataComplete -or !$freshBefore.camera.valid -or $freshBefore.camera.pending -or $freshBefore.identity.objects[0].present -or $freshBefore.camera.required -ne 0 -or $freshBefore.camera.budget -ne 20000 -or $freshBefore.camera.centerX -ne $released.camera.centerX -or $freshBefore.camera.centerZ -ne $released.camera.centerZ){throw 'Fresh process did not establish an absent, unpinned target in the same away world'}
   $reloadProvenance=@{firstProcessId=$firstProcessId;firstProcessStartUtc=$firstProcessStart;reloadProcessId=$p.Id;reloadProcessStartUtc=$p.StartTime.ToUniversalTime().ToString('o');exeHash=(Get-FileHash $exe).Hash;rendererHash=(Get-FileHash (Join-Path $GameDir 'wgpu_renderer.dll')).Hash;world=$world;profile=$profile;mission=$mission;arguments=$reloadArgs;environment=$environment;saveSha256=$saveHash.Hash;firstProcessCleanExit=$true}
   $originalProvenance=Get-Content (Join-Path $out 'provenance.json') -Raw | ConvertFrom-Json
   if($reloadProvenance.exeHash -ne $originalProvenance.hashes[0].Hash -or $reloadProvenance.rendererHash -ne $originalProvenance.hashes[1].Hash){throw 'Installed game binaries changed between save and fresh-process reload'}
   $reloadProvenance | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $out 'reload-provenance.json')
  }
  $stream.ReadTimeout=120000
  try{$loadReply=Send @{cmd='eval';code=('triLoadGame "'+$saveLabel+'"')}}finally{$stream.ReadTimeout=30000}
  if($loadReply.result.Trim('"') -ne 'OK'){throw 'Cold mutated-object binary reload failed'}
  $deadline=[DateTime]::UtcNow.AddSeconds(240)
  do{Start-Sleep -Seconds 2;$restored=Sample 'save-restored'}while((!$restored.camera.valid -or $restored.camera.pending) -and [DateTime]::UtcNow -lt $deadline)
  $restoredTarget=$restored.identity.objects[0]
  if(!$restored.camera.valid -or $restored.camera.pending -or !$restoredTarget.present -or $restoredTarget.id -ne $saved.id -or $restoredTarget.model -ne $saved.model -or !$restoredTarget.mustBeSaved -or [Math]::Abs($restoredTarget.rawDamage-0.25) -gt 0.001 -or $restoredTarget.visualResident -or $restored.camera.centerX -ne $released.camera.centerX -or $restored.camera.centerZ -ne $released.camera.centerZ){throw 'Same-world reload failed saved identity/damage or away camera residency'}
  foreach($axis in @('x','y','z')){if([Math]::Abs($restoredTarget.$axis-$saved.$axis) -gt 0.001){throw "Reload changed saved target coordinate $axis"}}
  if($FreshReload){
   if(!$restoredTarget.PSObject.Properties['frame'] -or @($restoredTarget.frame).Count -ne 12){throw 'Fresh restored-object identity probe omitted full frame'}
   for($i=0;$i -lt 12;$i++){if([Math]::Abs([double]$restoredTarget.frame[$i]-[double]$saved.frame[$i]) -gt 0.000001){throw "Fresh reload changed saved target frame coefficient $i"}}
   if([Math]::Abs([double]$restoredTarget.radius-[double]$saved.radius) -gt 0.000001 -or $restored.simulation.status -eq 'Ready' -or $restored.simulation.readyRegions -ne 0 -or $restored.simulation.readyQueries -ne 0){throw 'Fresh reload changed saved radius or fabricated complete readiness'}
   if(!$restoredTarget.shapeVisualDeferred -or $restoredTarget.normalVertexBuffers -ne 0 -or !$restoredTarget.hasGeometry -or !$restoredTarget.hasFireGeometry -or !$restoredTarget.hasViewGeometry){throw 'Fresh saved target restoration lacks CPU-only three-role geometry'}
  }
  $restoredFire=Send @{cmd='stream_fire_probe';from=@(5705.061035,68.4318695,2512.391357);to=@(5717.061035,68.4318695,2512.391357)}
  if(!@($restoredFire.contacts|Where-Object {$_.id -eq 83912800}).Count){throw 'Same-world reload lost saved target fire geometry'}
  # Object::CreateObject invokes ResolveObject during load. Its required pins
  # are loader ownership, so postload zero pins is intentionally not asserted.
  $saveScope=if($FreshReload){'Fresh-process SaveBin/LoadBin of one previously unpinned cold mutation in the same authored world; not multiplayer or performance acceptance'}else{'Same-world SaveBin/LoadBin only; not fresh-process, multiplayer or performance acceptance'}
  $saveEvidence=@{scope=$saveScope;freshReload=[bool]$FreshReload;frameTolerance=0.000001;save=$saveReply;saveFile=$saveFiles[0].FullName;saveBytes=$saveFiles[0].Length;saveSha256=$saveHash.Hash;requiredBeforeSave=$released.camera.required;controlDamage=$controlDamage;control=$control;freshBeforeLoad=$freshBefore;reloadProvenance=$reloadProvenance;load=$loadReply;restored=$restored;restoredFire=$restoredFire;loaderRequiredObjects=$restored.camera.required}
 }
 $null=Send @{cmd='exit'};if(!$p.WaitForExit(15000) -or $p.ExitCode){throw 'Game failed clean exit'}
 if(Select-String -LiteralPath $log -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet){throw 'Renderer error'}
 if($reloadLog -and (Select-String -LiteralPath $reloadLog -Pattern 'Out of Memory|Validation Error|panicked at|DeviceLost|UNHANDLED EXCEPTION' -Quiet)){throw 'Fresh reload renderer error'}
 @{passed=$true;lifecycle=[bool]$Lifecycle;saveRoundTrip=[bool]$SaveRoundTrip;freshReload=[bool]$FreshReload;pboTextures=[bool]$PBOTextures;requiredPreparedAlphaFacts=[bool]$RequirePreparedAlphaFacts;requiredCapacitySkip=[bool]$RequireCapacitySkip;saveEvidence=$saveEvidence;before=$before;cold=$cold;fire=$fire;queued=$queued;collision=$collision;promoted=$promoted;demoted=$demoted;damage=$damage;released=$released;performanceMeasured=$false} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'result.json')
 Write-Output "DayZ cold actor evidence: $out"
}finally{
 if($client){$client.Dispose()};if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}
 foreach($name in $savedEnvironment.Keys){[Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name])}
}
