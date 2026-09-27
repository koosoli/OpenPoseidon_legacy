param([ValidateSet('Valid','HashMismatch','DisabledArguments','DisabledNoArguments')][string]$Mode='Valid',[string]$Label='original-geometry-admission',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',[string]$Python='C:\Program Files\Python311\python.exe',[string]$OriginalSource='', [string]$Manifest='', [string]$Tools='')
$ErrorActionPreference='Stop'
if($Label -notmatch '^[a-zA-Z0-9_-]{1,64}$'){throw 'Invalid bounded label'}
if(!$env:LOCK_OWNER){
 $shell=Join-Path $PSHOME $(if($PSVersionTable.PSEdition -eq 'Core'){'pwsh.exe'}else{'powershell.exe'})
 $args=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$shell.Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python,'-Mode',$Mode)
 foreach($pair in @(@('OriginalSource',$OriginalSource),@('Manifest',$Manifest),@('Tools',$Tools))){if($pair[1]){$args+=@('-'+$pair[0],$pair[1])}}
 $env:LOCK_OWNER='original geometry startup admission fixture'
 try{& 'C:\Program Files\Git\bin\bash.exe' @args;if($LASTEXITCODE){throw "Fixture child failed $LASTEXITCODE"}}finally{Remove-Item Env:LOCK_OWNER}
 return
}
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
if(!$OriginalSource){$OriginalSource=Join-Path $root 'build/original-mlod-cli-20260927/input.p3d'}
if(!$Manifest){$Manifest=Join-Path $root 'build/original-mlod-cli-20260927/valid-output/manifest.json'}
if(!$Tools){$Tools=Join-Path $root 'build/win-x64-clang-rwdi/apps/tools/Tools/PoseidonTools.exe'}
$OriginalSource=[IO.Path]::GetFullPath($OriginalSource);$Manifest=[IO.Path]::GetFullPath($Manifest);$Tools=[IO.Path]::GetFullPath($Tools)
if($OriginalSource.Length -gt 1023 -or $OriginalSource -match '[^\x20-\x7e]' -or $OriginalSource.Contains('"')){throw 'Original path must be bounded ASCII without quote'}
$positive=$Mode -in @('Valid','DisabledNoArguments')
$enabled=$Mode -in @('Valid','HashMismatch')

$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('op-cp-'+[guid]::NewGuid().ToString('N').Substring(0,8))
$profile=Join-Path $out 'user';$mission=Join-Path $out 'cold-paa.eden'
New-Item -ItemType Directory $out,$profile,$mission | Out-Null
trap {if($out -and (Test-Path -LiteralPath $out) -and !(Test-Path -LiteralPath (Join-Path $out 'failure.json'))){@{passed=$false;mode=$Mode;reason=$_.Exception.Message;phase='setup-or-propagated-failure';normalShutdown=$false;forcedTermination=$false}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'failure.json')};break}

$generator=Join-Path $PSScriptRoot 'streaming/build_cold_paa_handoff_fixture.py'
$producerLines=@(& $Python $generator $fixture)
if($LASTEXITCODE -or $producerLines.Count -ne 1 -or $producerLines[0].Length -gt 8192){throw 'Original bounded producer failed'}
$producer=$producerLines[0]|ConvertFrom-Json
if($producer.schema -ne 1 -or !$producer.originalGenerated -or $producer.modelCount -ne 2 -or @($producer.placements).Count -ne 2 -or $producer.chainBytes -ne 5456 -or $producer.selectedMipCount -ne 4 -or $producer.selectedChainBytes -ne 5440 -or $producer.memberBytes -gt 16777216){throw 'Producer contract mismatch'}
foreach($file in $producer.files){if(!(Test-Path -LiteralPath $file.path) -or (Get-Item -LiteralPath $file.path).Length -ne $file.bytes -or (Get-FileHash -LiteralPath $file.path).Hash -ne $file.sha256){throw 'Producer file hash/size mismatch'}}
$producerLines[0]|Set-Content (Join-Path $out 'producer.json')

