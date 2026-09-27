param([ValidateSet('None','Missing','Corrupt')][string]$DiskFault='None',[string]$Tools='',
 [string]$Label='original-geometry-pages',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',[string]$Python='C:\Program Files\Python311\python.exe')
$ErrorActionPreference='Stop'
if($Label -notmatch '^[a-zA-Z0-9_-]{1,64}$'){throw 'Invalid bounded label'}
if(!$env:LOCK_OWNER){
 $shell=Join-Path $PSHOME $(if($PSVersionTable.PSEdition -eq 'Core'){'pwsh.exe'}else{'powershell.exe'})
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$shell.Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python,'-DiskFault',$DiskFault)
 if($Tools){$argv+=@('-Tools',$Tools)}
 $env:LOCK_OWNER='original source private paged geometry fixture'
 try{& 'C:\Program Files\Git\bin\bash.exe' @argv;if($LASTEXITCODE){throw "Original pages child failed $LASTEXITCODE"}}finally{Remove-Item Env:LOCK_OWNER};return
}
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
if(!$Tools){$Tools=Join-Path $root 'build/win-x64-clang-rwdi/apps/tools/Tools/PoseidonTools.exe'}
$Tools=[IO.Path]::GetFullPath($Tools)
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('op-cp-'+[guid]::NewGuid().ToString('N').Substring(0,8))
$profile=Join-Path $out 'user';$mission=Join-Path $out 'cold-paa.eden'
New-Item -ItemType Directory $out,$profile,$mission | Out-Null
trap {if($out -and (Test-Path -LiteralPath $out) -and !(Test-Path -LiteralPath (Join-Path $out 'failure.json'))){@{passed=$false;diskFault=$DiskFault;reason=$_.Exception.Message;phase='setup-or-propagated-failure';normalShutdown=$false;forcedTermination=$false}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'failure.json')};break}

$generator=Join-Path $PSScriptRoot 'streaming/build_cold_paa_handoff_fixture.py'
$producerLines=@(& $Python $generator $fixture)
if($LASTEXITCODE -or $producerLines.Count -ne 1 -or $producerLines[0].Length -gt 8192){throw 'Original bounded producer failed'}
$producer=$producerLines[0]|ConvertFrom-Json
if($producer.schema -ne 1 -or !$producer.originalGenerated -or $producer.modelCount -ne 2 -or @($producer.placements).Count -ne 2 -or $producer.chainBytes -ne 5456 -or $producer.selectedMipCount -ne 4 -or $producer.selectedChainBytes -ne 5440 -or $producer.memberBytes -gt 16777216){throw 'Producer contract mismatch'}
foreach($file in $producer.files){if(!(Test-Path -LiteralPath $file.path) -or (Get-Item -LiteralPath $file.path).Length -ne $file.bytes -or (Get-FileHash -LiteralPath $file.path).Hash -ne $file.sha256){throw 'Producer file hash/size mismatch'}}
$producerLines[0]|Set-Content (Join-Path $out 'producer.json')

$originalGenerator=Join-Path $PSScriptRoot 'streaming/build_original_mlod_fixture.py'
$sourceDirectory=Join-Path $out 'original-source'
$sourceLines=@(& $Python $originalGenerator $sourceDirectory)
if($LASTEXITCODE -or $sourceLines.Count -ne 1 -or $sourceLines[0].Length -gt 8192){throw 'Original P3DM generator failed'}
$sourceFixture=$sourceLines[0]|ConvertFrom-Json
if($sourceFixture.schemaVersion -ne 1 -or @($sourceFixture.lods).Count -ne 2 -or $sourceFixture.lods[0].triangles -ne 2 -or $sourceFixture.lods[1].triangles -ne 512 -or $sourceFixture.input.bytes -gt 131072){throw 'Original source fixture contract mismatch'}
$OriginalSource=[IO.Path]::GetFullPath($sourceFixture.input.path)
if($OriginalSource.Length -gt 1023 -or $OriginalSource -match '[^\x20-\x7e]' -or $OriginalSource.Contains('"')){throw 'Bounded original path required'}
if((Get-Item -LiteralPath $OriginalSource).Length -ne $sourceFixture.input.bytes -or (Get-FileHash -LiteralPath $OriginalSource).Hash -ine $sourceFixture.input.sha256){throw 'Independent original byte proof mismatch'}
$sourceLines[0]|Set-Content (Join-Path $out 'source-fixture.json')
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
foreach($file in @($OriginalSource,$Tools)){if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw "Missing original producer input $file"}}

