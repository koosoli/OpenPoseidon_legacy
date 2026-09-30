param([switch]$RetailVisible,[switch]$RetailSource,[switch]$TextureReuseOnly,[switch]$GeneratedTextures,[switch]$HierarchyAuto,[switch]$HierarchyProjected,[switch]$DemandViews,[switch]$PaletteContentWitness,[switch]$HierarchyRefill,[switch]$HierarchyDisk,[ValidateSet('None','Scalar','Localized')][string]$HierarchyCuts='None',[switch]$Hierarchy,[ValidateSet('None','Missing','Corrupt')][string]$DiskFault='None',[switch]$CameraPrefetch,[switch]$ProjectedPrefetch,[switch]$AutoFine,[switch]$AutoCoarse,[switch]$Interactive,[switch]$CameraTuple,[switch]$CameraTupleScaleRefusal,[switch]$SurfaceCertificate,[switch]$MainCountProbe,[ValidateSet('None','DigestMismatch','Malformed')][string]$SurfaceFault='None',[string]$Tools='',
 [string]$Label='original-geometry-pages',[string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',[string]$Python='C:\Program Files\Python311\python.exe')
$ErrorActionPreference='Stop'
if($RetailVisible){$RetailSource=$true}
if($RetailSource){$TextureReuseOnly=$true}
if($TextureReuseOnly){$GeneratedTextures=$true}
if($PaletteContentWitness -and !$MainCountProbe){throw 'Palette content witness requires MainCountProbe'}
if($HierarchyRefill){$HierarchyDisk=$true;if($HierarchyCuts -ne 'None'){throw 'Hierarchy refill needs a never-used fourth model slot; choose no intermediate cut'}}
if($HierarchyAuto){$HierarchyDisk=$true;$DemandViews=$true;if($HierarchyProjected -or $HierarchyRefill -or $HierarchyCuts -ne 'None'){throw 'Automatic binary hierarchy is an independent bounded-cut mode'}}
if($HierarchyProjected){$HierarchyDisk=$true;$DemandViews=$true;if($HierarchyRefill -or $HierarchyCuts -ne 'None'){throw 'Projected hierarchy preference is an independent bounded-cut mode'}}
if($HierarchyDisk -or $HierarchyCuts -ne 'None'){$Hierarchy=$true}
if($DemandViews -and !$Hierarchy){throw 'Demand-view collection currently requires the hierarchy fixture'}
if($Hierarchy -and ($DiskFault -ne 'None' -or $CameraPrefetch -or $ProjectedPrefetch -or $AutoFine -or $AutoCoarse -or $Interactive -or $CameraTuple -or $CameraTupleScaleRefusal -or $SurfaceCertificate -or $MainCountProbe -or $SurfaceFault -ne 'None')){throw 'Hierarchy is an independent original-source page fixture'}
if($SurfaceFault -ne 'None'){$SurfaceCertificate=$true}
if($MainCountProbe){$SurfaceCertificate=$true}
if($CameraTupleScaleRefusal){$CameraTuple=$true}
if($AutoFine -and !$ProjectedPrefetch){throw 'AutoFine requires the private projected prefetch fixture'}
if($AutoCoarse -and !$AutoFine){throw 'AutoCoarse requires the private Auto-Fine fixture'}
if($Interactive -and !$AutoFine){throw 'Interactive showcase requires private Auto-Fine'}
if($ProjectedPrefetch){$CameraTuple=$true;if($CameraPrefetch -or $CameraTupleScaleRefusal){throw 'ProjectedPrefetch requires native camera tuple and is exclusive with distance prefetch'}}
if($CameraTuple){$SurfaceCertificate=$true;if($DiskFault -ne 'None' -or $SurfaceFault -ne 'None'){throw 'CameraTuple requires valid disk and surface artifacts'}}
if($CameraPrefetch -and $DiskFault -ne 'None'){throw 'CameraPrefetch requires a valid disk artifact'}
if($Label -notmatch '^[a-zA-Z0-9_-]{1,64}$'){throw 'Invalid bounded label'}
if(!$env:LOCK_OWNER){
 $shell=Join-Path $PSHOME $(if($PSVersionTable.PSEdition -eq 'Core'){'pwsh.exe'}else{'powershell.exe'})
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$shell.Replace('\','/'),'-NoProfile','-File',$PSCommandPath,'-Label',$Label,'-GameDir',$GameDir,'-Python',$Python,'-DiskFault',$DiskFault)
 if($Tools){$argv+=@('-Tools',$Tools)}
 if($Hierarchy){$argv+='-Hierarchy'}
 if($HierarchyDisk){$argv+='-HierarchyDisk'}
 if($HierarchyRefill){$argv+='-HierarchyRefill'}
 if($RetailVisible){$argv+='-RetailVisible'}
 if($RetailSource){$argv+='-RetailSource'}
 if($TextureReuseOnly){$argv+='-TextureReuseOnly'}
 if($GeneratedTextures){$argv+='-GeneratedTextures'}
 if($HierarchyAuto){$argv+='-HierarchyAuto'}
 if($HierarchyProjected){$argv+='-HierarchyProjected'}
 if($DemandViews){$argv+='-DemandViews'}
 if($PaletteContentWitness){$argv+='-PaletteContentWitness'}
 $argv+=@('-HierarchyCuts',$HierarchyCuts)
 if($CameraPrefetch){$argv+='-CameraPrefetch'}
 if($ProjectedPrefetch){$argv+='-ProjectedPrefetch'}
 if($AutoFine){$argv+='-AutoFine'}
 if($AutoCoarse){$argv+='-AutoCoarse'}
 if($Interactive){$argv+='-Interactive'}
 if($CameraTuple){$argv+='-CameraTuple'}
 if($CameraTupleScaleRefusal){$argv+='-CameraTupleScaleRefusal'}
 if($SurfaceCertificate){$argv+='-SurfaceCertificate'}
 if($MainCountProbe){$argv+='-MainCountProbe'}
 $argv+=@('-SurfaceFault',$SurfaceFault)
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
 $toolArgs=@('geometry-page-mlod','--input',('"'+$OriginalSource+'"'),'--output-directory',('"'+$offline+'"'));if($SurfaceCertificate){$toolArgs+='--surface-certificate'};if($HierarchyDisk){$toolArgs+='--hierarchy-pages'}
 $toolProcess=Start-Process -FilePath $Tools -ArgumentList $toolArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
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
if($originalProducer.selectedCut.source.sourceSha256 -ceq $rawHash -or $originalProducer.selectedCut.formatVersion -ne 1 -or $originalProducer.selectedCut.algorithmVersion -ne 2 -or $originalProducer.selectedCut.packing.clusterVertices -ne 64 -or $originalProducer.selectedCut.packing.clusterTriangles -ne 124 -or $originalProducer.selectedCut.packing.pageBytes -ne 65536 -or $originalProducer.fileBytes -lt 1 -or $originalProducer.fileBytes -gt 131072 -or $originalProducer.knownSourceBytes -lt 1 -or $originalProducer.knownSourceBytes -gt 131072 -or $originalProducer.coarseTriangles -ne 64 -or $originalProducer.fineTriangles -ne 512 -or $originalProducer.authoredFallbackTriangles -ne 2 -or $originalProducer.originalFineVertices -ne 289 -or $originalProducer.originalFineTriangles -ne 512 -or $originalProducer.coarsePages -ne 1 -or $originalProducer.finePages -ne 1 -or $originalProducer.selectedClusters -lt 1 -or $originalProducer.selectedClusters -gt 64 -or $originalProducer.scope -cne 'external-original-byte-producer; independently-verify-input-hash; no-retail/runtime/GPU/source-freshness-or-hard-peak-proof'){throw 'Selected package scope/cap mismatch'}
foreach($f in @((Join-Path $offline 'selected.gcd'))){if(!(Test-Path -LiteralPath $f) -or (Get-Item -LiteralPath $f).Length -ne $originalProducer.fileBytes -or (Get-FileHash -LiteralPath $f).Hash -ine $originalProducer.fileSha256){throw 'Selected file hash/length mismatch'}}
$hierarchyAuthority=$null
if($HierarchyDisk){
 RequireFields $originalProducer @('hierarchyPages')
 $h=$originalProducer.hierarchyPages
 RequireFields $h @('file','fileBytes','fileSha256','metadataOffset','metadataBytes','metadataSha256','diskCodecSchema','headerBytes','sVertexBytes','vertexScalarBytes','sVertexLayoutKeyHex','clodLibraryRevision','adapterVersion','sourceBytes','source','packageSha256','pageCount','pageByteLimit','groupCount','clusterCount','rootPageIds')
 if($h.file -cne 'hierarchy.ghp' -or $h.metadataOffset -ne 0 -or $h.metadataBytes -lt 212 -or $h.metadataBytes -gt 32768 -or $h.fileBytes -le $h.metadataBytes -or $h.fileBytes -gt (32768+64*65536) -or $h.diskCodecSchema -ne 1 -or $h.headerBytes -ne 212 -or $h.sVertexBytes -ne 68 -or $h.vertexScalarBytes -ne 68 -or $h.sVertexLayoutKeyHex -cne $originalProducer.sVertexLayoutKeyHex -or $h.clodLibraryRevision -cne $originalProducer.clodLibraryRevision -or $h.adapterVersion -ne 1 -or $h.sourceBytes -ne $rawBytes -or !(EqualJson $h.source $originalProducer.originalSource) -or $h.packageSha256 -notmatch '^[0-9a-f]{64}$' -or $h.pageCount -lt 2 -or $h.pageCount -gt 64 -or $h.pageByteLimit -ne 65536 -or $h.groupCount -lt 2 -or $h.groupCount -gt 64 -or $h.clusterCount -lt 1 -or $h.clusterCount -gt 64 -or !@($h.rootPageIds).Count){throw 'Hierarchy producer source/layout/metadata contract mismatch'}
 $hierarchyPath=Join-Path $offline 'hierarchy.ghp'
 if(!(Test-Path -LiteralPath $hierarchyPath -PathType Leaf) -or (Get-Item -LiteralPath $hierarchyPath).Length -ne $h.fileBytes -or (Get-FileHash -LiteralPath $hierarchyPath).Hash -ine $h.fileSha256){throw 'Hierarchy file hash/length mismatch'}
 $prefix=New-Object byte[] ([int]$h.metadataBytes)
 $stream=[IO.File]::OpenRead($hierarchyPath)
 try{$got=0;while($got -lt $prefix.Length){$n=$stream.Read($prefix,$got,$prefix.Length-$got);if(!$n){throw 'Truncated hierarchy metadata'};$got+=$n}}finally{$stream.Dispose()}
 $sha=[Security.Cryptography.SHA256]::Create()
 try{$prefixHash=[BitConverter]::ToString($sha.ComputeHash($prefix)).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
 if($prefixHash -cne $h.metadataSha256){throw 'Hierarchy metadata hash mismatch'}
 $hierarchyAuthority=@{path=$hierarchyPath;fileBytes=$h.fileBytes;metadataBytes=$h.metadataBytes;metadataSha256=$prefixHash;packageSha256=$h.packageSha256;source=$h.source;metadataHex=[BitConverter]::ToString($prefix).Replace('-','').ToLowerInvariant()}
 @{producer=$h;verifiedPath=$hierarchyPath;verifiedPrefixSha256=$prefixHash;scope='Fresh source and Tools authority verified before game startup; per-page worker verification remains required'}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'hierarchy-disk-authority.json')
}
if((Get-FileHash -LiteralPath $OriginalSource).Hash -ine $rawHash -or (Get-FileHash -LiteralPath $Tools).Hash -ne $toolsHash){throw 'Source/Tools changed during proof'}
$id=$originalProducer.originalSource
$originalKey=@([Convert]::ToUInt64($id.geometryOptionsHex,16),[Convert]::ToUInt64($id.materialOptionsHex,16),$id.producerVersion,$id.coarseRepresentation,$id.fineRepresentation,$id.vertexLayout,$id.materialMapping)-join ':'
$suppliedHash=$rawHash
$surfaceProof=$null;$surfaceExpectedStatus=0;$surfaceKnownUpper=0
if($SurfaceCertificate){
 $surfaceManifest=Join-Path $offline 'manifest.json';$surfaceDescriptor=Join-Path $offline 'surface-certificate.json'
 if(!(Test-Path -LiteralPath $surfaceDescriptor) -or (Get-Item -LiteralPath $surfaceDescriptor).Length -gt 8192){throw 'Missing bounded actual Tools surface certificate'}
 $surfaceManifestHash=(Get-FileHash -LiteralPath $surfaceManifest).Hash.ToLowerInvariant()
 $surfaceDescriptorHash=(Get-FileHash -LiteralPath $surfaceDescriptor).Hash.ToLowerInvariant()
 $surfaceDescriptorValue=Get-Content -LiteralPath $surfaceDescriptor -Raw|ConvertFrom-Json
 if($surfaceDescriptorValue.fileBytes -ne $originalProducer.fileBytes -or $surfaceDescriptorValue.fileSha256 -cne $originalProducer.fileSha256){throw 'Surface certificate does not bind fresh selected artifact'}
 $surfaceKnownUpper=[double]$surfaceDescriptorValue.hausdorffUpper
 if([double]::IsNaN($surfaceKnownUpper) -or [double]::IsInfinity($surfaceKnownUpper) -or $surfaceKnownUpper -lt 0){throw 'Invalid actual Tools selected-surface bound'}
 $surfaceExpectedStatus=1
 if($SurfaceFault -ne 'None'){
  # Keep the fresh trusted producer files intact. Fault only a private descriptor copy.
  $badSurface=Join-Path $out 'surface-fault.json'
  if($SurfaceFault -eq 'DigestMismatch'){
   [IO.File]::WriteAllBytes($badSurface,[Text.Encoding]::ASCII.GetBytes('{}'))
   $surfaceExpectedStatus=6
  }else{
   [IO.File]::WriteAllBytes($badSurface,[Text.Encoding]::ASCII.GetBytes('{"duplicate":0,"duplicate":1}'))
   $surfaceDescriptorHash=(Get-FileHash -LiteralPath $badSurface).Hash.ToLowerInvariant()
   $surfaceExpectedStatus=2
  }
  $surfaceDescriptor=$badSurface
 }
 $surfaceProof=@{manifest=$surfaceManifest;manifestSha256=$surfaceManifestHash;certificate=$surfaceDescriptor;certificateSha256=$surfaceDescriptorHash
  expectedStatus=$surfaceExpectedStatus;actualToolsDescriptor=$surfaceDescriptorValue;fault=$SurfaceFault
  scope='Fresh independently hashed actual Tools outputs; selected packed unions only, authored fallback quality remains unproved'}
 $surfaceProof|ConvertTo-Json -Depth 14|Set-Content (Join-Path $out 'surface-startup-proof.json')
}


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
$environment.WGR_GEOMETRY_PAGE_SURFACE_CERTIFICATE=$(if($SurfaceCertificate -or $RetailVisible){'1'}else{'0'})
$environment.WGR_GEOMETRY_MAIN_COUNT_PROBE=$(if($MainCountProbe){'1'}else{'0'})
$environment.WGR_GEOMETRY_PALETTE_CONTENT_WITNESS=$(if($PaletteContentWitness){'1'}else{'0'})
$environment.WGR_GEOMETRY_PAGE_DEMAND_VIEWS=$(if($DemandViews){'1'}else{'0'})
$environment.WGR_GEOMETRY_PAGE_RETAIL_SOURCE=$(if($RetailSource){'1'}else{'0'})
$environment.WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY=$(if($TextureReuseOnly){'1'}else{'0'})
$environment.WGR_OBJECT_STREAM_DAYZ_GENERATED_DEFER=$(if($GeneratedTextures){'1'}else{'0'})
$environment.WGR_GEOMETRY_PAGE_CAMERA_TUPLE=$(if($CameraTuple -or $RetailVisible){'1'}else{'0'})
$environment.WGR_GEOMETRY_PAGE_RETAIL_VISIBLE=$(if($RetailVisible){'1'}else{'0'})
$environment.WGR_GEOMETRY_PAGE_PROJECTED_PREFETCH=$(if($ProjectedPrefetch){'1'}else{'0'})
$environment.WGR_GEOMETRY_PAGE_PRIVATE_AUTO_FINE=$(if($AutoFine){'1'}else{'0'})
$environment.WGR_GEOMETRY_PAGE_PRIVATE_AUTO_COARSE=$(if($AutoCoarse){'1'}else{'0'})
if($CameraTuple -or $RetailVisible){$environment.WGR_HDR='1';$environment.WGR_RENDER_SCALE=$(if($CameraTupleScaleRefusal){'75'}else{'100'})}
if($RetailVisible){
 $environment.WGR_TEMPORAL='0';$environment.WGR_DLSS='0';$environment.WGR_ABLATE='grass,clouds,planar'
 # The renderer enables eye adaptation by default; its 0.4 s history changes
 # repeated stills as the private page alternates with the source reference.
 # Disable only for this material/geometry parity capture. The exposure scale
 # starts at neutral 1.0 and remains there while disabled.
 $environment.WGR_AUTO_EXPOSURE='0'
}
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
 if($SurfaceCertificate){$argv+=@('--geometry-pages-surface-manifest',('"'+$surfaceManifest+'"'),$surfaceManifestHash,'--geometry-pages-surface-certificate',('"'+$surfaceDescriptor+'"'),$surfaceDescriptorHash)}
 @{head=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash $PSCommandPath).Hash;generatorSha256=(Get-FileHash $generator).Hash;installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'));hashes=$hashes;environment=$environment;argv=$argv;fixture=$producer;originalProof=$originalProducer;sourceGeneratorSha256=(Get-FileHash $originalGenerator).Hash;diskFault=$DiskFault;surfaceCertificate=[bool]$SurfaceCertificate;surfaceFault=$SurfaceFault;surfaceProof=$surfaceProof;cameraPrefetch=[bool]$CameraPrefetch;projectedPrefetch=[bool]$ProjectedPrefetch;autoFine=[bool]$AutoFine;autoCoarse=[bool]$AutoCoarse;cameraTuple=[bool]$CameraTuple;cameraTupleScaleRefusal=[bool]$CameraTupleScaleRefusal;toolsSha256=$toolsHash;scope='Actual original-source private paging; copied record cuts, no retail/all-pass/pixel/FPS proof'}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'provenance.json')
 $p=Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle $(if($Interactive){'Normal'}else{'Hidden'}) -PassThru -ArgumentList $argv
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
        if($action -eq 'beginOriginalHierarchy'){$command.sourceAdmissionEpoch=1}
        if($action -eq 'beginOriginalHierarchyDisk'){$command.sourceAdmissionEpoch=1;$command.originalSourceBytes=$rawBytes;$command.hierarchyDisk=$hierarchyAuthority}
        Send $command
    }
    function Capture($name) {
        $path=Join-Path $out $name
        $null=Send @{cmd='screenshot';path=$path}
        $until=[DateTime]::UtcNow.AddSeconds(5)
        while (!(Test-Path -LiteralPath $path) -and [DateTime]::UtcNow -lt $until) {Start-Sleep -Milliseconds 100}
        if (!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0) {throw "Missing capture: $path"}
    }
    function MainCountPublished($cut) {
        if(!(Test-Path -LiteralPath $log)){return $false}
        # Observe may advance requestId without changing the selected model.
        # A prior COUNT still proves that same live generation. Producer handles
        # never recycle, and all five rows must share token, revision and cull.
        $rows=Get-Content -LiteralPath $log -Raw -ErrorAction Stop
        $pattern='Private (?<pass>main|cascade-[0-3]) GPU COUNT: token=(?<token>[1-9][0-9]*) pageEpoch='+$cut.pageEpoch+
            ' admissionEpoch='+$cut.sourceAdmissionEpoch+' request=(?<request>[1-9][0-9]*) model='+$cut.producerModel+
            ' rendererModel='+$cut.rendererModel+' sourceGeneration=(?<source>[1-9][0-9]*) cullEpoch=(?<cull>[1-9][0-9]*) state=1 count=1\b'
        $groups=@{}
        foreach($match in [regex]::Matches($rows,$pattern)){
            if([uint64]$match.Groups['request'].Value -gt [uint64]$cut.requestId){continue}
            $key=$match.Groups['token'].Value+':'+$match.Groups['request'].Value+':'+
                $match.Groups['source'].Value+':'+$match.Groups['cull'].Value
            if(!$groups.ContainsKey($key)){$groups[$key]=@{}}
            $groups[$key][$match.Groups['pass'].Value]=$true
            if($groups[$key].Count -eq 5){return $true}
        }
        return $false
    }
    function RequireMainCountPublication($cut,$label) {
        if(!$MainCountProbe){return}
        $until=[DateTime]::UtcNow.AddSeconds(3)
        for($attempt=0;$attempt -le 3;$attempt++){
            if(MainCountPublished $cut){return}
            if($attempt -eq 3 -or [DateTime]::UtcNow -ge $until){break}
            # A passive sleep does not pump this harness's renderer. Request one
            # bounded screenshot to advance a frame and poll the pending GPU map.
            Capture "$label-count-poll-$attempt.png"
        }
        throw "Main COUNT publication absent for selected model $($cut.producerModel), request <= $($cut.requestId)"
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
            # Projected owner maintenance may publish a newer complete coarse cut after
            # this explicit Observe. Accept only that active-policy supersession; all
            # other modes and manual Fine/Abort observations retain exact request identity.
            $observedSerial=$cut.observedRequestId -eq $request.requestId
            if($ProjectedPrefetch -and $cut.projectedDemandActive -and $active -and !$fine){
                $observedSerial=$cut.requestId -ge $request.requestId -and $cut.observedRequestId -eq $cut.requestId
            }
            if ($null -eq $request.requestId -or $request.requestId -lt 1 -or $cut.requestId -lt 1 -or
                !$observedSerial -or $cut.active -ne $active -or
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

            Require $cut @('surfaceCertificateStatus','surfaceCertificateAdmitted','surfaceSelectedVerified','surfaceCertificateKnownBytes','selectedSurfaceUpper','selectedUnionMinimum','selectedUnionMaximum')
            $expectSurface=$SurfaceCertificate -and $SurfaceFault -eq 'None'
            if($cut.surfaceCertificateStatus -ne $surfaceExpectedStatus -or $cut.surfaceCertificateAdmitted -ne $expectSurface){throw 'Surface admission status disagrees with independent startup proof'}
            if(!$expectSurface){
                if($cut.surfaceSelectedVerified -or $cut.surfaceCertificateKnownBytes -ne 0 -or $cut.selectedSurfaceUpper -ne 0){throw 'Rejected/OFF optional proof published numeric authority'}
            }else{
                if($cut.surfaceCertificateKnownBytes -lt 1 -or $cut.surfaceCertificateKnownBytes -gt 131072 -or $cut.selectedSurfaceUpper -ne $surfaceKnownUpper){throw 'Bound selected-surface proof accounting/value mismatch'}
                foreach($bounds in @($cut.selectedUnionMinimum,$cut.selectedUnionMaximum)){
                    if(@($bounds).Count -ne 3){throw 'Missing selected union envelope'}
                    foreach($component in $bounds){$v=[double]$component;if([double]::IsNaN($v) -or [double]::IsInfinity($v) -or [Math]::Abs($v) -gt 10000){throw 'Invalid actual selected union envelope'}}
                }
                for($axis=0;$axis -lt 3;$axis++){if($cut.selectedUnionMinimum[$axis] -gt $cut.selectedUnionMaximum[$axis]){throw 'Inverted selected union envelope'}}
                if($cut.pageWorkState -eq 'Ready' -and $cut.diskReadStatus -eq 'Read' -and !$cut.surfaceSelectedVerified){throw 'Ready selected worker result lacks exact captured-byte surface binding'}
            }
            return $cut
        }

    # Copied successful main-frame tuple + ideal projection only; no pixel/all-pass/GPU-completion claim.
    function TupleCut($c) {
     Require $c @('cameraTupleEnabled','cameraTupleValid','projectedSurfaceIdealBound','cameraTupleStatus','cameraTupleGeneration','cameraTupleIndex','cameraTupleSource','cameraTupleVersion','cameraTupleStructBytes','cameraTupleOutputWidth','cameraTupleOutputHeight','cameraTuplePosition','cameraTupleClipNear','cameraTupleProjectionXScale','cameraTupleProjectionYScale','viewportWidth','viewportHeight','cameraTupleModelBirth','cameraTupleCertificateGeneration','projectedSurfaceStatus','projectedSurfaceDepthMinimum','projectedSurfaceXUpper','projectedSurfaceYUpper','projectedSurfaceEuclideanUpper')
     if(@($c.cameraTuplePosition).Count -ne 3){throw 'Missing copied actual camera position'}
     foreach($coordinate in @($c.cameraTuplePosition)){
      if($null -eq $coordinate -or $coordinate -is [string] -or $coordinate -is [bool]){throw 'Invalid copied camera coordinate type'}
      $v=[double]$coordinate
      if([double]::IsNaN($v) -or [double]::IsInfinity($v) -or [Math]::Abs($v) -gt 1000000){throw 'Invalid copied camera coordinate'}
     }
     if(!$c.cameraTupleValid -and @($c.cameraTuplePosition | Where-Object {$_ -ne 0}).Count){throw 'Refused tuple exposed camera position'}
     foreach($field in @('cameraTupleEnabled','cameraTupleValid','projectedSurfaceIdealBound')){if($c.$field -isnot [bool]){throw "Invalid tuple Boolean $field"}}
     foreach($field in @('cameraTupleGeneration','cameraTupleModelBirth','cameraTupleCertificateGeneration','cameraTupleStatus','cameraTupleIndex','cameraTupleSource','cameraTupleVersion','cameraTupleStructBytes','cameraTupleOutputWidth','cameraTupleOutputHeight','viewportWidth','viewportHeight','projectedSurfaceStatus')){
      $raw=$c.$field;if($raw -is [string] -or $raw -is [bool]){throw "Invalid tuple integer type $field"};$v=[double]$raw
      if([double]::IsNaN($v) -or [double]::IsInfinity($v) -or $v -lt 0 -or $v -gt 9007199254740991 -or [Math]::Floor($v) -ne $v){throw "Invalid tuple integer $field"}
     }
     foreach($field in @('cameraTupleClipNear','cameraTupleProjectionXScale','cameraTupleProjectionYScale','projectedSurfaceDepthMinimum','projectedSurfaceXUpper','projectedSurfaceYUpper','projectedSurfaceEuclideanUpper')){
      $raw=$c.$field;if($raw -is [string] -or $raw -is [bool]){throw "Invalid tuple bound type $field"};$v=[double]$raw
      if([double]::IsNaN($v) -or [double]::IsInfinity($v) -or $v -lt 0 -or $v -gt 9007199254740991){throw "Invalid tuple bound $field"}
     }
     if(!$c.cameraTupleEnabled -or $c.cameraTupleVersion -ne 1 -or $c.cameraTupleStructBytes -ne 192 -or $c.cameraTupleOutputWidth -ne 1280 -or $c.cameraTupleOutputHeight -ne 720 -or $c.cameraTupleGeneration -lt 1 -or $c.cameraTupleIndex -eq 4294967295 -or $c.cameraTupleSource -notin @(1,2,3,4)){throw 'Missing actual versioned main-camera authority'}
     $w=1280;$h=720;if($CameraTupleScaleRefusal){$w=960;$h=540}
     if($c.viewportWidth -ne $w -or $c.viewportHeight -ne $h){throw 'Actual renderer dimensions disagree with explicit render-scale arm'}
     if($c.projectedSurfaceIdealBound){
      if(!$c.cameraTupleValid -or $c.cameraTupleStatus -ne 1 -or $c.projectedSurfaceStatus -ne 1 -or $c.cameraTupleModelBirth -ne (1+[double]$c.producerModel) -or $c.cameraTupleCertificateGeneration -ne $c.sourceAdmissionEpoch -or !$c.surfaceSelectedVerified){throw 'Ideal bound lacks exact source/model/certificate join'}
      if($c.cameraTupleClipNear -le 0 -or $c.cameraTupleProjectionXScale -le 0 -or $c.cameraTupleProjectionYScale -le 0 -or $c.projectedSurfaceDepthMinimum -le $c.cameraTupleClipNear){throw 'Same-frame projection limits are absent or do not enclose selected union'}
     }elseif($c.projectedSurfaceDepthMinimum -ne 0 -or $c.projectedSurfaceXUpper -ne 0 -or $c.projectedSurfaceYUpper -ne 0 -or $c.projectedSurfaceEuclideanUpper -ne 0){throw 'Refused projection exposed numeric bound'}
     if(!$c.cameraTupleValid -and ($c.cameraTupleClipNear -ne 0 -or $c.cameraTupleProjectionXScale -ne 0 -or $c.cameraTupleProjectionYScale -ne 0)){throw 'Refused tuple exposed projection packet'}
     return $c
    }
    function TuplePose($pose){$r=Send @{cmd='eval';code=('triFreeFlyPose "'+$pose+'"')};Require $r @('result');if($r.result.Trim('"') -ne 'OK'){throw 'Actual camera pose refused'}}
    function ObserveTuple($previous,$kind){
     $until=[DateTime]::UtcNow.AddSeconds(15)
     do{$c=TupleCut (PageCut (Observe $true $true));$fresh=$c.cameraTupleGeneration -gt $previous
      $ok=switch($kind){
       'Bound' {$c.cameraTupleValid -and $c.cameraTupleStatus -eq 1 -and $c.projectedSurfaceIdealBound}
       'Near' {$c.cameraTupleValid -and $c.cameraTupleStatus -eq 1 -and !$c.projectedSurfaceIdealBound -and $c.projectedSurfaceStatus -eq 5}
       'Clipped' {$c.cameraTupleValid -and $c.cameraTupleStatus -eq 1 -and !$c.projectedSurfaceIdealBound -and $c.projectedSurfaceStatus -eq 6}
       'Scaled' {!$c.cameraTupleValid -and !$c.projectedSurfaceIdealBound -and $c.cameraTupleStatus -eq 5 -and $c.projectedSurfaceStatus -eq 0}
      }
      if($fresh -and $ok){return $c};Start-Sleep -Milliseconds 100
     }while([DateTime]::UtcNow -lt $until)
     throw "Camera tuple $kind did not settle: $($c|ConvertTo-Json -Compress)"
    }
    function SameTupleModel($a,$b){
     foreach($f in @('producerModel','rendererModel','producerInstance','rendererInstance','sourceAdmissionEpoch','meshCount')){if($a.$f -ne $b.$f){throw "Camera movement changed private $f"}}
     for($i=0;$i -lt $a.meshCount;$i++){if($a.rendererMeshHandles[$i] -ne $b.rendererMeshHandles[$i]){throw 'Camera movement changed full-generation mesh history'}}
    }
    function RunCameraTupleProof($selected){
     if(!$CameraTuple){return $null}
     $scaleSetup=$null
     if($CameraTupleScaleRefusal){
      # GraphicsApply may restore native scale after the startup environment.
      # Pin the real live tuning after mission startup; keep exact dimension gates.
      $r=Send @{cmd='eval';code='triSetRenderScale 0.75'};Require $r @('result')
      if($r.result.Trim('"') -ne 'OK'){throw 'Actual live render-scale pin refused'}
      $until=[DateTime]::UtcNow.AddSeconds(15)
      do{
       $c=PageCut (Observe $true $true)
       Require $c @('cameraTupleEnabled','cameraTupleStatus','viewportWidth','viewportHeight','cameraTupleOutputWidth','cameraTupleOutputHeight')
       if($c.cameraTupleEnabled -and $c.cameraTupleStatus -eq 5 -and $c.viewportWidth -eq 960 -and $c.viewportHeight -eq 540 -and $c.cameraTupleOutputWidth -eq 1280 -and $c.cameraTupleOutputHeight -eq 720){$scaleSetup=TupleCut $c;break}
       Start-Sleep -Milliseconds 100
      }while([DateTime]::UtcNow -lt $until)
      if($null -eq $scaleSetup){throw 'Actual live scale did not reach960x540/output1280x720'}
     }
     $kind='Bound';if($CameraTupleScaleRefusal){$kind='Scaled'}
     TuplePose '1184 1240 18 90 -8';$native=ObserveTuple 0 $kind;SameTupleModel $selected $native
     TuplePose '1176 1242 19 90 -8';$moved=ObserveTuple $native.cameraTupleGeneration $kind;SameTupleModel $native $moved
     if(!$CameraTupleScaleRefusal -and $moved.projectedSurfaceEuclideanUpper -eq $native.projectedSurfaceEuclideanUpper){throw 'Movement did not change the actual ideal projection bound'}
     $near=$null;$clipped=$null
     if(!$CameraTupleScaleRefusal){
      TuplePose '1200 1240 15.5 90 0';$near=ObserveTuple $moved.cameraTupleGeneration 'Near';SameTupleModel $native $near
      TuplePose '1184 1240 18 30 -8';$clipped=ObserveTuple $near.cameraTupleGeneration 'Clipped';SameTupleModel $native $clipped
     }
     $last=$moved;if($null -ne $clipped){$last=$clipped}
     TuplePose '1184 1240 18 90 -8';$restored=ObserveTuple $last.cameraTupleGeneration $kind;SameTupleModel $native $restored
     $e=@{native=$native;moved=$moved;nearRefusal=$near;clippedRefusal=$clipped;restored=$restored;liveScaleSetup=$scaleSetup;scaleRefusal=[bool]$CameraTupleScaleRefusal;scope='Actual copied nonjittered main camera, ideal selected-union bound/refusal only; no pixels/all-pass/GPU-completion/FPS proof'}
     $e|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'camera-tuple-proof.json');return $e
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
                if ($state.coarseResident -and $state.status -eq 'Ready' -and $state.mainFrameReturned -and $state.observedRequestId -eq $state.requestId) {break}
                if ($state.pageWorkState -match 'Failed|Refused|Invalid|Mismatch|Capacity') {throw 'Disk coarse transaction refused'}
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $until)
            if (!$state.coarseResident -or $state.diskReadStatus -ne 'Read' -or $state.diskRequestedRepresentation -ne 'Coarse' -or
                $state.fineResident -or $state.pageWorkState -ne 'CoarseResident') {throw 'Complete actual disk coarse result not published'}
            # Keep pollCoarse's request current until the asynchronous COUNT maps.
            # Action coarse starts a new request and the owner rightly rejects stale rows.
            RequireMainCountPublication $state 'original-coarse-current'
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
    $retailEvidence=$null
    if($RetailSource){
     function RetailReady([string]$action){
      $until=[DateTime]::UtcNow.AddSeconds(30)
      $r=Send @{cmd='geometry_page_fixture';action=$action}
      while($r.status -eq 'Pending' -and [DateTime]::UtcNow -lt $until){Start-Sleep -Milliseconds 120;$r=Send @{cmd='geometry_page_fixture';action='pollRetailRecords'}}
      if($r.status -ne 'Ready'){ $r|ConvertTo-Json -Depth 10|Set-Content (Join-Path $out 'retail-record-failed.json');throw "Retail record action $action failed: $($r.pageWorkState)" }
      return $r
     }
     $retailEvidence=Send @{cmd='geometry_page_fixture';action='probeRetailOdol7'}
     $retailBegin=$retailEvidence
     if($retailEvidence.status -eq 'Pending'){$retailEvidence=RetailReady 'pollRetailRecords'}
     $retailEvidence|ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'retail-source-proof.json')
     if($retailEvidence.status -ne 'Ready' -or !$retailEvidence.retailSourceProbe -or !$retailEvidence.retailSourceExported -or !$retailEvidence.retailSourceOtherPassesRequired -or !$retailEvidence.retailRecordOnly -or !$retailEvidence.retailPixelSourceVerified -or $retailEvidence.retailPixelSourceSha256.Length -ne 64 -or $retailEvidence.coarseTriangles -ne 144 -or $retailEvidence.fineTriangles -ne 250 -or $retailEvidence.originalSourceBytes -ne 53786 -or $retailEvidence.retailRecordMeshesPresent -lt 1 -or $retailEvidence.retailRecordModelsPresent -lt 1 -or $retailEvidence.modelAdmissionAccepted -lt 1 -or $retailEvidence.modelAdmissionRejected -ne 0 -or $retailEvidence.modelAdmissionPending -ne 0 -or $retailEvidence.retailRecordWorkerReads -lt 1){throw 'Actual retail source/final Shape/source-pixel/page record proof failed'}
     if($retailEvidence.producerInstance -ne 4294967295 -or $retailEvidence.rendererInstance -ne 4294967295 -or $retailEvidence.mainFrameReturned){throw 'Retail record-only pilot unexpectedly claims instance/frame authority'}
     $visibleRows=@()
     if($RetailVisible){
      function VisibleReady([string]$action){
       $until=[DateTime]::UtcNow.AddSeconds(15)
       $r=Send @{cmd='geometry_page_fixture';action=$action}
       while($r.status -eq 'Pending' -and [DateTime]::UtcNow -lt $until){Start-Sleep -Milliseconds 120;$r=Send @{cmd='geometry_page_fixture';action='pollRetailVisible'}}
       if($r.status -ne 'Ready' -or !$r.retailVisiblePilot -or !$r.retailVisibleReturned -or $r.retailVisibleFrame -le 0){$r|ConvertTo-Json -Depth 10|Set-Content (Join-Path $out 'retail-visible-failed.json');throw "Retail visible $action failed: $($r.retailVisiblePhase)"}
       return $r
      }
      $v=VisibleReady 'beginRetailVisible';$visibleRows+=@{action='fallback';evidence=$v};Capture 'retail-fallback.png'
      if(!$v.retailVisiblePresent -or $v.retailVisibleSlot -ne 0 -or $v.retailVisibleReferenceTriangles -ne 250 -or !$v.retailFineTriangleSetExact){throw 'Retail exact source Fine reference was not consumed'}
      Capture 'retail-reference-repeat.png'
      $v=VisibleReady 'rootRetailVisible';$visibleRows+=@{action='root';evidence=$v};Capture 'retail-root.png'
      if(!$v.retailVisiblePresent -or $v.retailVisibleSlot -ne 1){throw 'Retail visible root was not consumed'}
      $v=VisibleReady 'fineRetailVisible';$visibleRows+=@{action='fine';evidence=$v};Capture 'retail-fine.png'
      if(!$v.retailVisiblePresent -or $v.retailVisibleSlot -ne 2){throw 'Retail visible fine was not consumed'}
      Capture 'retail-fine-repeat.png'
      $v=VisibleReady 'fallbackRetailVisible';$visibleRows+=@{action='source-fine-reference-again';evidence=$v};Capture 'retail-reference-again.png'
      $v=VisibleReady 'fineRetailVisible';$visibleRows+=@{action='fine-again';evidence=$v}
      $v=VisibleReady 'rootRetailVisible';$visibleRows+=@{action='root-before-release';evidence=$v}
     }
     $retailRelease=$null;$retailRefill=$null
     if($retailEvidence.retailRecordDistinctCuts){
      $retailRelease=RetailReady 'releaseRetailRecords'
      if($retailRelease.retailRecordMeshesAbsent -lt 1 -or $retailRelease.retailRecordMeshesPresent -lt 1){throw 'Retail detail release failed historical Absent/root retention'}
      $retailRefill=RetailReady 'refillRetailRecords'
      if($retailRefill.retailRecordFreshIds -le $retailEvidence.retailRecordFreshIds -or $retailRefill.retailRecordMeshesAbsent -lt $retailRelease.retailRecordMeshesAbsent){throw 'Retail refill failed fresh IDs/historical Absent'}
      if($RetailVisible){$v=VisibleReady 'fineRetailVisible';$visibleRows+=@{action='refill-fine';evidence=$v};Capture 'retail-refill-fine.png';if(!$v.retailVisiblePresent -or $v.retailVisibleSlot -ne 3){throw 'Retail visible refill did not use fresh model'}}
     }
     if($RetailVisible){$v=VisibleReady 'removeRetailVisible';$visibleRows+=@{action='remove';evidence=$v};Capture 'retail-removed.png';if(!$v.retailVisibleRemoved -or $v.retailVisiblePresent){throw 'Retail visible remove failed exact historical absence'};$visibleRows|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'retail-visible-lifetime.json')}
     $retailAbort=RetailReady 'abortRetailRecords'
     if($retailAbort.retailRecordMeshesPresent -ne 0 -or $retailAbort.retailRecordModelsPresent -ne 0 -or $retailAbort.retailRecordMeshesAbsent -lt 1 -or $retailAbort.pageLiveJobs -ne 0 -or $retailAbort.pageReservedBytes -ne 0){throw 'Retail abort failed all-record absence or worker debt'}
     @{begin=$retailBegin;ready=$retailEvidence;released=$retailRelease;refilled=$retailRefill;aborted=$retailAbort;scope='Actual immutable retail source and strict pixel upload plus non-instanced page/model records; no pixels/world routing/all-pass/device-free/performance acceptance'}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'retail-record-lifetime.json')
    }
    $reuseEvidence=$null
    if($TextureReuseOnly){
     $reuseEvidence=Send @{cmd='geometry_page_fixture';action='probeTextureReuseOnly'}
     if($reuseEvidence.status -ne 'Ready' -or !$reuseEvidence.textureReuseProbe -or !$reuseEvidence.textureReusePassed -or !$reuseEvidence.textureReuseHeldDenied -or !$reuseEvidence.textureReuseResidentStable -or !$reuseEvidence.textureReuseUnexpectedDenied -or !$reuseEvidence.textureReuseRetriesResident -or $reuseEvidence.textureReuseAllowed -ne 1 -or $reuseEvidence.textureReuseDenied -ne 2 -or $reuseEvidence.textureReuseOwnerCreates -ne 2){throw 'Actual reuse-only denial/retry contract failed'}
    }
    $generatedEvidence=$null
    if($GeneratedTextures){
     $generatedEvidence=Send @{cmd='geometry_generated_admission'}
     if(!$generatedEvidence.passed -or !$generatedEvidence.capturedHeld -or !$generatedEvidence.ownerResident -or !$generatedEvidence.repeatStable -or !$generatedEvidence.escapedHeld -or !$generatedEvidence.escapedResident -or !$generatedEvidence.eagerResident -or $generatedEvidence.deferred -ne 2 -or $generatedEvidence.staged -ne 1 -or $generatedEvidence.escaped -ne 1 -or $generatedEvidence.failed -ne 0 -or $generatedEvidence.pendingBytes -ne 0 -or $generatedEvidence.heldBytes -lt 1 -or $generatedEvidence.heldBytes -gt 1048576){throw 'Actual generated capture/stage/escape/default contract failed'}
    }
    if($Hierarchy){
     function HierarchyReady {
      $until=[DateTime]::UtcNow.AddSeconds(15)
      do {
       $c=Action 'status'
       if($c.status -eq 'Ready' -and $c.mainFrameReturned -and $c.observedRequestId -eq $c.requestId){return $c}
       if($c.status -notin @('Pending','Ready','Busy')){throw "Hierarchy refused: $($c|ConvertTo-Json -Compress)"}
       $null=Send @{cmd='screenshot';path=(Join-Path $out 'hierarchy-pump.png')}
      }while([DateTime]::UtcNow -lt $until)
      throw 'Hierarchy did not settle'
     }
     function HierarchyBounded($c){
      Require $c @('modelAdmissionAccepted','modelAdmissionRejected','modelAdmissionPending')
      if($c.modelAdmissionRejected -ne 0 -or ($c.modelAdmissionAccepted+$c.modelAdmissionPending) -gt 4 -or ($c.status -eq 'Ready' -and ($c.modelAdmissionPending -ne 0 -or $c.modelAdmissionAccepted -lt 1))){throw 'Selected model drain receipt was rejected, pending or absent at readiness'}
      Require $c @('hierarchicalPilot','hierarchyPages','hierarchyRootPages','hierarchyResidentPages','hierarchyPackageSha256','sourceAdmissionEpoch','knownPayloadBytes','pageReservedBytes','meshCount','hierarchySelectedClusters','hierarchySelectedTriangles')
      if(!$c.hierarchicalPilot -or $c.hierarchyPages -lt 2 -or $c.hierarchyPages -gt 64 -or $c.hierarchyRootPages -lt 1 -or $c.hierarchyRootPages -gt $c.hierarchyPages -or $c.hierarchyResidentPages -gt $c.hierarchyPages -or $c.hierarchyPackageSha256 -notmatch '^[0-9a-f]{64}$' -or $c.sourceAdmissionEpoch -ne 1 -or ($c.knownPayloadBytes+$c.pageReservedBytes) -gt 1048576 -or $c.meshCount -gt 64 -or @($c.hierarchySelectedClusters).Count -gt 64){throw 'Hierarchy source, capacity or topology contract failed'}
      if($DemandViews){
       Require $c @('demandViews');$v=$c.demandViews
       if(!$v.available -or !$v.mainPacketJoined -or $v.status -notin @(1,2) -or $v.viewCount -ne 36 -or $v.generation -lt 1 -or $v.instanceEpoch -lt 1 -or $v.required -lt 1 -or $v.required -ge 68719476736 -or ([uint64]$v.known -band [uint64]$v.unknown) -ne 0 -or ([uint64]$v.known -bor [uint64]$v.unknown) -ne [uint64]$v.required -or ([uint64]$v.known -band 1) -ne 1){throw 'Actual completed demand-view snapshot lacks exact main packet or complete configured mask'}
       $seen=[uint64]0
       foreach($row in @($v.rows)){
        if($row.id -lt 1 -or $row.id -gt 36 -or $row.generation -ne $v.generation){throw 'Demand-view row identity/generation mismatch'}
        $bit=[uint64]1 -shl ($row.id-1)
        if(($seen -band $bit) -ne 0 -or ([uint64]$v.required -band $bit) -eq 0){throw 'Duplicate or unconfigured demand-view row'}
        $seen=$seen -bor $bit
        if(([uint64]$v.known -band $bit) -ne 0 -and ($row.status -ne 1 -or $row.kind -lt 1 -or $row.kind -gt 6 -or $row.provenance -lt 1 -or $row.width -lt 1 -or $row.height -lt 1 -or $row.width -gt 16384 -or $row.height -gt 16384 -or ($row.flags -band 1) -eq 0)){throw 'Known demand-view row lacks numeric provenance/viewport'}
       }
       if($seen -ne [uint64]$v.required){throw 'Missing enabled demand-view row was omitted'}
      }
      return $c
     }
     $hierarchyBegin=if($HierarchyDisk){'beginOriginalHierarchyDisk'}else{'beginOriginalHierarchy'}
     $badDiskAuthority=$null
     if($HierarchyDisk){
      $badAuthority=$hierarchyAuthority.Clone()
      $badAuthority.metadataSha256=('0'*64)
      $badDiskAuthority=Send @{cmd='geometry_page_fixture';action=$hierarchyBegin;x=1200;y=15;z=1240;sourceAdmissionEpoch=1;originalSourceBytes=$rawBytes;hierarchyDisk=$badAuthority}
      if($badDiskAuthority.status -ne 'Invalid' -or $badDiskAuthority.active -or $badDiskAuthority.meshCount -ne 0){throw 'Invalid external metadata authority published resources'}
     }
     $begin=Action $hierarchyBegin;$fallback=HierarchyBounded (HierarchyReady)
     if(!$fallback.fallbackSelected -or $fallback.fallbackTriangles -ne 2 -or $fallback.hierarchyResidentPages -ne 0 -or $fallback.meshesPresent -ne 1){throw 'Authored fallback missing before hierarchy page loading'}
     Capture 'hierarchy-authored.png'
     $cuts=@();$steps=@();$specialDemand=$null;$rejectedDemand=$null
     $targets=@('rootHierarchy')
     if($HierarchyCuts -eq 'Scalar'){$targets+='intermediateHierarchy'}
     if($HierarchyCuts -eq 'Localized'){$targets+='localHierarchy'}
     if(!$HierarchyProjected -and !$HierarchyAuto){$targets+=@('fineHierarchy','rootHierarchy')}
     foreach($target in $targets){
      $before=HierarchyReady
      if($target -in @('intermediateHierarchy','localHierarchy')){
       Require $before @('hierarchyDemandGroupCount','hierarchyGroupErrors','hierarchyRetainedCutModels')
       $count=[int]$before.hierarchyDemandGroupCount
       if($count -lt 2 -or $count -gt 64 -or @($before.hierarchyGroupErrors).Count -ne $count){throw 'Missing bounded actual group errors'}
       # This generated chain has three non-root levels. Skip its finest level:
       # refining that leaf activates every ancestor and requests the full Fine cut.
       $finite=@($before.hierarchyGroupErrors|Where-Object {$_ -gt 0 -and $_ -lt 3.4e38}|Sort-Object -Unique)
       if($finite.Count -lt 2){throw 'Actual source lacks distinct intermediate refinement errors'}
       $command=@{cmd='geometry_page_fixture';action=$target;packageSha256=$before.hierarchyPackageSha256;sourceAdmissionEpoch=$before.sourceAdmissionEpoch;pageEpoch=$before.pageEpoch;expectedRequestId=$before.observedRequestId;groupCount=$count}
       if($target -eq 'intermediateHierarchy'){$command.threshold=$finite[1]}
       else {
        $values=@(0..($count-1)|ForEach-Object {[double]$fallback.hierarchyThreshold})
        $chosen=-1
        for($g=0;$g -lt $count;$g++){if($before.hierarchyGroupErrors[$g] -eq $finite[1]){$chosen=$g;break}}
        if($chosen -lt 0){throw 'No actual group to refine'}
        $values[$chosen]=0;$command.groupThresholds=$values
       }
       # Incorrect source token must be rejected without changing demand or loading pages.
       $bad=$command.Clone();$bad.pageEpoch=[uint64]$before.pageEpoch+1
       $rejectedDemand=Send $bad
       if($rejectedDemand.status -ne 'Invalid'){throw 'Foreign hierarchy demand was accepted'}
       $unchanged=HierarchyReady
       if($unchanged.requestId -ne $before.requestId -or $unchanged.hierarchyResidentPages -ne $before.hierarchyResidentPages -or $unchanged.producerModel -ne $before.producerModel){throw 'Refused demand changed owned state'}
       $specialDemand=$command;$accepted=Send $command
       if($accepted.status -ne 'Ready'){throw 'Valid source-bound hierarchy demand refused'}
      }else{$null=Action $target}
      for($i=0;$i -le 65;$i++){
       $prior=HierarchyReady;$step=Action 'stepHierarchy';$c=HierarchyBounded (HierarchyReady)
       if($c.hierarchyResidentPages-$prior.hierarchyResidentPages -gt 1){throw 'More than one hierarchy page uploaded by one step'}
       if($c.rendererInstance -ne $fallback.rendererInstance -or $c.producerInstance -ne $fallback.producerInstance -or $c.hierarchyPackageSha256 -cne $fallback.hierarchyPackageSha256){throw 'Hierarchy source or fixed instance changed'}
       $steps+=@{target=$target;before=$prior;after=$c}
       # Page residency alone leaves the previous complete cut selected. Wait
       # for the returned cut publication, including non-Fine intermediate cuts.
       if($c.pageWorkState -eq 'HierarchyCutResident' -and !$c.fallbackSelected -and $c.hierarchyMissingMask -eq 0 -and (($target -eq 'fineHierarchy') -eq $c.fineSelected)){break}
       if($HierarchyDisk -and $c.pageWorkState -eq 'HierarchyDiskQueued'){Start-Sleep -Milliseconds 50}
      }
      if($i -gt 65 -or $c.hierarchySelectedTriangles -lt 1 -or @($c.hierarchySelectedClusters).Count -lt 1){throw 'Complete requested hierarchy cut not published'}
      if($target -eq 'fineHierarchy' -and $c.hierarchySelectedTriangles -ne 512){throw 'Fine hierarchy lost original triangles'}
      $cuts+=,$c;Capture ('hierarchy-cut-'+$cuts.Count+'.png')
     }
     $last=$cuts.Count-1
     if($cuts[0].producerModel -ne $cuts[$last].producerModel -or $cuts[0].rendererModel -ne $cuts[$last].rendererModel -or $cuts[$last].meshCount -ne $cuts[$last-1].meshCount){throw 'Root return recreated owned resources'}
     if($HierarchyCuts -ne 'None'){
      if($cuts[1].hierarchySelectedTriangles -le $cuts[0].hierarchySelectedTriangles -or $cuts[1].hierarchySelectedTriangles -ge 512 -or $cuts[1].producerModel -eq $cuts[0].producerModel -or $cuts[1].producerModel -eq $cuts[2].producerModel -or $cuts[$last].hierarchyRetainedCutModels -ne 3){throw 'Intermediate/local frontier did not retain a distinct bounded cut'}
      $beforeFull=HierarchyReady
      $fourth=@{cmd='geometry_page_fixture';packageSha256=$beforeFull.hierarchyPackageSha256;sourceAdmissionEpoch=$beforeFull.sourceAdmissionEpoch;pageEpoch=$beforeFull.pageEpoch;expectedRequestId=$beforeFull.observedRequestId;groupCount=$count}
      if($HierarchyCuts -eq 'Localized'){$fourth.action='intermediateHierarchy';$fourth.threshold=$finite[1]}
      else{$fourth.action='localHierarchy';$values=@(0..($count-1)|ForEach-Object {[double]$fallback.hierarchyThreshold});for($g=0;$g -lt $count;$g++){if($beforeFull.hierarchyGroupErrors[$g] -eq $finite[1]){$values[$g]=0;break}};$fourth.groupThresholds=$values}
      $fullRefusal=Send $fourth
      $afterFull=HierarchyReady
      if($fullRefusal.status -ne 'Busy' -or $fullRefusal.pageWorkState -ne 'HierarchyCutSlotsFull' -or $afterFull.requestId -ne $beforeFull.requestId -or $afterFull.producerModel -ne $beforeFull.producerModel -or $afterFull.hierarchyResidentPages -ne $beforeFull.hierarchyResidentPages -or $afterFull.meshCount -ne $beforeFull.meshCount){throw 'Fourth distinct cut bypassed bounded cache or changed retained state'}
     }
     if($HierarchyDisk){
      foreach($c in @($fallback)+$cuts){Require $c @('hierarchicalDiskPilot','hierarchyDiskPageReads','diskReadStatus','pageLiveJobs','pageReservedBytes');if(!$c.hierarchicalDiskPilot -or $c.hierarchyDiskPageReads -ne $c.hierarchyResidentPages){throw 'Disk page read/publication counters disagree'}}
      if($fallback.hierarchyDiskPageReads -ne 0 -or $cuts[$last].diskReadStatus -ne 'Read' -or $cuts[$last].pageLiveJobs -ne 0 -or $cuts[$last].pageReservedBytes -ne 0){throw 'Independent disk reads did not settle without worker debt'}
     }
     $projectedCuts=@();$projectedSteps=@();$badProjected=$null
     if($HierarchyProjected){
      $previousFrame=[uint64]0
      foreach($phase in @(@{name='far';pose='1080 1240 18 90 -1.4'},@{name='near';pose='1188 1240 18 90 -8'},@{name='far-return';pose='1080 1240 18 90 -1.4'})){
       TuplePose $phase.pose;Capture ('projected-'+$phase.name+'-before.png')
       $null=Action 'observe';$before=HierarchyBounded (HierarchyReady)
       if($before.demandViews.generation -le $previousFrame){throw 'Projected demand reused an older returned camera cut'}
       $previousFrame=[uint64]$before.demandViews.generation
       $command=@{cmd='geometry_page_fixture';action='projectHierarchy';packageSha256=$before.hierarchyPackageSha256;sourceAdmissionEpoch=$before.sourceAdmissionEpoch;pageEpoch=$before.pageEpoch;expectedRequestId=$before.observedRequestId;groupCount=$before.hierarchyDemandGroupCount;frameGeneration=$before.demandViews.generation;pixelIndicatorAllowance=4}
       $bad=$command.Clone();$bad.frameGeneration=[uint64]$before.demandViews.generation+1
       $badProjected=Send $bad;$unchanged=HierarchyReady
       if($badProjected.status -ne 'Invalid' -or $unchanged.requestId -ne $before.requestId -or $unchanged.producerModel -ne $before.producerModel -or $unchanged.meshCount -ne $before.meshCount -or $unchanged.hierarchyProjectedGeneration -ne $before.hierarchyProjectedGeneration){throw 'Foreign completed-camera generation changed demand/resources'}
       $accepted=Send $command;if($accepted.status -ne 'Ready'){throw 'Actual-view hierarchy preference was refused'}
       for($i=0;$i -le 65;$i++){
        $prior=HierarchyReady;$null=Action 'stepHierarchy';$c=HierarchyBounded (HierarchyReady)
        if($c.hierarchyResidentPages-$prior.hierarchyResidentPages -gt 1 -or $c.hierarchyDiskPageReads-$prior.hierarchyDiskPageReads -gt 1 -or $c.rendererInstance -ne $fallback.rendererInstance -or $c.hierarchyPackageSha256 -cne $fallback.hierarchyPackageSha256){throw 'Projected page staging exceeded quota or changed source/instance'}
        $projectedSteps+=@{phase=$phase.name;before=$prior;after=$c}
        if($c.pageWorkState -eq 'HierarchyCutResident' -and !$c.fallbackSelected -and $c.hierarchyMissingMask -eq 0){break}
        if($c.pageWorkState -eq 'HierarchyDiskQueued'){Start-Sleep -Milliseconds 50}
       }
       if($i -gt 65 -or !$c.hierarchySourceVerticesExact -or $c.hierarchyProjectedGeneration -ne $previousFrame -or $c.hierarchyProjectedRequired -ne $before.demandViews.required -or ([uint64]$c.hierarchyProjectedOutside -band [uint64]$c.hierarchyProjectedRequired) -ne [uint64]$c.hierarchyProjectedOutside -or ([uint64]$c.hierarchyProjectedForcedFine -band [uint64]$c.hierarchyProjectedRequired) -ne [uint64]$c.hierarchyProjectedForcedFine -or $c.pageLiveJobs -ne 0 -or $c.pageReservedBytes -ne 0){throw 'Projected source/view closure or completed-cut proof failed'}
       $projectedCuts+=,$c;Capture ('projected-'+$phase.name+'-after.png')
      }
      if($projectedCuts[0].hierarchySelectedTriangles -ne $cuts[0].hierarchySelectedTriangles -or $projectedCuts[1].hierarchySelectedTriangles -le $projectedCuts[0].hierarchySelectedTriangles -or $projectedCuts[2].producerModel -ne $cuts[0].producerModel -or $projectedCuts[2].rendererModel -ne $cuts[0].rendererModel -or $projectedCuts[2].hierarchySelectedTriangles -ne $cuts[0].hierarchySelectedTriangles){throw 'Actual far/near/far views did not refine and reuse complete root'}
     }
     $autoCuts=@();$autoPolls=@();$badAuto=$null;$stoppedAuto=$null
     if($HierarchyAuto){
      TuplePose '1080 1240 18 90 -1.4';Capture 'auto-far-before.png';$null=Action 'observe';$before=HierarchyBounded (HierarchyReady)
      $follow=@{cmd='geometry_page_fixture';action='followHierarchy';packageSha256=$before.hierarchyPackageSha256;sourceAdmissionEpoch=$before.sourceAdmissionEpoch;pageEpoch=$before.pageEpoch;expectedRequestId=$before.observedRequestId;groupCount=$before.hierarchyDemandGroupCount}
      $bad=$follow.Clone();$bad.pageEpoch=[uint64]$before.pageEpoch+1;$badAuto=Send $bad
      if($badAuto.status -ne 'Invalid' -or $badAuto.hierarchyAutoEnabled){throw 'Stale automatic hierarchy binding accepted'}
      $accepted=Send $follow;if($accepted.status -ne 'Ready' -or !$accepted.hierarchyAutoEnabled){throw 'Complete-root automatic demand refused'}
      $busy=Action 'fineHierarchy';if($busy.status -ne 'Busy'){throw 'Manual cut bypassed automatic owner'}
      $previousFrame=[uint64]0
      foreach($phase in @(@{name='far';pose='1080 1240 18 90 -1.4';triangles=64;refines=0;coarsens=0},@{name='near';pose='1188 1240 18 90 -8';triangles=512;refines=1;coarsens=0},@{name='far-return';pose='1080 1240 18 90 -1.4';triangles=64;refines=1;coarsens=1},@{name='near-again';pose='1188 1240 18 90 -8';triangles=512;refines=2;coarsens=1},@{name='far-return-again';pose='1080 1240 18 90 -1.4';triangles=64;refines=2;coarsens=2})){
       TuplePose $phase.pose;Capture ('auto-'+$phase.name+'-before.png')
       $until=[DateTime]::UtcNow.AddSeconds(20)
       do {
        $c=Action 'status';$autoPolls+=@{phase=$phase.name;report=$c}
        if(!$c.hierarchyAutoEnabled -or $c.status -notin @('Ready','Pending','Busy')){throw 'Automatic hierarchy stopped or failed'}
        if($c.status -eq 'Ready' -and $c.mainFrameReturned -and $c.observedRequestId -eq $c.requestId -and !$c.hierarchyAutoPending -and $c.hierarchySelectedTriangles -eq $phase.triangles -and $c.hierarchyProjectedGeneration -gt $previousFrame -and $c.hierarchyAutoObservations -gt 0 -and $c.hierarchyAutoRefines -ge $phase.refines -and $c.hierarchyAutoCoarsens -ge $phase.coarsens){break}
        Capture 'auto-pump.png'
       }while([DateTime]::UtcNow -lt $until)
       if([DateTime]::UtcNow -ge $until){throw ('Automatic hierarchy did not reach '+$phase.name)}
       $c=HierarchyBounded $c;$previousFrame=[uint64]$c.hierarchyProjectedGeneration
       if($c.rendererInstance -ne $fallback.rendererInstance -or $c.hierarchyPackageSha256 -cne $fallback.hierarchyPackageSha256 -or $c.hierarchyRetainedCutModels -gt 2 -or !$c.hierarchySourceVerticesExact -or $c.pageLiveJobs -ne 0 -or $c.pageReservedBytes -ne 0){throw 'Automatic hierarchy violated fixed-instance/source/work bounds'}
       $autoCuts+=,$c;Capture ('auto-'+$phase.name+'-after.png')
      }
      if($autoCuts[0].producerModel -ne $cuts[0].producerModel -or $autoCuts[2].producerModel -ne $cuts[0].producerModel -or $autoCuts[4].producerModel -ne $cuts[0].producerModel -or $autoCuts[1].producerModel -ne $autoCuts[3].producerModel -or $autoCuts[1].meshCount -ne $autoCuts[4].meshCount -or $autoCuts[1].hierarchyDiskPageReads -ne $autoCuts[4].hierarchyDiskPageReads){throw 'Automatic repeat traversal recreated root/Fine resources or reread resident pages'}
      $null=Action 'stopHierarchy';$stoppedAuto=HierarchyBounded (HierarchyReady)
      if($stoppedAuto.hierarchyAutoEnabled -or $stoppedAuto.hierarchyAutoPending -or $stoppedAuto.producerModel -ne $cuts[0].producerModel){throw 'Stopping automatic interest changed the complete root'}
     }
     $badRelease=$null;$released=$null;$hierarchyRefilled=$null;$refillRoot=$null;$refillSteps=@()
     if($HierarchyRefill){
      $beforeRelease=HierarchyReady
      $release=@{cmd='geometry_page_fixture';action='releaseHierarchyNonroot';packageSha256=$beforeRelease.hierarchyPackageSha256;sourceAdmissionEpoch=$beforeRelease.sourceAdmissionEpoch;pageEpoch=$beforeRelease.pageEpoch;expectedRequestId=$beforeRelease.observedRequestId;groupCount=$beforeRelease.hierarchyDemandGroupCount}
      $bad=$release.Clone();$bad.pageEpoch=[uint64]$beforeRelease.pageEpoch+1
      $badRelease=Send $bad;$unchanged=HierarchyReady
      if($badRelease.status -ne 'Invalid' -or $unchanged.requestId -ne $beforeRelease.requestId -or $unchanged.meshCount -ne $beforeRelease.meshCount -or $unchanged.hierarchyDiskPageReads -ne $beforeRelease.hierarchyDiskPageReads -or $unchanged.hierarchyResidentMaskExact -cne $beforeRelease.hierarchyResidentMaskExact -or $unchanged.producerModel -ne $beforeRelease.producerModel){throw 'Stale release token changed owned resources'}
      $acceptedRelease=Send $release
      if($acceptedRelease.status -ne 'Pending' -or $acceptedRelease.pageWorkState -ne 'HierarchyReleasing'){throw 'Valid root release was refused'}
      $released=HierarchyBounded (HierarchyReady)
      Require $released @('hierarchyReleaseCount','hierarchyRefillCount','hierarchyRetirementPending','hierarchyRetiredPageMaskExact','expectedAbsentMeshes','observedExpectedAbsentMeshes','rendererMeshHandles')
      if($released.pageWorkState -ne 'HierarchyReleased' -or $released.hierarchyReleaseCount -ne 1 -or $released.hierarchyRefillCount -ne 0 -or $released.hierarchyRetirementPending -or $released.hierarchySelectedTriangles -ne $cuts[0].hierarchySelectedTriangles -or $released.producerModel -ne $cuts[0].producerModel -or $released.rendererModel -ne $cuts[0].rendererModel -or $released.rendererInstance -ne $fallback.rendererInstance -or $released.hierarchyResidentPages -ne $released.hierarchyRootPages -or $released.hierarchyRetainedCutModels -ne 1 -or $released.expectedAbsentMeshes -lt 1 -or $released.observedExpectedAbsentMeshes -ne $released.expectedAbsentMeshes -or $released.meshesAbsent -ne $released.expectedAbsentMeshes -or $released.hierarchyDiskPageReads -ne $beforeRelease.hierarchyDiskPageReads -or $released.pageLiveJobs -ne 0 -or $released.pageReservedBytes -ne 0){throw 'Nonroot record retirement did not preserve a complete root and clear worker debt'}
      Capture 'hierarchy-released-root.png'
      $null=Action 'fineHierarchy'
      for($i=0;$i -le 65;$i++){
       $prior=HierarchyReady;$null=Action 'stepHierarchy';$c=HierarchyBounded (HierarchyReady)
       if($c.hierarchyResidentPages-$prior.hierarchyResidentPages -gt 1 -or $c.hierarchyDiskPageReads-$prior.hierarchyDiskPageReads -gt 1 -or $c.rendererInstance -ne $fallback.rendererInstance -or $c.hierarchyPackageSha256 -cne $fallback.hierarchyPackageSha256){throw 'Refill exceeded one-page quota or changed the instance/source'}
       $refillSteps+=@{before=$prior;after=$c}
       if($c.pageWorkState -eq 'HierarchyCutResident' -and $c.fineSelected -and $c.hierarchyMissingMask -eq 0){break}
       if($c.pageWorkState -eq 'HierarchyDiskQueued'){Start-Sleep -Milliseconds 50}
      }
      $hierarchyRefilled=$c
      $newHandles=@($c.rendererMeshHandles|Where-Object {$_ -ne 0})
      $oldHandles=@($released.rendererMeshHandles|Where-Object {$_ -ne 0})
      $newOnly=@($newHandles|Where-Object {$_ -notin $oldHandles})
      $loadedPages=$c.hierarchyResidentPages-$released.hierarchyResidentPages
      if($i -gt 65 -or $loadedPages -lt 1 -or $c.hierarchySelectedTriangles -ne 512 -or $c.producerModel -eq $cuts[1].producerModel -or $c.rendererModel -eq $cuts[1].rendererModel -or $c.hierarchyReleaseCount -ne 1 -or $c.hierarchyRefillCount -ne $loadedPages -or $c.hierarchyDiskPageReads -ne ($released.hierarchyDiskPageReads+$loadedPages) -or $c.meshCount -le $released.meshCount -or $newOnly.Count -ne ($c.meshCount-$released.meshCount) -or $newHandles.Count -ne @($newHandles|Sort-Object -Unique).Count -or $c.expectedAbsentMeshes -ne $released.expectedAbsentMeshes -or $c.observedExpectedAbsentMeshes -ne $released.expectedAbsentMeshes -or $c.meshesAbsent -ne $released.expectedAbsentMeshes -or $c.pageLiveJobs -ne 0 -or $c.pageReservedBytes -ne 0){throw 'Refill did not append fresh owned IDs while retaining historical absence'}
      Capture 'hierarchy-refilled-fine.png'
      $null=Action 'rootHierarchy';$null=Action 'stepHierarchy';$refillRoot=HierarchyBounded (HierarchyReady)
      if($refillRoot.producerModel -ne $released.producerModel -or $refillRoot.rendererModel -ne $released.rendererModel -or $refillRoot.rendererInstance -ne $released.rendererInstance -or $refillRoot.hierarchySelectedTriangles -ne $released.hierarchySelectedTriangles -or $refillRoot.meshCount -ne $hierarchyRefilled.meshCount -or $refillRoot.hierarchyDiskPageReads -ne $hierarchyRefilled.hierarchyDiskPageReads){throw 'Post-refill root return recreated or lost owned state'}
     }
     $null=Action 'abort';$retired=HierarchyReady
     if($retired.active -or $retired.meshesPresent -ne 0 -or $retired.meshesAbsent -ne $retired.meshCount){throw 'Hierarchy Abort did not observe full record retirement'}
     $secondBegin=Action $hierarchyBegin
     if(!($secondBegin.status -eq 'Busy' -or ($secondBegin.status -eq 'Invalid' -and $secondBegin.pageWorkState -eq 'HierarchyAdmissionUnavailable' -and !$secondBegin.active -and $secondBegin.requestId -eq 0))){throw 'Original admission rearmed after Abort'}
     $result=@{passed=$true;hierarchy=$true;hierarchyDisk=[bool]$HierarchyDisk;hierarchyRefill=[bool]$HierarchyRefill;hierarchyCuts=$HierarchyCuts;badDiskAuthority=$badDiskAuthority;specialDemand=$specialDemand;rejectedDemand=$rejectedDemand;fullRefusal=$fullRefusal;badRelease=$badRelease;released=$released;hierarchyRefilled=$hierarchyRefilled;refillRoot=$refillRoot;refillSteps=$refillSteps;begin=$begin;fallback=$fallback;cuts=$cuts;steps=$steps;retired=$retired;secondBegin=$secondBegin;scope='Authenticated original snapshot; bounded independent pages and complete cuts on one instance, record retirement and optional fresh-ID refill; no automatic retail paging/all-view/device-free/performance proof'}
    }else{
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
    }elseif($CameraPrefetch -or $ProjectedPrefetch){
     BootstrapDiskCoarse;$coarse=PageCut (Observe $true $false)
     if($coarse.fallbackSelected -or $coarse.rendererModel -eq $fallback.rendererModel -or $coarse.rendererInstance -ne $fallback.rendererInstance -or $coarse.coarseTriangles -ne 64 -or $coarse.fineTriangles -ne 512){throw 'Camera mode coarse identity/count mismatch'}
     function CameraPose([string]$value){
      $reply=Send @{cmd='eval';code=('triFreeFlyPose "'+$value+'"')};Require $reply @('result')
      if($reply.result.Trim('"') -ne 'OK'){throw 'Actual camera refused'}
     }
     function CameraCut($cut){
      $cut=PageCut $cut
      Require $cut @('cameraDemandActive','cameraDesiredFine','cameraHasObservation','cameraDemandRevision','cameraRequestedRevision','cameraObservations','cameraRequests','cameraCancels','cameraAttempts','cameraDistance','finePrepared','preparedProducerModel')
      foreach($field in @('cameraDemandRevision','cameraRequestedRevision','cameraObservations','cameraRequests','cameraCancels','cameraAttempts')){
       $n=[double]$cut.$field
       if([double]::IsNaN($n) -or [double]::IsInfinity($n) -or $n -lt 0 -or $n -gt 9007199254740991 -or [Math]::Floor($n) -ne $n){throw "Inexact camera counter $field"}
      }
      if($ProjectedPrefetch){
       Require $cut @('projectedDemandActive','projectedDemandHasBound','projectedDemandGeneration','projectedDemandStatus','projectedDemandUpper','privateAutoFineEnabled','privateAutoFineCommitted','privateAutoFineCameraGeneration','privateAutoFineSourceRequest','privateAutoCoarseEnabled','privateAutoCoarseCommitted','privateAutoCoarseObservations','privateAutoCoarseCameraGeneration','privateAutoCoarseSourceRequest','privateAutoCoarseCertifiedUpper')
       foreach($field in @('projectedDemandActive','projectedDemandHasBound')){if($cut.$field -isnot [bool]){throw 'Untyped projected policy boolean'}}
       foreach($field in @('privateAutoFineEnabled','privateAutoFineCommitted','privateAutoCoarseEnabled','privateAutoCoarseCommitted')){if($cut.$field -isnot [bool]){throw 'Untyped private Auto-Fine boolean'}}
       foreach($field in @('projectedDemandGeneration','projectedDemandStatus','privateAutoFineCameraGeneration','privateAutoFineSourceRequest','privateAutoCoarseObservations','privateAutoCoarseCameraGeneration','privateAutoCoarseSourceRequest')){$raw=$cut.$field;if($null -eq $raw -or $raw -is [string] -or $raw -is [bool]){throw 'Invalid projected numeric type'};$v=[double]$raw;if([double]::IsNaN($v) -or [double]::IsInfinity($v) -or $v -lt 0 -or $v -gt 9007199254740991 -or [Math]::Floor($v) -ne $v){throw 'Invalid projected policy identity/status'}}
       $raw=$cut.projectedDemandUpper;if($null -eq $raw -or $raw -is [string] -or $raw -is [bool]){throw 'Invalid projected bound type'};$u=[double]$raw;if([double]::IsNaN($u) -or [double]::IsInfinity($u) -or $u -lt 0 -or (!$cut.projectedDemandHasBound -and $u -ne 0)){throw 'Invalid/refused projected policy value'}
       if($cut.cameraDemandActive){throw 'Projected and distance demand overlap'}
       $upper=[double]$cut.privateAutoCoarseCertifiedUpper
       if([double]::IsNaN($upper) -or [double]::IsInfinity($upper) -or $upper -lt 0 -or $cut.privateAutoCoarseObservations -gt 64){throw 'Invalid private Auto-Coarse witness'}
       if($cut.privateAutoFineEnabled -ne [bool]$AutoFine -or (!$AutoFine -and ($cut.privateAutoFineCommitted -or $cut.privateAutoFineCameraGeneration -ne 0 -or $cut.privateAutoFineSourceRequest -ne 0)) -or
          (!$cut.privateAutoFineCommitted -and ($cut.privateAutoFineCameraGeneration -ne 0 -or $cut.privateAutoFineSourceRequest -ne 0))){throw 'Private Auto-Fine OFF/commit record mismatch'}
       if($cut.privateAutoCoarseEnabled -ne [bool]$AutoCoarse -or (!$AutoCoarse -and ($cut.privateAutoCoarseCommitted -or $cut.privateAutoCoarseObservations -ne 0 -or $cut.privateAutoCoarseCameraGeneration -ne 0 -or $cut.privateAutoCoarseSourceRequest -ne 0 -or $upper -ne 0)) -or
          (!$cut.privateAutoCoarseCommitted -and ($cut.privateAutoCoarseCameraGeneration -ne 0 -or $cut.privateAutoCoarseSourceRequest -ne 0 -or $upper -ne 0))){throw 'Private Auto-Coarse OFF/commit record mismatch'}
      }
      $d=[double]$cut.cameraDistance
      if([double]::IsNaN($d) -or [double]::IsInfinity($d) -or $d -lt 0 -or $cut.cameraAttempts -gt 8 -or $cut.cameraRequests -gt $cut.cameraAttempts){throw 'Invalid camera policy bounds'}
      return $cut
     }
     function FixedCoarse($cut){
      if(!$cut.active -or !$cut.coarseResident -or $cut.fineSelected -or $cut.fallbackSelected -or $cut.producerModel -ne $coarse.producerModel -or $cut.rendererModel -ne $coarse.rendererModel -or $cut.producerInstance -ne $coarse.producerInstance -or $cut.rendererInstance -ne $coarse.rendererInstance){throw 'Prefetch changed selected coarse/instance'}
     }
     function WaitCamera($predicate){
      $until=[DateTime]::UtcNow.AddSeconds(15)
      do{
       $cut=CameraCut (Action 'status');FixedCoarse $cut
       if(&$predicate $cut){return $cut}
       if($cut.pageWorkState -match 'Failed|Refused|Invalid|Capacity'){throw "Automatic prefetch refused: $($cut|ConvertTo-Json -Compress)"}
       Start-Sleep -Milliseconds 100
      }while([DateTime]::UtcNow -lt $until)
      throw "Automatic camera state timed out: $($cut|ConvertTo-Json -Compress)"
     }
     if($ProjectedPrefetch){
      # Zeus clamps x/z to the loaded world on its next simulation frame. Keep x/z over
      # the fixture and use altitude for actual far distance; pitch toward the model.
      # This remains an ideal main-camera pair bound, not a pixel/visibility claim.
      CameraPose '1200 1240 100000 0 -89';$projectedFar=TupleCut (PageCut (Observe $true $false));FixedCoarse $projectedFar
      $actual=@($projectedFar.cameraTuplePosition)
      # Persist the copied packet and interval gate before any pose/threshold assertion.
      # Every value below belongs to this one returned-frame Observe, not a later camera query.
      $farPoseProof=@{requestedPose=@(1200,100000,1240);actualPosition=$actual;
       requestId=$projectedFar.requestId;observedRequestId=$projectedFar.observedRequestId;
       cameraGeneration=$projectedFar.cameraTupleGeneration;cameraStatus=$projectedFar.cameraTupleStatus;
       cameraValid=$projectedFar.cameraTupleValid;idealBoundValid=$projectedFar.projectedSurfaceIdealBound;
       mainFrameReturned=$projectedFar.mainFrameReturned;renderReturnStatus=$projectedFar.renderReturnStatus;
       viewport=@($projectedFar.viewportWidth,$projectedFar.viewportHeight);
       projectionScale=@($projectedFar.cameraTupleProjectionXScale,$projectedFar.cameraTupleProjectionYScale);
       clipNear=$projectedFar.cameraTupleClipNear;selectedUnionDepthMinimum=$projectedFar.projectedSurfaceDepthMinimum;
       selectedPairPixelUpper=$projectedFar.projectedSurfaceEuclideanUpper;projectionStatus=$projectedFar.projectedSurfaceStatus;
       scope='One actual returned main-camera frame; selected-union ideal interval bound only, not source-error, pixels, visibility or all-pass quality'}
      $farPoseProof|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'projected-far-pose-proof.json')
      if(!$projectedFar.cameraTupleValid -or [Math]::Abs([double]$actual[0]-1200) -gt 2 -or
         [Math]::Abs([double]$actual[1]-100000) -gt 2 -or [Math]::Abs([double]$actual[2]-1240) -gt 2){
       throw 'Actual rendered camera did not reach the in-world high-altitude pose'
      }
      if(!$projectedFar.projectedSurfaceIdealBound -or $projectedFar.cameraTupleGeneration -le $coarse.cameraTupleGeneration -or $projectedFar.projectedSurfaceEuclideanUpper -gt 1){throw 'Far pose did not establish actual low ideal pair discrepancy'}
      $follow=Send @{cmd='geometry_page_fixture';action='followProjected';enterUpper=2;leaveUpper=1;dwellMs=200;retryMs=250}
      $far=WaitCamera {param($c) $c.projectedDemandActive -and $c.projectedDemandHasBound -and $c.projectedDemandUpper -le 1 -and !$c.cameraDesiredFine -and $c.cameraObservations -ge 3}
     }else{
      CameraPose '1080 1240 18 90 -8'
      $follow=Send @{cmd='geometry_page_fixture';action='followCamera';nearDistance=30;farDistance=50;dwellMs=200;retryMs=250}
      $far=WaitCamera {param($c) $c.cameraDemandActive -and $c.cameraHasObservation -and $c.cameraDistance -gt 100 -and !$c.cameraDesiredFine -and $c.cameraObservations -ge 3}
     }
     if($ProjectedPrefetch){$exclusive=Send @{cmd='geometry_page_fixture';action='followCamera'};if($exclusive.status -ne 'Busy'){throw 'Distance policy overlapped projected policy'}}
     if($far.cameraRequests -ne 0 -or $far.cameraAttempts -ne 0 -or $far.pageRequest -ne 0 -or $far.fineResident -or $far.finePrepared){throw 'Far camera requested fine'}
     $null=Action 'holdFine';CameraPose '1184 1240 18 90 -8'
     # Owner NextFrame is the only source of fine requests and polls in this branch.
     $held=WaitCamera {param($c) $c.cameraHasObservation -and $(if($ProjectedPrefetch){$c.projectedDemandHasBound -and $c.projectedDemandUpper -ge 2 -and $c.projectedDemandGeneration -gt $far.projectedDemandGeneration}else{$c.cameraDistance -lt 30}) -and $c.cameraDesiredFine -and $c.cameraRequests -eq 1 -and $c.pageRequest -gt 0 -and $c.pageActive -eq 1 -and $c.pageReservedBytes -gt 0}
     if($held.fineResident -or $held.finePrepared -or $held.cameraAttempts -ne 1 -or $held.cameraRequestedRevision -ne $held.cameraDemandRevision){throw 'Held request lacks fresh automatic interest'}
     if($ProjectedPrefetch){
      CameraPose '1200 1240 15.5 90 0'
      $cancelled=WaitCamera {param($c) !$c.projectedDemandHasBound -and $c.projectedDemandStatus -eq 5 -and $c.projectedDemandGeneration -gt $held.projectedDemandGeneration -and !$c.cameraDesiredFine -and $c.cameraCancels -eq 1 -and $c.pageRequest -eq 0}
     }else{
      CameraPose '1080 1240 18 90 -8'
      $cancelled=WaitCamera {param($c) $c.cameraHasObservation -and $c.cameraDistance -gt 100 -and !$c.cameraDesiredFine -and $c.cameraCancels -eq 1 -and $c.pageRequest -eq 0}
     }
     if($cancelled.pageActive -ne 1 -or $cancelled.pageReservedBytes -le 0 -or $cancelled.fineResident -or $cancelled.finePrepared -or $cancelled.cameraRequests -ne 1){throw 'Departure discarded held debt or admitted fine'}
     $cancelledCoarse=CameraCut (Observe $true $false);FixedCoarse $cancelledCoarse
     Capture 'camera-departure-coarse.png';$null=Action 'releaseFine'
     $settledCancel=WaitCamera {param($c) $c.pageLiveJobs -eq 0 -and $c.pageQueued -eq 0 -and $c.pageActive -eq 0 -and $c.pageReady -eq 0 -and $c.pageReservedBytes -eq 0}
     if($settledCancel.pageCancelled -le $coarse.pageCancelled -or $settledCancel.cameraRequests -ne 1){throw 'Cancelled exit missing or far camera retried'}
     CameraPose '1184 1240 18 90 -8'
     $autoSelected=$null
     if($AutoFine){
      # Selection can follow the completed Prepared cut before a polling script sees it.
      # No explicit Fine action is sent in this arm; require its own returned-frame record.
      $until=[DateTime]::UtcNow.AddSeconds(15)
      do{$cut=CameraCut (Action 'status')
       if($cut.privateAutoFineCommitted -and $cut.fineSelected -and $cut.status -eq 'Ready' -and
          $cut.mainFrameReturned -and $cut.renderReturnStatus -eq 0 -and $cut.observedRequestId -eq $cut.requestId){$autoSelected=$cut;break}
       if($cut.pageWorkState -match 'Failed|Refused|Invalid|Capacity'){throw 'Private Auto-Fine refused'}
       Start-Sleep -Milliseconds 100
      }while([DateTime]::UtcNow -lt $until)
      if($null -eq $autoSelected){throw 'Private Auto-Fine did not return a completed Fine cut'}
      if($autoSelected.pageWorkState -ne 'AutoFineSelected' -or
         $autoSelected.privateAutoFineCameraGeneration -le $far.projectedDemandGeneration -or
         $autoSelected.privateAutoFineSourceRequest -lt $held.requestId -or
         $autoSelected.privateAutoFineSourceRequest -ge $autoSelected.requestId -or
         $autoSelected.cameraRequests -ne 2 -or $autoSelected.cameraAttempts -ne 2 -or
         $autoSelected.meshCount -ne (1+$originalProducer.selectedClusters) -or
         $autoSelected.meshesPresent -ne $autoSelected.meshCount -or $autoSelected.meshesAbsent -ne 0 -or
         $autoSelected.preparedProducerModel -eq 4294967295 -or
         $autoSelected.producerModel -ne $autoSelected.preparedProducerModel -or
         $autoSelected.producerInstance -ne $coarse.producerInstance -or
         $autoSelected.rendererInstance -ne $coarse.rendererInstance -or
         $autoSelected.rendererModel -eq $coarse.rendererModel -or
         $autoSelected.fineGeneration -ne 1 -or $autoSelected.pageLiveJobs -ne 0 -or
         $autoSelected.pageReservedBytes -ne 0){throw 'Private Auto-Fine lacks fresh prepared source/renderer association'}
      $automaticPrepared=$autoSelected;$prepared=$autoSelected
      Capture 'camera-auto-fine.png'
      if($Interactive){
       @{pid=$p.Id;status='Ready';scope='Visible controlled original-model GeometryPage fixture; not whole-world asset streaming';output=$out;installed=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt'))}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $out 'interactive-session.json')
       Write-Host "Interactive GeometryPage fixture ready: PID $($p.Id), evidence $out"
       $p.WaitForExit()
       @{pid=$p.Id;status='Closed';exitCode=$p.ExitCode;normalShutdown=($p.ExitCode -eq 0)}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $out 'interactive-session-closed.json')
       return
      }
     }else{
      $automaticPrepared=WaitCamera {param($c) $c.finePrepared -and $c.fineResident -and $c.status -eq 'Ready' -and $c.observedRequestId -eq $c.requestId -and $c.mainFrameReturned -and $c.renderReturnStatus -eq 0 -and $c.pageLiveJobs -eq 0 -and $c.pageReservedBytes -eq 0}
      if($automaticPrepared.cameraRequests -ne 2 -or $automaticPrepared.cameraAttempts -ne 2 -or $automaticPrepared.cameraRequestedRevision -ne $automaticPrepared.cameraDemandRevision -or $automaticPrepared.meshCount -ne (1+$originalProducer.selectedClusters) -or $automaticPrepared.meshesPresent -ne $automaticPrepared.meshCount -or $automaticPrepared.meshesAbsent -ne 0 -or $automaticPrepared.preparedProducerModel -eq 4294967295 -or $automaticPrepared.preparedProducerModel -eq $coarse.producerModel -or $automaticPrepared.fineGeneration -ne 1){throw 'Automatic fine preparation lacks complete fresh rows/identity'}
      # Preparation proof precedes the explicit Observe and manual Fine selection.
      $prepared=CameraCut (Observe $true $false);FixedCoarse $prepared
      if(!$prepared.finePrepared -or $prepared.preparedProducerModel -ne $automaticPrepared.preparedProducerModel){throw 'Prepared model changed'}
      Capture 'camera-prepared-coarse.png'
     }
     $returnedCoarse=$null;$farFineControl=$null
     if($AutoFine){
      CameraPose '1200 1240 100000 0 -89'
      if($AutoCoarse){
       $until=[DateTime]::UtcNow.AddSeconds(15)
       do{$cut=CameraCut (Action 'status')
        if($cut.privateAutoCoarseCommitted -and !$cut.fineSelected -and $cut.status -eq 'Ready' -and
           $cut.mainFrameReturned -and $cut.renderReturnStatus -eq 0 -and $cut.observedRequestId -eq $cut.requestId){$returnedCoarse=$cut;break}
        if($cut.pageWorkState -match 'Failed|Refused|Invalid|Capacity'){throw 'Private Auto-Coarse refused'}
        Start-Sleep -Milliseconds 100
       }while([DateTime]::UtcNow -lt $until)
       if($null -eq $returnedCoarse){throw 'Private Auto-Coarse did not return a completed Coarse cut'}
       if($returnedCoarse.pageWorkState -ne 'AutoCoarseSelected' -or
          $returnedCoarse.privateAutoCoarseCameraGeneration -le $autoSelected.privateAutoFineCameraGeneration -or
          $returnedCoarse.privateAutoCoarseSourceRequest -le $autoSelected.privateAutoFineSourceRequest -or
          $returnedCoarse.privateAutoCoarseSourceRequest -ge $returnedCoarse.requestId -or
          $returnedCoarse.privateAutoCoarseCertifiedUpper -gt 1 -or
          $returnedCoarse.privateAutoCoarseObservations -gt 64 -or
          $returnedCoarse.producerModel -ne $coarse.producerModel -or
          $returnedCoarse.rendererModel -ne $coarse.rendererModel -or
          $returnedCoarse.rendererInstance -ne $coarse.rendererInstance -or
          $returnedCoarse.producerInstance -ne $coarse.producerInstance -or
          $returnedCoarse.meshCount -ne $autoSelected.meshCount -or
          $returnedCoarse.meshesPresent -ne $returnedCoarse.meshCount -or $returnedCoarse.meshesAbsent -ne 0 -or
          $returnedCoarse.pageLiveJobs -ne 0 -or $returnedCoarse.pageReservedBytes -ne 0 -or
          $returnedCoarse.cameraRequests -ne 2 -or $returnedCoarse.pageCompleted -ne $autoSelected.pageCompleted){throw 'Private Auto-Coarse return lacks exact resident cut'}
       for($i=0;$i -lt $autoSelected.meshCount;$i++){if($returnedCoarse.rendererMeshHandles[$i] -ne $autoSelected.rendererMeshHandles[$i]){throw 'Auto-Coarse recreated a retained mesh'}}
       Capture 'camera-auto-coarse.png'
      }else{
       # Flag-OFF control: a fresh Fine-selected far cut cannot select Coarse.
       $farFineControl=CameraCut (Observe $true $true)
       if(!$farFineControl.fineSelected -or $farFineControl.producerModel -ne $autoSelected.producerModel -or
          $farFineControl.privateAutoCoarseCommitted -or $farFineControl.privateAutoCoarseObservations -ne 0){throw 'Auto-Coarse OFF changed selected Fine'}
      }
     }
     $stop=CameraCut (Action 'stopCamera')
     if($stop.cameraDemandActive -or ($ProjectedPrefetch -and $stop.projectedDemandActive) -or $stop.cameraDesiredFine -or !$stop.finePrepared -or !$stop.fineResident){throw 'Stop lost prepared resources'}
     if($ProjectedPrefetch){foreach($action in @('followProjected','followCamera')){$rearm=Send @{cmd='geometry_page_fixture';action=$action};if($rearm.status -ne 'Busy'){throw 'Stopped projected policy reset lifetime attempt budget'}}}
     if(!$AutoFine -or $AutoCoarse){$null=Action 'fine'}
     $fine=CameraCut (Observe $true $true)
     if($fine.producerModel -ne $prepared.preparedProducerModel -or $fine.rendererModel -eq $coarse.rendererModel -or $fine.producerInstance -ne $coarse.producerInstance -or $fine.rendererInstance -ne $coarse.rendererInstance -or $fine.meshCount -ne $prepared.meshCount -or $fine.pageCompleted -ne $prepared.pageCompleted -or $fine.cameraRequests -ne 2 -or $fine.pageLiveJobs -ne 0 -or $fine.pageReservedBytes -ne 0){throw 'Fine selection did not reuse prepared resources'}
     for($i=0;$i -lt $prepared.meshCount;$i++){if($fine.rendererMeshHandles[$i] -ne $prepared.rendererMeshHandles[$i]){throw 'Manual selection recreated prepared meshes'}}
     Capture 'camera-manual-fine.png'
     $cameraTupleEvidence=RunCameraTupleProof $fine
     $cameraEvidence=@{follow=$follow;far=$far;held=$held;cancelled=$cancelled;settledCancel=$settledCancel;automaticPrepared=$automaticPrepared;prepared=$prepared;autoSelected=$autoSelected;returnedCoarse=$returnedCoarse;farFineControl=$farFineControl;stop=$stop;manualFine=$(if(!$AutoFine -or $AutoCoarse){$fine}else{$null});finalFine=$fine;scope='Actual camera preparation and fixed-source Fine publication; no automatic quality/all-pass/pixel/FPS claim'}
    }else{
     BootstrapDiskCoarse;$coarse=PageCut (Observe $true $false)
     if($coarse.fallbackSelected -or $coarse.rendererModel -eq $fallback.rendererModel -or $coarse.rendererInstance -ne $fallback.rendererInstance -or $coarse.coarseTriangles -ne $originalProducer.coarseTriangles){throw 'Selected coarse not committed on fixed instance'}
     Capture 'original-coarse.png'
     RequireMainCountPublication $coarse 'original-coarse'
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
      do{$null=Action 'pollFine';$state=PageCut (Action 'status');if($state.pageWorkState -match 'Failed|Refused|Invalid|Capacity'){throw 'Fine transaction refused'};if($state.fineResident -and $state.status -eq 'Ready' -and $state.mainFrameReturned -and $state.observedRequestId -eq $state.requestId){break};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $until)
      if(!$state.fineResident -or $state.diskReadStatus -ne 'Read' -or $state.diskRequestedRepresentation -ne 'Fine'){throw 'Actual fine disk result not committed'}
      # Observe starts another request. Let the current Fine request publish
      # before it is superseded, for both first admission and refill.
      RequireMainCountPublication $state 'original-fine-current'
      return (PageCut (Observe $true $true))
     }
     $fine=AdmitFine
     if($fine.fallbackSelected -or $fine.fineGeneration -ne 1 -or $fine.fineTriangles -ne 512 -or $fine.rendererModel -eq $coarse.rendererModel -or $fine.rendererInstance -ne $coarse.rendererInstance -or $fine.producerInstance -ne $coarse.producerInstance){throw 'First actual fine cut identity mismatch'}
     Capture 'original-fine.png'
     RequireMainCountPublication $fine 'original-fine'
     $cameraTupleEvidence=RunCameraTupleProof $fine
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
     RequireMainCountPublication $refilled 'original-refilled-fine'
     if($CameraTuple){$finalTuple=ObserveTuple $cameraTupleEvidence.restored.cameraTupleGeneration $(if($CameraTupleScaleRefusal){'Scaled'}else{'Bound'});SameTupleModel $refilled $finalTuple;$cameraTupleEvidence.finalRefill=$finalTuple;$cameraTupleEvidence|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'camera-tuple-proof.json')}
     $null=Action 'fallback';$returnedFallback=PageCut (Observe $true $false)
     if(!$returnedFallback.fallbackSelected -or $returnedFallback.rendererModel -ne $fallback.rendererModel -or $returnedFallback.rendererInstance -ne $fallback.rendererInstance){throw 'Authored fallback was lost'}
     Capture 'original-returned-fallback.png'
     $secondEvict=Action 'evictFine';if($secondEvict.status -ne 'Invalid'){throw 'Second eviction exceeded one-cycle scope'}
    }
    $null=Action 'abort';$retired=PageCut (Observe $false $false)
    $settled=WaitPage {param($c) $c.pageLiveJobs -eq 0 -and $c.pageQueued -eq 0 -and $c.pageActive -eq 0 -and $c.pageReady -eq 0 -and $c.pageReservedBytes -eq 0}
    if($CameraPrefetch -or $ProjectedPrefetch){$retired=CameraCut $retired;$settled=CameraCut $settled;if($retired.cameraDemandActive -or ($ProjectedPrefetch -and $retired.projectedDemandActive) -or $retired.finePrepared -or $retired.meshCount -ne $fine.meshCount -or $retired.meshesAbsent -ne $fine.meshCount -or $settled.cameraRequests -ne 2){throw 'Camera Abort failed exact cleanup/idle proof'}}
    if($CameraTuple){Require $retired @('cameraTupleValid','projectedSurfaceIdealBound');if($retired.cameraTupleValid -or $retired.projectedSurfaceIdealBound){throw 'Abort retained camera projection authority'}}
    $secondBegin=Action 'beginOriginalDiskClodPaged';Require $secondBegin @('status','pageWorkState')
    if($secondBegin.status -ne 'Busy' -or $secondBegin.pageWorkState -ne 'OriginalAdmissionConsumed'){throw 'Single source admission rearmed after Abort'}
    $result=@{passed=$true;surfaceCertificate=[bool]$SurfaceCertificate;surfaceFault=$SurfaceFault;surfaceProof=$surfaceProof;diskFault=$DiskFault;cameraPrefetch=[bool]$CameraPrefetch;projectedPrefetch=[bool]$ProjectedPrefetch;autoFine=[bool]$AutoFine;autoCoarse=[bool]$AutoCoarse;cameraTuple=[bool]$CameraTuple;cameraTupleScaleRefusal=[bool]$CameraTupleScaleRefusal;cameraEvidence=$cameraEvidence;cameraTupleEvidence=$cameraTupleEvidence;begin=$begin;baseline=$baseline;normalworldCoverage=$worldCoverage;normalworldCamera=$camera;fallback=$fallback;coarse=$coarse;fine=$fine;heldRequest=$heldRequest;held=$held;cancelled=$cancelled;cancelledCoarse=$cancelledCoarse;settledCancel=$settledCancel;evicted=$evicted;refilled=$refilled;returnedFallback=$returnedFallback;typedFault=$refused;retired=$retired;settled=$settled;secondBegin=$secondBegin;sourceFixture=$sourceFixture;producer=$originalProducer;scope='Original authored P3DM controlled subset, actual fallback/coarse/fine/refill record cuts; no retail/all-pass/pixel/FPS/device-free or complete ownership claim'}
    if($CameraPrefetch -or $ProjectedPrefetch){$result.scope='Actual original-file camera prefetch/held departure/complete prepared rows/coarse unchanged/manual Fine reuse/all-Absent cleanup; no eviction/refill/automatic quality/retail/all-pass/pixel/FPS/device-free/complete ownership claim'}
    if($ProjectedPrefetch){$cameraEvidence.projectedFar=$projectedFar;$cameraEvidence.scope=$(if($AutoCoarse){'One private Fine update and one source-bound far-view Coarse return on the same instance; no eviction/source-error/pixel/all-pass/FPS proof'}elseif($AutoFine){'One private prepared-Fine instance update after exact completed source/camera/mesh cut; no automatic coarse, source-error/pixel/all-pass/FPS proof'}else{'Actual ideal pair-discrepancy preparation preference, refusal/cancellation debt, fixed coarse/manual Fine reuse; no source-error/pixel/automatic quality/all-pass/FPS proof'});$result.scope=$cameraEvidence.scope}
    }
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
$result.demandViewsRequested=[bool]$DemandViews
if($RetailSource){$result.retailSource=$true;$result.retailSourceEvidence=$retailEvidence}
if($TextureReuseOnly){$result.textureReuseOnly=$true;$result.textureReuseEvidence=$reuseEvidence}
if($GeneratedTextures){$result.generatedTextures=$true;$result.generatedEvidence=$generatedEvidence}
if($HierarchyProjected){$result.hierarchyProjected=$true;$result.projectedCuts=$projectedCuts;$result.projectedSteps=$projectedSteps;$result.badProjected=$badProjected}
if($HierarchyAuto){$result.hierarchyAuto=$true;$result.autoCuts=$autoCuts;$result.autoPolls=$autoPolls;$result.badAuto=$badAuto;$result.stoppedAuto=$stoppedAuto}
$result|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'result.json')
Write-Host $out