function RequireFields($r,$names){foreach($name in $names){if($null -eq $r -or $null -eq $r.PSObject.Properties[$name] -or $null -eq $r.$name){throw "Missing producer field $name"}}}
function EqualJson($a,$b){
 if($null -eq $a -or $null -eq $b){return $null -eq $a -and $null -eq $b}
 if($a -is [pscustomobject] -and $b -is [pscustomobject]){
  $keys=@($a.PSObject.Properties.Name);if((@($keys|Sort-Object)-join '|') -cne (@($b.PSObject.Properties.Name|Sort-Object)-join '|')){return $false}
  foreach($key in $keys){if(!(EqualJson $a.$key $b.$key)){return $false}};return $true
 }
 if($a -is [array] -and $b -is [array]){if($a.Count -ne $b.Count){return $false};for($i=0;$i -lt $a.Count;$i++){if(!(EqualJson $a[$i] $b[$i])){return $false}};return $true}
 return $a.GetType() -eq $b.GetType() -and $a -ceq $b
}
foreach($file in @($OriginalSource,$Manifest,$Tools)){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing original producer input $file"}}
if((Get-Item -LiteralPath $Manifest).Length -gt 8192){throw 'Manifest exceeds8192bytes'}
$rawBytes=(Get-Item -LiteralPath $OriginalSource).Length
if($rawBytes -lt 1 -or $rawBytes -gt 131072){throw 'Original source exceeds admission cap'}
$rawHash=(Get-FileHash -LiteralPath $OriginalSource).Hash.ToLowerInvariant()
$trusted=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
$toolsHash=(Get-FileHash -LiteralPath $Tools).Hash
$stdout=Join-Path $out 'original-producer-stdout.json';$stderr=Join-Path $out 'original-producer-stderr.txt';$offline=Join-Path $out 'offline-original'
$toolProcess=$null
try{
 $toolProcess=Start-Process -FilePath $Tools -ArgumentList @('geometry-page-mlod','--input',('"'+$OriginalSource+'"'),'--output-directory',('"'+$offline+'"')) -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
 $until=[DateTime]::UtcNow.AddSeconds(60)
 while(!$toolProcess.HasExited){foreach($f in @($stdout,$stderr)){if((Test-Path -LiteralPath $f) -and (Get-Item -LiteralPath $f).Length -gt 8192){throw 'Offline output exceeds8192bytes'}};if([DateTime]::UtcNow -gt $until){throw 'Offline producer timed out'};Start-Sleep -Milliseconds 100}
 $toolProcess.WaitForExit();if($toolProcess.ExitCode -ne 0){throw 'Offline original producer failed'}
}finally{if($toolProcess -and !$toolProcess.HasExited){Stop-Process -Id $toolProcess.Id -Force;$toolProcess.WaitForExit(5000)|Out-Null}}
foreach($f in @($stdout,$stderr,(Join-Path $offline 'manifest.json'))){if(!(Test-Path -LiteralPath $f) -or (Get-Item -LiteralPath $f).Length -gt 8192){throw 'Missing/bounded offline evidence'}}
$originalProducer=Get-Content -LiteralPath $stdout -Raw|ConvertFrom-Json
RequireFields $originalProducer @('schemaVersion','producer','originalSubsetVersion','file','fileBytes','fileSha256','sourceBytes','sourceResolutionBits','diskCodecSchema','sVertexBytes','sVertexLayoutKeyHex','clodLibraryRevision','clodAdapterVersion','ramAdapterVersion','originalSource','selectedCut','coarseThresholdBits','fineThresholdBits','originalFineVertices','originalFineTriangles','authoredFallbackTriangles','coarseTriangles','fineTriangles','coarsePages','finePages','selectedClusters','bakeGroups','bakeClusters','knownSourceBytes','scope')
if(!(EqualJson $originalProducer $trusted) -or !(EqualJson $originalProducer (Get-Content -LiteralPath (Join-Path $offline 'manifest.json') -Raw|ConvertFrom-Json))){throw 'Fresh stdout differs from trusted/file manifest'}
if($originalProducer.schemaVersion -ne 2 -or $originalProducer.producer -cne 'controlled-original-mlod' -or $originalProducer.originalSubsetVersion -ne 1 -or $originalProducer.sourceBytes -ne $rawBytes -or $originalProducer.originalSource.sourceSha256 -cne $rawHash -or $originalProducer.file -cne 'selected.gcd' -or $originalProducer.diskCodecSchema -ne 1 -or $originalProducer.sVertexBytes -ne 68 -or $originalProducer.sVertexLayoutKeyHex -cne 'df19d63f75aab18d' -or $originalProducer.clodLibraryRevision -cne '9e1f07b159d3cb777f1c67ed31fc11fd117986f4' -or $originalProducer.clodAdapterVersion -ne 1 -or $originalProducer.ramAdapterVersion -ne 1){throw 'Original pin/layout/source mismatch'}
foreach($identity in @($originalProducer.originalSource,$originalProducer.selectedCut.source)){
 RequireFields $identity @('sourceSha256','geometryOptionsHex','materialOptionsHex','producerVersion','coarseRepresentation','fineRepresentation','vertexLayout','materialMapping')
 if($identity.sourceSha256 -notmatch '^[0-9a-f]{64}$' -or $identity.geometryOptionsHex -cne '00004d4c4f445031' -or $identity.materialOptionsHex -cne '0000000000000000' -or $identity.producerVersion -ne 1 -or $identity.coarseRepresentation -notin @(0,1) -or $identity.fineRepresentation -notin @(0,1) -or $identity.coarseRepresentation -eq $identity.fineRepresentation -or $identity.vertexLayout -ne 68 -or $identity.materialMapping -ne 1){throw 'Full source-key mismatch'}
}
if($originalProducer.selectedCut.source.sourceSha256 -ceq $rawHash -or $originalProducer.selectedCut.formatVersion -ne 1 -or $originalProducer.selectedCut.algorithmVersion -ne 2 -or $originalProducer.selectedCut.packing.clusterVertices -ne 64 -or $originalProducer.selectedCut.packing.clusterTriangles -ne 124 -or $originalProducer.selectedCut.packing.pageBytes -ne 65536 -or $originalProducer.fileBytes -lt 1 -or $originalProducer.fileBytes -gt 131072 -or $originalProducer.knownSourceBytes -gt 131072){throw 'Selected package scope/cap mismatch'}
foreach($f in @((Join-Path $offline 'selected.gcd'),(Join-Path (Split-Path $Manifest -Parent) 'selected.gcd'))){if(!(Test-Path -LiteralPath $f) -or (Get-Item -LiteralPath $f).Length -ne $originalProducer.fileBytes -or (Get-FileHash -LiteralPath $f).Hash -ine $originalProducer.fileSha256){throw 'Selected file hash/length mismatch'}}
if((Get-FileHash -LiteralPath $OriginalSource).Hash -ine $rawHash -or (Get-FileHash -LiteralPath $Tools).Hash -ne $toolsHash){throw 'Source/Tools changed during proof'}
$id=$originalProducer.originalSource
$originalKey=@([Convert]::ToUInt64($id.geometryOptionsHex,16),[Convert]::ToUInt64($id.materialOptionsHex,16),$id.producerVersion,$id.coarseRepresentation,$id.fineRepresentation,$id.vertexLayout,$id.materialMapping)-join ':'
$suppliedHash=$rawHash
if($Mode -eq 'HashMismatch'){$suppliedHash=$(if($rawHash[0] -eq '0'){'1'}else{'0'})+$rawHash.Substring(1)}
@{original=$OriginalSource;bytes=$rawBytes;rawSha256=$rawHash;suppliedSha256=$suppliedHash;fullKey=$originalKey;manifest=$Manifest;producer=$originalProducer;tools=$Tools;toolsSha256=$toolsHash}|ConvertTo-Json -Depth 14|Set-Content (Join-Path $out 'original-proof.json')

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
 WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF='0';WGR_OBJECT_STREAM_ASYNC='0';WGR_OBJECT_STREAM_ASYNC_ADAPT='0';WGR_OBJECT_STREAM_ASYNC_WORKERS='2'
 WGR_OBJECT_STREAM_WARM_TEXTURES='0';WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS='0';WGR_OBJECT_STREAM_PBO_TEXTURES='0';WGR_OBJECT_STREAM_PBO='0'
 WGR_OBJECT_STREAM_WINDOW_GROWTH='0';WGR_OBJECT_STREAM_RADIUS_CELLS='8';WGR_OBJECT_STREAM_GPU_BUDGET='0';WGR_OBJECT_STREAM_MAX_OBJECTS='20000';WGR_OBJECT_STREAM_TEST_BUDGET='1'
 WGR_NATIVE_DDS_PREPARE='0';WGR_NATIVE_DDS_BC3_ONLY='0';WGR_ADAPTIVE_TEXTURE_DETAIL='0';WGR_WATER_BACKEND='0';WGR_GEOMETRY_PAGE_FIXTURE='0'
 WGR_CULL_MODEL_ROW_UPLOAD='0';WGR_LOCAL_SHADOW_POSE_CACHE='0';WGR_LOCAL_POSE_CACHE_TRACE='0';WGR_LAZY_TEXTURE_BIND_GROUPS='0';WGR_GEOMETRY_OWNER_LEDGER='0';WGR_PAA_PREP_INFLIGHT='0';WGR_PAA_PREP_ATTEMPT_DIAG='0';WGR_TERRAIN_PAGE_TIMINGS='0';WGR_OBJECT_STREAM_WARM_PROFILE='0';WGR_RENDER_CALL_CPU_TIMINGS='0';WGR_GPU_MODEL_PARK_REFILL='0';WGR_OBJECT_STREAM_REGISTRATION_QUOTA='0';WGR_CULL_SECTION_REUSE='0';WGR_CULL_LOD_REUSE='0';WGR_PAA_LZO_REPLAY_CACHE='0'
}
$environment.WGR_GEOMETRY_PAGE_ORIGINAL_SOURCE=$(if($enabled){'1'}else{'0'})
$old=@{};Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {$old[$_.Name]=$_.Value}
$p=$null;$client=$null;$normal=$false;$forced=$false;$result=$null;$failure=$null
$exe=Join-Path $GameDir 'OpenPoseidon.exe';$dll=Join-Path $GameDir 'wgpu_renderer.dll';$log=Join-Path $out 'engine.log'
$hashes=@(Get-FileHash -LiteralPath $exe,$dll)
try{
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $environment.Keys){Set-Item ('Env:'+$key) $environment[$key]}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',[string]$port,'--test-world-freefly','100','100','80','45','-25','--test-world',('"'+(Join-Path $fixture 'cold-paa.wrp')+'"'),'--addon-root',('"'+(Join-Path $fixture 'addons')+'"'),'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 if($Mode -ne 'DisabledNoArguments'){$argv+=@('--geometry-original-source',('"'+$OriginalSource+'"'),'--geometry-original-sha256',$suppliedHash,'--geometry-original-bytes',[string]$rawBytes,'--geometry-original-key',$originalKey)}
 @{head=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash $PSCommandPath).Hash;generatorSha256=(Get-FileHash $generator).Hash;installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));hashes=$hashes;environment=$environment;argv=$argv;fixture=$producer;originalProof=$originalProducer;mode=$Mode;toolsSha256=$toolsHash;scope='Startup original-byte admission only, normal two-object coverage; no original GPU consumer/all-pass/FPS proof'}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'provenance.json')
 $p=Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $argv
 $null=$p.Handle
 if(!$positive){
  if(!$p.WaitForExit(20000)){throw 'Negative startup did not exit within20seconds'}
  if($p.ExitCode -ne 2){throw "Expected startup refusal exit2, got $($p.ExitCode)"};$normal=$true
  $text=Get-Content -LiteralPath $log -Raw
  if($text -match 'Out of memory|Validation Error|panicked at|DeviceLost|UNHANDLED'){throw 'Negative startup runtime error'}
  if($text -notmatch 'Shutdown: begin \(exit code 2\)' -or $text -notmatch 'Shutdown complete'){throw 'No normal shutdown marker'}
  if($text -match 'Original geometry startup admission status=1'){throw 'Negative case admitted source'}
  if($Mode -eq 'HashMismatch' -and $text -notmatch 'Original geometry startup admission status=7 .*snapshotValidated=(false|0)'){throw 'Missing typed identity mismatch'}
  if($Mode -eq 'DisabledArguments' -and $text -notmatch 'Original geometry startup admission: explicit arguments require exact opt-in flag; no fixture published'){throw 'Missing explicit OFF-arguments rejection'}
  $result=@{passed=$true;mode=$Mode;normalShutdown=$true;forcedTermination=$false;exitCode=$p.ExitCode;scope='Bounded startup refusal; no admission/renderer coverage claimed'}
 }else{
 $client=[Net.Sockets.TcpClient]::new();$until=[DateTime]::UtcNow.AddSeconds(90)
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
 if(@($after.identity.objects|Where-Object {$_.present -and $_.visualResident -and $_.normalVertexBuffers -gt 0}).Count -ne 2 -or !$after.camera.valid -or $after.camera.pending -or $after.camera.resident -ne 2 -or $after.camera.desired -ne 2 -or $after.camera.required -ne 0 -or $after.camera.budget -ne 20000){throw 'Original two-model coverage not established'}
 Start-Sleep -Seconds 3
 $archiveAfter=Send @{cmd='archive_source_bindings'};Require $archiveAfter @('enabled','liveBindings','knownCppBytes','wrappedReads','initRetains')
 $archiveAfter|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'archive-after.json')
 $null=Send @{cmd='screenshot';path=(Join-Path $out 'fixture.png')}
 $null=Send @{cmd='exit'}
 if(!$p.WaitForExit(15000) -or $p.ExitCode -ne 0){throw 'Game did not exit normally'};$normal=$true
 $text=Get-Content -LiteralPath $log -Raw
 if($text -match 'Out of memory|Validation Error|panicked at|DeviceLost|UNHANDLED'){throw 'Runtime error log'}
 if($text -notmatch 'Shutdown: begin \(exit code 0\)' -or $text -notmatch 'Shutdown complete'){throw 'Missing normal control shutdown'}
 $admissionRows=@($text -split "`r?`n"|Where-Object {$_ -match 'Original geometry startup admission status='})
 if($Mode -eq 'Valid'){
  if($admissionRows.Count -ne 1 -or $admissionRows[0] -notmatch 'status=1 epoch=1 rawBytes=(\d+) knownRetainedBytes=(\d+) snapshotValidated=(true|1) scope=source-probe-only-no-render-consumer'){throw 'Missing exact successful startup source cut'}
  if([uint64]$Matches[1] -ne $rawBytes -or [uint64]$Matches[2] -lt 1 -or [uint64]$Matches[2] -gt 131072){throw 'Invalid admission byte charge'}
 }elseif($admissionRows.Count){throw 'DisabledNoArguments unexpectedly ran admission'}
 $afterHashes=@(Get-FileHash -LiteralPath $exe,$dll)
 for($i=0;$i -lt 2;$i++){if($hashes[$i].Hash -ne $afterHashes[$i].Hash){throw 'Installed artifact changed during fixture'}}
 $result=@{passed=$true;mode=$Mode;admissionRows=$admissionRows;normalShutdown=$true;forcedTermination=$false;exitCode=$p.ExitCode;baseline=$baseline;after=$after;archiveBefore=$archiveBefore;archiveAfter=$archiveAfter;scope='Actual startup admission and normal two-model camera coverage; no original-asset render consumer/pixel/all-pass/performance proof'}
 }
 # Verify original producer, source, installed artifacts remained unchanged for every mode.
 if((Get-FileHash -LiteralPath $OriginalSource).Hash -ine $rawHash -or (Get-FileHash -LiteralPath $Tools).Hash -ne $toolsHash){throw 'Original proof changed during game'}
 $finalHashes=@(Get-FileHash -LiteralPath $exe,$dll);for($i=0;$i -lt 2;$i++){if($hashes[$i].Hash -ne $finalHashes[$i].Hash){throw 'Installed artifact changed'}}
}catch{$failure=$_}
finally{
 if($p -and !$p.HasExited){try{if($client -and $client.Connected){$null=Send @{cmd='exit'};$normal=$p.WaitForExit(15000) -and $p.ExitCode -eq 0}}catch{};if(!$p.HasExited){Stop-Process -Id $p.Id -Force;$forced=$true;$p.WaitForExit(5000)|Out-Null}}
 if($client){$client.Dispose()}
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $old.Keys){Set-Item ('Env:'+$key) $old[$key]}
 if($failure){@{passed=$false;reason=$failure.Exception.Message;mode=$Mode;exitCode=$(if($p -and $p.HasExited){$p.ExitCode}else{$null});normalShutdown=$normal;forcedTermination=$forced;pid=$(if($p){$p.Id}else{$null})}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'failure.json')}
}
if($failure){throw $failure}
$result|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'result.json')
Write-Host $out