$rawBytes=(Get-Item -LiteralPath $OriginalSource).Length
if($rawBytes -lt 1 -or $rawBytes -gt 131072){throw 'Original source exceeds admission cap'}
$rawHash=(Get-FileHash -LiteralPath $OriginalSource).Hash.ToLowerInvariant()

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
if(!(EqualJson $originalProducer (Get-Content -LiteralPath (Join-Path $offline 'manifest.json') -Raw|ConvertFrom-Json))){throw 'Fresh stdout differs semantically from file manifest'}
if($originalProducer.schemaVersion -ne 2 -or $originalProducer.producer -cne 'controlled-original-mlod' -or $originalProducer.originalSubsetVersion -ne 1 -or $originalProducer.sourceBytes -ne $rawBytes -or $originalProducer.originalSource.sourceSha256 -cne $rawHash -or $originalProducer.file -cne 'selected.gcd' -or $originalProducer.diskCodecSchema -ne 1 -or $originalProducer.sVertexBytes -ne 68 -or $originalProducer.sVertexLayoutKeyHex -cne 'df19d63f75aab18d' -or $originalProducer.clodLibraryRevision -cne '9e1f07b159d3cb777f1c67ed31fc11fd117986f4' -or $originalProducer.clodAdapterVersion -ne 1 -or $originalProducer.ramAdapterVersion -ne 1){throw 'Original pin/layout/source mismatch'}
foreach($identity in @($originalProducer.originalSource,$originalProducer.selectedCut.source)){
 RequireFields $identity @('sourceSha256','geometryOptionsHex','materialOptionsHex','producerVersion','coarseRepresentation','fineRepresentation','vertexLayout','materialMapping')
 if($identity.sourceSha256 -notmatch '^[0-9a-f]{64}$' -or $identity.geometryOptionsHex -cne '00004d4c4f445031' -or $identity.materialOptionsHex -cne '0000000000000000' -or $identity.producerVersion -ne 1 -or $identity.coarseRepresentation -notin @(0,1) -or $identity.fineRepresentation -notin @(0,1) -or $identity.coarseRepresentation -eq $identity.fineRepresentation -or $identity.vertexLayout -ne 68 -or $identity.materialMapping -ne 1){throw 'Full source-key mismatch'}
}
if($originalProducer.selectedCut.source.sourceSha256 -ceq $rawHash -or $originalProducer.selectedCut.formatVersion -ne 1 -or $originalProducer.selectedCut.algorithmVersion -ne 2 -or $originalProducer.selectedCut.packing.clusterVertices -ne 64 -or $originalProducer.selectedCut.packing.clusterTriangles -ne 124 -or $originalProducer.selectedCut.packing.pageBytes -ne 65536 -or $originalProducer.fileBytes -lt 1 -or $originalProducer.fileBytes -gt 131072 -or $originalProducer.knownSourceBytes -lt 1 -or $originalProducer.knownSourceBytes -gt 131072 -or $originalProducer.coarseTriangles -ne 63 -or $originalProducer.fineTriangles -ne 512 -or $originalProducer.authoredFallbackTriangles -ne 2 -or $originalProducer.originalFineVertices -ne 289 -or $originalProducer.originalFineTriangles -ne 512 -or $originalProducer.coarsePages -ne 1 -or $originalProducer.finePages -ne 1 -or $originalProducer.selectedClusters -lt 1 -or $originalProducer.selectedClusters -gt 64 -or $originalProducer.scope -cne 'external-original-byte-producer; independently-verify-input-hash; no-retail/runtime/GPU/source-freshness-or-hard-peak-proof'){throw 'Selected package scope/cap mismatch'}
foreach($f in @((Join-Path $offline 'selected.gcd'))){if(!(Test-Path -LiteralPath $f) -or (Get-Item -LiteralPath $f).Length -ne $originalProducer.fileBytes -or (Get-FileHash -LiteralPath $f).Hash -ine $originalProducer.fileSha256){throw 'Selected file hash/length mismatch'}}
if((Get-FileHash -LiteralPath $OriginalSource).Hash -ine $rawHash -or (Get-FileHash -LiteralPath $Tools).Hash -ne $toolsHash){throw 'Source/Tools changed during proof'}
$id=$originalProducer.originalSource
$originalKey=@([Convert]::ToUInt64($id.geometryOptionsHex,16),[Convert]::ToUInt64($id.materialOptionsHex,16),$id.producerVersion,$id.coarseRepresentation,$id.fineRepresentation,$id.vertexLayout,$id.materialMapping)-join ':'
$suppliedHash=$rawHash

@{original=$OriginalSource;bytes=$rawBytes;rawSha256=$rawHash;suppliedSha256=$suppliedHash;fullKey=$originalKey;producer=$originalProducer;tools=$Tools;toolsSha256=$toolsHash}|ConvertTo-Json -Depth 14|Set-Content (Join-Path $out 'original-proof.json')

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
$environment.WGR_GEOMETRY_PAGE_ORIGINAL_SOURCE='1';$environment.WGR_GEOMETRY_PAGE_FIXTURE='1'
$old=@{};Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {$old[$_.Name]=$_.Value}
$p=$null;$client=$null;$normal=$false;$forced=$false;$result=$null;$failure=$null
$exe=Join-Path $GameDir 'OpenPoseidon.exe';$dll=Join-Path $GameDir 'wgpu_renderer.dll';$log=Join-Path $out 'engine.log'
$hashes=@(Get-FileHash -LiteralPath $exe,$dll)
try{
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $environment.Keys){Set-Item ('Env:'+$key) $environment[$key]}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000','--harness',[string]$port,'--test-world-freefly','1184','1240','18','90','-8','--test-world',('"'+(Join-Path $fixture 'cold-paa.wrp')+'"'),'--addon-root',('"'+(Join-Path $fixture 'addons')+'"'),'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 $argv+=@('--geometry-original-source',('"'+$OriginalSource+'"'),'--geometry-original-sha256',$suppliedHash,'--geometry-original-bytes',[string]$rawBytes,'--geometry-original-key',$originalKey)
 @{head=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash $PSCommandPath).Hash;generatorSha256=(Get-FileHash $generator).Hash;installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));hashes=$hashes;environment=$environment;argv=$argv;fixture=$producer;originalProof=$originalProducer;sourceGeneratorSha256=(Get-FileHash $originalGenerator).Hash;diskFault=$DiskFault;toolsSha256=$toolsHash;scope='Actual original-source private paging; copied record cuts, no retail/all-pass/pixel/FPS proof'}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'provenance.json')
 $p=Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $argv
 $null=$p.Handle
 $client=[Net.Sockets.TcpClient]::new();$until=[DateTime]::UtcNow.AddSeconds(90)
 while(!$client.Connected){try{$client.Connect('127.0.0.1',$port)}catch{if($p.HasExited -or [DateTime]::UtcNow -gt $until){throw};Start-Sleep -Milliseconds 250}}
 $stream=$client.GetStream();$stream.ReadTimeout=15000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command){$writer.WriteLine(($command|ConvertTo-Json -Depth 12 -Compress));do{$line=$reader.ReadLine();if($null -eq $line){throw 'Harness closed'};$r=$line|ConvertFrom-Json}while($null -eq $r.ok);($command|ConvertTo-Json -Depth 12 -Compress)|Add-Content (Join-Path $out 'harness.jsonl');$line|Add-Content (Join-Path $out 'harness.jsonl');if(!$r.ok){throw $line};return $r}
 function Require($r,$names){foreach($name in $names){if($null -eq $r -or $null -eq $r.PSObject.Properties[$name] -or $null -eq $r.$name){throw "Missing field $name"}}}
    $Clod=$true;$Paged=$true;$Evict=$true;$Disk=$true
    function Action($action) {
        $command=@{cmd='geometry_page_fixture';action=$action;x=1200;y=15;z=1240}
        if($action -eq 'beginOriginalDiskClodPaged'){$command.disk=@{path=$diskPath;producer=$originalProducer};$command.sourceAdmissionEpoch=1}
        Send $command
    }
    function Capture($name) {
        $path=Join-Path $out $name
        $null=Send @{cmd='screenshot';path=$path}
        $until=[DateTime]::UtcNow.AddSeconds(5)
        while (!(Test-Path -LiteralPath $path) -and [DateTime]::UtcNow -lt $until) {Start-Sleep -Milliseconds 100}
        if (!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0) {throw "Missing capture: $path"}
    }
    $script:fixtureObservedFrame=0
    function Observe($active,$fine) {
        $until=[DateTime]::UtcNow.AddSeconds(15)
        do {
            $request=Action 'observe'
            do {Start-Sleep -Milliseconds 100;$cut=Action 'status'} while ($cut.status -eq 'Pending' -and
                !($Evict -and $cut.fineEvictionPending -and $cut.mainFrameReturned) -and [DateTime]::UtcNow -lt $until)
            if ($Evict -and $cut.status -eq 'Pending' -and $cut.fineEvictionPending -and
                $cut.mainFrameReturned -and [DateTime]::UtcNow -lt $until) {continue}
            if ($cut.status -ne 'Ready') {throw "Fixture refused: $($cut | ConvertTo-Json -Compress)"}
            foreach ($field in @('requestId','observedRequestId','active','fineSelected','mainFrameReturned',
                'cpuHelpersUnchanged','helperHashBefore','helperHashAfter','sourceSha256','coarseTriangles','fineTriangles',
                'knownPayloadBytes','meshCount','meshesPresent','meshesAbsent','producerModel','rendererModel','producerInstance','rendererInstance')) {
                if ($null -eq $cut.$field) {throw "Missing fixture field: $field"}
            }
            if ($Clod) {
                foreach ($field in @('clodPilot','fallbackSelected','clodAdapterVersion','clodLibraryRevision',
                    'authoredFineTriangles','fallbackTriangles','clodGroups','clodClusters',
                    'coarseThreshold','fineThreshold','clodBakeMs')) {
                    if ($null -eq $cut.$field) {throw "Missing CLOD fixture field: $field"}
                }
                foreach ($field in @('coarseThreshold','fineThreshold','clodBakeMs')) {
                    if ([double]::IsNaN([double]$cut.$field) -or [double]::IsInfinity([double]$cut.$field)) {
                        throw "Nonfinite CLOD fixture field: $field"
                    }
                }
            }
            $historyValid=$false
            if ($Evict) {
                foreach ($field in @('expectedPresentMeshes','expectedAbsentMeshes','observedExpectedPresentMeshes',
                    'observedExpectedAbsentMeshes','fineGeneration','fineEvictionCount','fineEvictionPending',
                    'retiredFineFirst','retiredFineCount','rendererMeshHandles')) {
                    if ($null -eq $cut.$field) {throw "Missing eviction history field: $field"}
                }
                $handles=@($cut.rendererMeshHandles)
                if ($handles.Count -ne $cut.meshCount -or $cut.fineEvictionCount -gt 1 -or $cut.fineGeneration -gt 2) {
                    throw 'Unbounded or missing full-generation mesh history'
                }
                foreach ($handle in $handles) {if ($handle -notmatch '^[1-9][0-9]*$') {throw 'Invalid historical renderer handle'}}
                if (@($handles | Select-Object -Unique).Count -ne $handles.Count) {throw 'Historical full-generation handles alias'}
                $historyValid=($cut.expectedPresentMeshes+$cut.expectedAbsentMeshes) -eq $cut.meshCount -and
                    $cut.observedExpectedPresentMeshes -eq $cut.expectedPresentMeshes -and
                    $cut.observedExpectedAbsentMeshes -eq $cut.expectedAbsentMeshes -and
                    $cut.meshesPresent -eq $cut.expectedPresentMeshes -and $cut.meshesAbsent -eq $cut.expectedAbsentMeshes
            }
            if (!$active -and $cut.meshesPresent -gt 0 -and [DateTime]::UtcNow -lt $until) {continue}
            if ($null -eq $request.requestId -or $request.requestId -lt 1 -or $cut.requestId -lt 1 -or
                $cut.observedRequestId -ne $request.requestId -or $cut.active -ne $active -or
                $cut.fineSelected -ne $fine -or !$cut.mainFrameReturned -or $cut.cpuHelpersUnchanged -or
                $cut.helperHashBefore -ne '' -or $cut.helperHashAfter -ne '' -or $cut.sourceSha256 -notmatch '^[a-fA-F0-9]{64}$' -or
                $cut.helperHashBefore -ne $cut.helperHashAfter -or
                (!$Clod -and ($cut.coarseTriangles -ne 12 -or $cut.fineTriangles -ne 48)) -or
                ($Clod -and (!$cut.clodPilot -or $cut.clodAdapterVersion -ne 1 -or
                    $cut.clodLibraryRevision -ne '9e1f07b159d3cb777f1c67ed31fc11fd117986f4' -or
                    $cut.authoredFineTriangles -ne 512 -or $cut.fallbackTriangles -ne 2 -or
                    (!$Disk -and ($cut.coarseTriangles -le 0 -or $cut.fineTriangles -le $cut.coarseTriangles -or
                        $cut.fineTriangles -gt 512 -or $cut.clodGroups -lt 2 -or $cut.clodClusters -lt 1 -or $cut.clodClusters -gt 64 -or
                        $cut.coarseThreshold -le 0 -or $cut.fineThreshold -ne 0 -or $cut.clodBakeMs -lt 0)) -or
                    ($Disk -and ($cut.clodGroups -ne 0 -or $cut.clodClusters -ne 0 -or $cut.coarseThreshold -ne 0 -or
                        $cut.fineThreshold -ne 0 -or $cut.clodBakeMs -ne 0)))) -or
                $cut.knownPayloadBytes -le 0 -or $cut.knownPayloadBytes -gt 1048576 -or $cut.meshCount -lt 1 -or $cut.meshCount -gt 64 -or
                $cut.meshesPresent -lt 0 -or $cut.meshesAbsent -lt 0 -or ($cut.meshesPresent+$cut.meshesAbsent) -ne $cut.meshCount -or
                ($active -and ($cut.rendererModel -eq 4294967295 -or $cut.rendererInstance -eq 4294967295 -or ((!$Evict -and $cut.meshesPresent -ne $cut.meshCount) -or ($Evict -and !$historyValid)))) -or
                (!$active -and ($cut.rendererInstance -ne 4294967295 -or $cut.meshesAbsent -ne $cut.meshCount))) {
                throw "Unexpected resident cut: $($cut | ConvertTo-Json -Compress)"
            }
            if ($null -eq $cut.renderReturnStatus -or $cut.renderReturnStatus -ne 0 -or $null -eq $cut.passFacts) {
                throw 'Real renderer frame/getter evidence missing or failed'
            }
            $pass=$cut.passFacts
            foreach ($field in @('valid','version','structBytes','enabled','frame','instanceEpoch','required','capabilities','pending',
                'recordedThisFrame','cascadeCount','cascadeDrawMask','cascadeEpoch','cascadeFrame','localCount','localDrawMask',
                'localValidMask','interiorDrawMask','interiorValidMask','giRsmEpoch','giRsmFrame','reflectionEpoch','reflectionFrame')) {
                if ($null -eq $pass.$field) {throw "Missing pass fact: $field"}
            }
            foreach ($field in @('frame','instanceEpoch','cascadeEpoch','cascadeFrame','giRsmEpoch','giRsmFrame','reflectionEpoch','reflectionFrame')) {
                $number=[double]$pass.$field
                if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt 0 -or
                    $number -gt 9007199254740991 -or [Math]::Floor($number) -ne $number) {throw "Inexact pass fact: $field"}
            }
            foreach ($entry in @(@{name='localEpochs';count=24},@{name='localFrames';count=24},
                @{name='interiorEpochs';count=5},@{name='interiorFrames';count=5})) {
                if ($null -eq $pass.($entry.name) -or @($pass.($entry.name)).Count -ne $entry.count) {throw "Missing bounded array: $($entry.name)"}
                foreach ($element in @($pass.($entry.name))) {
                    if ($null -eq $element) {throw "Null bounded epoch/frame element: $($entry.name)"}
                    $number=[double]$element
                    if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt 0 -or
                        $number -gt 9007199254740991 -or [Math]::Floor($number) -ne $number) {throw "Inexact bounded epoch/frame element: $($entry.name)"}
                }
            }
            if (!$pass.valid -or $pass.version -ne 2 -or $pass.structBytes -ne 592 -or $pass.enabled -ne 1 -or
                $pass.frame -le $script:fixtureObservedFrame -or $pass.cascadeCount -gt 4 -or $pass.localCount -gt 24) {
                throw 'Invalid or nonmonotonic copied renderer pass facts'
            }
            if ($cut.cascadeConfigured -and (($pass.required -band 1) -eq 0 -or ($pass.capabilities -band 1) -eq 0 -or
                ($pass.pending -band 1) -ne 0 -or $pass.cascadeCount -lt 1 -or
                $pass.cascadeDrawMask -ne ((1 -shl [int]$pass.cascadeCount)-1) -or
                $pass.cascadeEpoch -ne $pass.instanceEpoch -or $pass.cascadeFrame -ne $pass.frame)) {
                throw 'Configured cascade lacks actual current-epoch command-recording facts'
            }
            if ($pass.capabilities -ne 31) {throw 'Unexpected pass recorder capabilities'}
            if (($pass.required -band 16) -ne 0 -and ($pass.pending -band 16) -eq 0 -and
                (($pass.recordedThisFrame -band 16) -eq 0 -or $pass.reflectionEpoch -ne $pass.instanceEpoch -or
                 $pass.reflectionFrame -ne $pass.frame)) {
                throw 'Reflection marked complete without actual same-frame command-recording facts'
            }
            # Configured reflection resource refusal and stale cached local/interior/GI may remain
            # Pending. These scoped facts do not claim visible fixture pixels or all-pass completeness.
            $script:fixtureObservedFrame=[double]$pass.frame
            return $cut
        } while ([DateTime]::UtcNow -lt $until)
        throw 'Private geometry records did not settle before deadline'
    }
        function PageCut($cut) {
            foreach ($field in @('pagedPilot','fineResident','pageEpoch','pageRequest','pageWorkState',
                'pageQueued','pageActive','pageReady','pageLiveJobs','pageReservedBytes','pageCompleted','pageCancelled')) {
                if ($null -eq $cut.$field) {throw "Missing page-worker field: $field"}
            }
            if ($Clod) {
                foreach ($field in @('clodRamAdapterVersion','clodRamKnownSourceBytes','pageSourceKnownBytes','selectedCutSha256','sourceSha256','fallbackSelected')) {
                    if ($null -eq $cut.$field) {throw "Missing selected-CLOD page field: $field"}
                }
                if ($cut.clodRamAdapterVersion -ne 1 -or
                    (!$Disk -and ($cut.clodRamKnownSourceBytes -le 0 -or $cut.clodRamKnownSourceBytes -gt 131072 -or
                        $cut.pageSourceKnownBytes -le 0 -or $cut.pageSourceKnownBytes -gt 131072)) -or
                    ($Disk -and ($cut.clodRamKnownSourceBytes -ne 0 -or $cut.pageSourceKnownBytes -ne 0)) -or
                    $cut.selectedCutSha256 -notmatch '^[a-fA-F0-9]{64}$' -or $cut.selectedCutSha256 -eq $cut.sourceSha256) {
                    throw 'Selected-cut page provenance/capacity is missing or aliases raw source identity'
                }
            }
            if ($Disk) {
                foreach ($field in @('diskPilot','coarseResident','diskReadStatus','diskRequestedRepresentation')) {
                    if ($null -eq $cut.$field) {throw "Missing disk-worker field: $field"}
                }
                if (!$cut.diskPilot -or $cut.diskReadStatus -notin @('NotRequested','Read','Missing','Capacity','ReadFailed','Invalid','Unsupported','AllocationFailed','Cancelled') -or
                    $cut.diskRequestedRepresentation -notin @('Coarse','Fine') -or
                    $cut.helperHashBefore -ne '' -or $cut.helperHashAfter -ne '' -or
                    $cut.sourceSha256 -ne $originalProducer.originalSource.sourceSha256 -or
                    $cut.selectedCutSha256 -ne $originalProducer.selectedCut.source.sourceSha256) {throw 'Disk metadata/source identity mismatch'}
                if ($cut.coarseResident -and ($cut.coarseTriangles -ne $originalProducer.coarseTriangles -or
                    $cut.fineTriangles -ne $originalProducer.fineTriangles)) {throw 'Decoded selected-cut counts differ from external producer'}
                if ($cut.fineResident -and ($cut.diskReadStatus -ne 'Read' -or $cut.diskRequestedRepresentation -ne 'Fine')) {throw 'Fine residency lacks actual typed disk-read proof'}
            }
            if (!$cut.pagedPilot -or $cut.pageEpoch -lt 1 -or $cut.pageLiveJobs -gt 2 -or
                $cut.pageQueued -gt 1 -or $cut.pageActive -gt 1 -or $cut.pageReady -gt 1 -or
                $cut.pageReservedBytes -gt 1048576) {throw "Invalid bounded page-worker cut: $($cut | ConvertTo-Json -Compress)"}
            Require $cut @('originalFilePilot','sourceSnapshotValidated','sourceAdmissionConsumed','sourceAdmissionEpoch','originalSourceBytes','sourceAdmissionKnownBytes','originalOriginRadius','helperCount')
            $r=[double]$cut.originalOriginRadius
            if(!$cut.originalFilePilot -or !$cut.sourceSnapshotValidated -or !$cut.sourceAdmissionConsumed -or $cut.sourceAdmissionEpoch -ne 1 -or $cut.originalSourceBytes -ne $rawBytes -or $cut.sourceAdmissionKnownBytes -lt 1 -or $cut.sourceAdmissionKnownBytes -gt 131072 -or $cut.helperCount -ne 0 -or $cut.cpuHelpersUnchanged -or $cut.helperHashBefore -ne '' -or $cut.helperHashAfter -ne '' -or [double]::IsNaN($r) -or [double]::IsInfinity($r) -or $r -le 0 -or ($cut.knownPayloadBytes+$cut.pageReservedBytes) -gt 1048576){throw 'Original source/material/bounds/admission/debt scope mismatch'}
            return $cut
        }
        function WaitPage($predicate) {
            $until=[DateTime]::UtcNow.AddSeconds(15)
            do {
                $cut=PageCut (Action 'status')
                if (& $predicate $cut) {return $cut}
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $until)
            throw "Page-worker state did not settle: $($cut | ConvertTo-Json -Compress)"
        }
        function BootstrapDiskCoarse {
            $until=[DateTime]::UtcNow.AddSeconds(15)
            do {
                $polled=PageCut (Action 'pollCoarse');$state=PageCut (Action 'status')
                if ($state.coarseResident) {break}
                if ($state.pageWorkState -match 'Failed|Refused|Invalid|Mismatch|Capacity') {throw 'Disk coarse transaction refused'}
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $until)
            if (!$state.coarseResident -or $state.diskReadStatus -ne 'Read' -or $state.diskRequestedRepresentation -ne 'Coarse' -or
                $state.fineResident -or $state.pageWorkState -ne 'CoarseResident') {throw 'Complete actual disk coarse result not published'}
            $null=Action 'coarse'
        }

    $diskPath=Join-Path $offline 'selected.gcd';$faultHash=$null
    if($DiskFault -ne 'None'){
     $faultDir=Join-Path $out 'disk-fault';if(Test-Path -LiteralPath $faultDir){throw 'Fault path must be fresh'};$null=New-Item -ItemType Directory $faultDir
     $diskPath=Join-Path $faultDir 'selected.gcd'
     if($DiskFault -eq 'Corrupt'){$bytes=[IO.File]::ReadAllBytes((Join-Path $offline 'selected.gcd'));if($bytes.Length -ne $originalProducer.fileBytes){throw 'Changed trusted artifact'};$bytes[0]=$bytes[0] -bxor 1;[IO.File]::WriteAllBytes($diskPath,$bytes);$faultHash=(Get-FileHash $diskPath).Hash;if($faultHash -ieq $originalProducer.fileSha256){throw 'No corruption'}}
     @{mode=$DiskFault;path=$diskPath;hash=$faultHash;trustedFileHash=$originalProducer.fileSha256}|ConvertTo-Json|Set-Content (Join-Path $out 'disk-fault-proof.json')
    }
    $baseline=Send @{cmd='stream_simulation_residency'};Require $baseline @('status','ownerPumps');if($baseline.status -ne 'Disabled' -or $baseline.ownerPumps -ne 0){throw 'Simulation unexpectedly enabled'}
    $pose=Send @{cmd='eval';code='triFreeFlyPose "1188 1200 18 90 -8"'};Require $pose @('result');if($pose.result.Trim('"') -ne 'OK'){throw 'Normalworld pose refused'}
    $until=[DateTime]::UtcNow.AddSeconds(60)
    do{$worldCoverage=Send @{cmd='stream_identity_probe';ids=@(1001,1002)};$camera=Send @{cmd='stream_residency'};Require $worldCoverage @('objects');Require $camera @('valid','pending','resident','desired','required');if(@($worldCoverage.objects).Count -ne 2){throw 'Normalworld identity count'};foreach($o in $worldCoverage.objects){Require $o @('id','present');if($o.present){Require $o @('visualResident','normalVertexBuffers')}};if(@($worldCoverage.objects|Where-Object {$_.present -and $_.visualResident -and $_.normalVertexBuffers -gt 0}).Count -eq 2 -and $camera.valid -and !$camera.pending){break};Start-Sleep -Milliseconds 250}while([DateTime]::UtcNow -lt $until)
    if($worldCoverage.objects[0].id -ne 1001 -or $worldCoverage.objects[1].id -ne 1002 -or @($worldCoverage.objects|Where-Object {$_.present -and $_.visualResident -and $_.normalVertexBuffers -gt 0}).Count -ne 2 -or !$camera.valid -or $camera.pending -or $camera.required -ne 0 -or $camera.resident -ne 2 -or $camera.desired -ne 2){throw 'Genuine normalworld two-object VB coverage missing'}
    @{objects=$worldCoverage;camera=$camera}|ConvertTo-Json -Depth 10|Set-Content (Join-Path $out 'normalworld-coverage.json')
    Capture 'baseline.png'
    $pose=Send @{cmd='eval';code='triFreeFlyPose "1184 1240 18 90 -8"'};Require $pose @('result');if($pose.result.Trim('"') -ne 'OK'){throw 'Original fixture camera refused'}
    $begin=Action 'beginOriginalDiskClodPaged'
    $fallback=PageCut (Observe $true $false)
    if(!$fallback.fallbackSelected -or $fallback.fallbackTriangles -ne 2 -or $fallback.coarseTriangles -ne 0 -or $fallback.fineTriangles -ne 0 -or $fallback.fineResident -or $fallback.coarseResident -or $fallback.meshesPresent -ne 1){throw 'Missing independently drawn original fallback'}
    Capture 'original-fallback.png'
    $refused=$null;$fine=$null;$refilled=$null;$cancelled=$null;$evicted=$null;$coarse=$null;$settledCancel=$null;$returnedFallback=$null
    if($DiskFault -ne 'None'){
     $expected=if($DiskFault -eq 'Missing'){'Missing'}else{'Invalid'}
     $until=[DateTime]::UtcNow.AddSeconds(15)
     do{$null=Action 'pollCoarse';$refused=PageCut (Action 'status');if($refused.diskReadStatus -eq $expected -and $refused.pageWorkState -eq 'Failed'){break};if($refused.coarseResident -or $refused.fineResident -or $refused.diskReadStatus -eq 'Read'){throw 'Fault published geometry'};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $until)
     if($refused.diskReadStatus -ne $expected -or $refused.pageWorkState -ne 'Failed'){throw 'No actual typed fault refusal'}
     $null=WaitPage {param($c) $c.pageLiveJobs -eq 0 -and $c.pageReservedBytes -eq 0}
     $returnedFallback=PageCut (Observe $true $false)
     if(!$returnedFallback.fallbackSelected -or $returnedFallback.rendererModel -ne $fallback.rendererModel -or $returnedFallback.rendererInstance -ne $fallback.rendererInstance -or $returnedFallback.meshCount -ne $fallback.meshCount -or $returnedFallback.meshesPresent -ne 1 -or $returnedFallback.coarseResident -or $returnedFallback.fineResident){throw 'Fault changed actual fallback/instance/history'}
     Capture 'fault-retained-original-fallback.png'
    }else{
     BootstrapDiskCoarse;$coarse=PageCut (Observe $true $false)
     if($coarse.fallbackSelected -or $coarse.rendererModel -eq $fallback.rendererModel -or $coarse.rendererInstance -ne $fallback.rendererInstance -or $coarse.coarseTriangles -ne $originalProducer.coarseTriangles){throw 'Selected coarse not committed on fixed instance'}
     Capture 'original-coarse.png'
     $null=Action 'holdFine';$heldRequest=PageCut (Action 'requestFine');if($heldRequest.pageRequest -lt 1){throw 'No held request accepted'}
     $held=WaitPage {param($c) $c.pageActive -eq 1}
     $cancelled=PageCut (Action 'cancelFine')
     if($cancelled.pageRequest -ne 0 -or $cancelled.pageActive -ne 1 -or $cancelled.pageReservedBytes -le 0 -or $cancelled.fineResident){throw 'Cancellation prematurely discarded real held debt'}
     $cancelledCoarse=PageCut (Observe $true $false)
     if($cancelledCoarse.rendererModel -ne $coarse.rendererModel -or $cancelledCoarse.rendererInstance -ne $coarse.rendererInstance){throw 'Cancel changed coarse'}
     $null=Action 'releaseFine'
     $settledCancel=WaitPage {param($c) $c.pageLiveJobs -eq 0 -and $c.pageActive -eq 0 -and $c.pageQueued -eq 0 -and $c.pageReady -eq 0 -and $c.pageReservedBytes -eq 0}
     if($settledCancel.pageCancelled -le $coarse.pageCancelled){throw 'Actual cancelled exit not counted'}
     function AdmitFine{
      $request=PageCut (Action 'requestFine');if($request.pageRequest -lt 1){throw 'Fine request refused'}
      $until=[DateTime]::UtcNow.AddSeconds(15)
      do{$null=Action 'pollFine';$state=PageCut (Action 'status');if($state.pageWorkState -match 'Failed|Refused|Invalid|Capacity'){throw 'Fine transaction refused'};if($state.fineResident){break};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $until)
      if(!$state.fineResident -or $state.diskReadStatus -ne 'Read' -or $state.diskRequestedRepresentation -ne 'Fine'){throw 'Actual fine disk result not committed'}
      return (PageCut (Observe $true $true))
     }
     $fine=AdmitFine
     if($fine.fallbackSelected -or $fine.fineGeneration -ne 1 -or $fine.fineTriangles -ne 512 -or $fine.rendererModel -eq $coarse.rendererModel -or $fine.rendererInstance -ne $coarse.rendererInstance -or $fine.producerInstance -ne $coarse.producerInstance){throw 'First actual fine cut identity mismatch'}
     Capture 'original-fine.png'
     $null=Action 'coarse';$returnedCoarse=PageCut (Observe $true $false)
     if($returnedCoarse.rendererModel -ne $coarse.rendererModel -or $returnedCoarse.rendererInstance -ne $coarse.rendererInstance -or $returnedCoarse.fallbackSelected){throw 'Coarse return lost fixed identity'}
     Capture 'original-returned-coarse.png'
     $started=PageCut (Action 'evictFine');if(!$started.fineEvictionPending -or $started.fineResident -or $started.pageWorkState -ne 'Evicting'){throw 'No transactional eviction'}
     $evicted=PageCut (Observe $true $false)
     if($evicted.fineEvictionPending -or $evicted.fineResident -or $evicted.fineEvictionCount -ne 1 -or $evicted.retiredFineCount -lt 1 -or $evicted.meshesAbsent -ne $evicted.retiredFineCount -or $evicted.meshesPresent -ne $coarse.meshCount -or $evicted.rendererModel -ne $coarse.rendererModel){throw 'Old fine not observed Absent while coarse retained'}
     $refilled=AdmitFine
     if($refilled.producerInstance -ne $coarse.producerInstance -or $refilled.fineGeneration -ne 2 -or $refilled.fineEvictionCount -ne 1 -or $refilled.meshCount -le $evicted.meshCount -or $refilled.meshesAbsent -ne $evicted.retiredFineCount -or $refilled.rendererModel -eq $fine.rendererModel -or $refilled.producerModel -eq $fine.producerModel -or $refilled.rendererInstance -ne $coarse.rendererInstance){throw 'Refill lacked distinct generation/fixed instance'}
     for($i=0;$i -lt $evicted.meshCount;$i++){if($evicted.rendererMeshHandles[$i] -ne $fine.rendererMeshHandles[$i] -or $refilled.rendererMeshHandles[$i] -ne $evicted.rendererMeshHandles[$i]){throw 'Refill replaced retained full-generation history'}}
     if(@($fallback.rendererModel,$coarse.rendererModel,$fine.rendererModel,$refilled.rendererModel|Select-Object -Unique).Count -ne 4 -or @($fallback.producerModel,$coarse.producerModel,$fine.producerModel,$refilled.producerModel|Select-Object -Unique).Count -ne 4){throw 'Four birth identities alias'}
     Capture 'original-refilled-fine.png'
     $null=Action 'fallback';$returnedFallback=PageCut (Observe $true $false)
     if(!$returnedFallback.fallbackSelected -or $returnedFallback.rendererModel -ne $fallback.rendererModel -or $returnedFallback.rendererInstance -ne $fallback.rendererInstance){throw 'Authored fallback was lost'}
     Capture 'original-returned-fallback.png'
     $secondEvict=Action 'evictFine';if($secondEvict.status -ne 'Invalid'){throw 'Second eviction exceeded one-cycle scope'}
    }
    $null=Action 'abort';$retired=PageCut (Observe $false $false)
    $settled=WaitPage {param($c) $c.pageLiveJobs -eq 0 -and $c.pageQueued -eq 0 -and $c.pageActive -eq 0 -and $c.pageReady -eq 0 -and $c.pageReservedBytes -eq 0}
    $secondBegin=Action 'beginOriginalDiskClodPaged';Require $secondBegin @('status','pageWorkState')
    if($secondBegin.status -ne 'Busy' -or $secondBegin.pageWorkState -ne 'OriginalAdmissionConsumed'){throw 'Single source admission rearmed after Abort'}
    $result=@{passed=$true;diskFault=$DiskFault;begin=$begin;baseline=$baseline;normalworldCoverage=$worldCoverage;normalworldCamera=$camera;fallback=$fallback;coarse=$coarse;fine=$fine;heldRequest=$heldRequest;held=$held;cancelled=$cancelled;cancelledCoarse=$cancelledCoarse;settledCancel=$settledCancel;evicted=$evicted;refilled=$refilled;returnedFallback=$returnedFallback;typedFault=$refused;retired=$retired;settled=$settled;secondBegin=$secondBegin;sourceFixture=$sourceFixture;producer=$originalProducer;scope='Original authored P3DM controlled subset, actual fallback/coarse/fine/refill record cuts; no retail/all-pass/pixel/FPS/device-free or complete ownership claim'}
    $null=Send @{cmd='exit'}
    if(!$p.WaitForExit(15000) -or $p.ExitCode -ne 0){throw 'Game did not exit normally'};$normal=$true
    $text=Get-Content -LiteralPath $log -Raw
    if($text -match 'Out of memory|Validation Error|panicked at|DeviceLost|UNHANDLED' -or $text -notmatch 'Shutdown complete'){throw 'Runtime error/unclean shutdown'}
    $rows=@($text -split "`r?`n"|Where-Object {$_ -match 'Original geometry startup admission status='})
    if($rows.Count -ne 1 -or $rows[0] -notmatch 'status=1 epoch=1 rawBytes=(\d+) knownRetainedBytes=(\d+) snapshotValidated=(true|1)' -or [uint64]$Matches[1] -ne $rawBytes -or [uint64]$Matches[2] -gt 131072){throw 'Startup source admission evidence mismatch'}
    $result.normalShutdown=$true;$result.forcedTermination=$false;$result.exitCode=$p.ExitCode
 # Verify original producer, source, installed artifacts remained unchanged for every mode.
 if((Get-FileHash -LiteralPath $OriginalSource).Hash -ine $rawHash -or (Get-FileHash -LiteralPath $Tools).Hash -ne $toolsHash){throw 'Original proof changed during game'}
 $finalHashes=@(Get-FileHash -LiteralPath $exe,$dll);for($i=0;$i -lt 2;$i++){if($hashes[$i].Hash -ne $finalHashes[$i].Hash){throw 'Installed artifact changed'}}
}catch{$failure=$_}
finally{
 if($p -and !$p.HasExited){try{if($client -and $client.Connected){try{$null=Action 'releaseFine';$null=Action 'abort'}catch{};$null=Send @{cmd='exit'};$normal=$p.WaitForExit(15000) -and $p.ExitCode -eq 0}}catch{};if(!$p.HasExited){Stop-Process -Id $p.Id -Force;$forced=$true;$p.WaitForExit(5000)|Out-Null}}
 if($client){$client.Dispose()}
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $old.Keys){Set-Item ('Env:'+$key) $old[$key]}
 if($failure){@{passed=$false;reason=$failure.Exception.Message;diskFault=$DiskFault;exitCode=$(if($p -and $p.HasExited){$p.ExitCode}else{$null});normalShutdown=$normal;forcedTermination=$forced;pid=$(if($p){$p.Id}else{$null})}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'failure.json')}
}
if($failure){throw $failure}
$result|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'result.json')
Write-Host $out
