param(
 [switch]$Records,
 [switch]$Visible,
 [switch]$Auto,
 [switch]$Residency,
 [ValidateSet('SkalaNew','Skala2')][string]$Asset='SkalaNew',
 [ValidateSet('None','Move','Remove')][string]$Invalidate='None',
 [string]$Label='retail-cold-world-source',
 [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
 [string]$Python='C:\Program Files\Python311\python.exe'
)
$ErrorActionPreference='Stop'
$Asset=if($Asset -eq 'Skala2'){'Skala2'}else{'SkalaNew'}
if($Residency -and $Invalidate -ne 'None'){throw 'Residency and actor invalidation are separate installed cycles'}
if($Residency){$Auto=$true}
if($Auto){$Visible=$true}
if($Invalidate -ne 'None'){$Visible=$true}
if($Visible){$Records=$true}
if($Label -notmatch '^[a-zA-Z0-9_-]{1,64}$'){throw 'Invalid bounded label'}
if(!$env:LOCK_OWNER){
 $shell=Join-Path $PSHOME $(if($PSVersionTable.PSEdition -eq 'Core'){'pwsh.exe'}else{'powershell.exe'})
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$shell.Replace('\','/'),'-NoProfile','-File',$PSCommandPath,
 '-Label',$Label,'-GameDir',$GameDir,'-Python',$Python)
 $argv+=@('-Asset',$Asset)
 if($Records){$argv+='-Records'}
 if($Visible){$argv+='-Visible'}
 if($Auto){$argv+='-Auto'}
 if($Residency){$argv+='-Residency'}
 if($Invalidate -ne 'None'){$argv+=@('-Invalidate',$Invalidate)}
 $env:LOCK_OWNER='retail cold world source fixture'
 try{& 'C:\Program Files\Git\bin\bash.exe' @argv;if($LASTEXITCODE){throw "Retail cold world child failed $LASTEXITCODE"}}
 finally{Remove-Item Env:LOCK_OWNER}
 return
}
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Game already running'}
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root ('build/stream-residency/'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N').Substring(0,6))
$fixture=Join-Path $out 'fixture'
$profile=Join-Path $out 'user'
$mission=Join-Path $out 'retail-cold.eden'
$exe=Join-Path $GameDir 'OpenPoseidon.exe'
$dll=Join-Path $GameDir 'wgpu_renderer.dll'
$stampPath=Join-Path $GameDir 'DEPLOYED-FROM.txt'
$log=Join-Path $out 'engine.log'
$generator=Join-Path $PSScriptRoot 'streaming/build_retail_cold_world_fixture.py'
$helper=Join-Path $PSScriptRoot 'streaming/build_simulation_residency_fixture.py'
$assetId=if($Asset -eq 'Skala2'){'skala2'}else{'skala_new'}
$modelPath=if($Asset -eq 'Skala2'){'data3d\skala2.p3d'}else{'data3d\skala_new.p3d'}
$expectedFineTriangles=if($Asset -eq 'Skala2'){77}else{250}
New-Item -ItemType Directory $out,$profile,$mission | Out-Null
$old=@{};Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {$old[$_.Name]=$_.Value}
$p=$null;$client=$null;$normal=$false;$forced=$false;$failure=$null;$result=$null;$visibleTrace=$null;$autoTrace=$null
try{
 if(!(Test-Path -LiteralPath $exe) -or !(Test-Path -LiteralPath $dll) -or !(Test-Path -LiteralPath $stampPath)){
  throw 'Installed game or deployment stamp missing'
 }
 $initialHashes=@(Get-FileHash -LiteralPath $exe,$dll)
 $installed=Get-Content -LiteralPath $stampPath -Raw
 $generated=@(& $Python $generator $fixture --asset $assetId --game-root $GameDir)
 if($LASTEXITCODE -ne 0 -or $generated.Count -ne 1 -or $generated[0].Length -gt 8192){throw 'Bounded retail WRP generation failed'}
 $proof=$generated[0]|ConvertFrom-Json
 $world=Join-Path $fixture 'retail-cold.wrp'
 if($proof.schema -ne 1 -or $proof.asset -cne $assetId -or $proof.modelPath -cne $modelPath -or
    $proof.modelCount -ne 1 -or $proof.landSide -ne 64 -or $proof.cellMeters -ne 50 -or
    @($proof.placements).Count -ne 2 -or
    $proof.worldBytes -le 0 -or $proof.worldBytes -gt 131072 -or
    $proof.worldSha256 -cne (Get-FileHash -LiteralPath $world).Hash.ToLowerInvariant()){
  throw 'Retail WRP self-check/provenance mismatch'
 }
 $bankProof=$proof.installedBankEvidence
 $expectedModelMember=if($Asset -eq 'Skala2'){'skala2.p3d'}else{'skala_new.p3d'}
 $expectedPacMember=if($Asset -eq 'Skala2'){'piskovec.pac'}else{'skala_piskovec2.pac'}
 if(!$bankProof.sourceEvidenceOnly -or $bankProof.liveMountedAuthority -or
    $bankProof.model.member -cne $expectedModelMember -or
    $bankProof.texture.member -cne $expectedPacMember -or
    $bankProof.model.archive -ine (Join-Path $GameDir 'DTA\data3d.pbo') -or
    $bankProof.texture.archive -ine (Join-Path $GameDir 'DTA\data.pbo') -or
    $bankProof.model.decodedSha256 -cnotmatch '^[0-9a-f]{64}$' -or
    $bankProof.texture.rawSha256 -cnotmatch '^[0-9a-f]{64}$'){
  throw 'Independent installed archive/member evidence mismatch'
 }
 $files=@(Get-ChildItem -LiteralPath $fixture -Recurse -File)
 if($files.Count -ne 2 -or @($files|Where-Object {$_.Extension -in @('.p3d','.pac','.paa','.pbo')}).Count){
  throw 'Retail fixture unexpectedly generated an asset override'
 }
 @'
version=11;
class Mission {
 randomSeed=1234;
 class Intel { year=1985; month=6; day=21; hour=12; minute=0; };
 class Groups { items=1; class Item0 { side="WEST"; class Vehicles { items=1; class Item0 {
  position[]={100,0,100}; id=0; side="WEST"; vehicle="SoldierWB";
  player="PLAYER COMMANDER"; leader=1; skill=1;
  init="this setBehaviour ""CARELESS""; this setCombatMode ""BLUE""; this disableAI ""MOVE""";
 }; }; }; };
};
class Intro { randomSeed=1; class Intel {}; };
class OutroWin { randomSeed=2; class Intel {}; };
class OutroLoose { randomSeed=3; class Intel {}; };
'@|Set-Content (Join-Path $mission 'mission.sqm')
 [IO.File]::WriteAllText((Join-Path $profile 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
 # A fresh process plus no model DDC forces the ordinary world/TextureBank path.
 # No private geometry action, addon root, substituted model, or render-path ablation.
 $environment=@{
  POSEIDON_USER_DIR=$profile;POSEIDON_MODEL_DDC='0'
  WGR_GEOMETRY_PAGE_RETAIL_COLD_SOURCE='1';WGR_GEOMETRY_PAGE_RETAIL_SOURCE='1'
  WGR_GEOMETRY_PAGE_RETAIL_ASSET=$assetId
 }
 if($Records){$environment.WGR_GEOMETRY_PAGE_FIXTURE='1';$environment.WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY='1'}
 if($Visible){
  $environment.WGR_GEOMETRY_PAGE_RETAIL_WORLD_VISIBLE='1'
  $environment.WGR_GEOMETRY_PAGE_CAMERA_TUPLE='1'
  $environment.WGR_HDR='1';$environment.WGR_RENDER_SCALE='100'
  $environment.WGR_TEMPORAL='0';$environment.WGR_DLSS='0';$environment.WGR_AUTO_EXPOSURE='0'
 }
 if($Auto){$environment.WGR_GEOMETRY_PAGE_RETAIL_WORLD_AUTO='1'}
 if($Residency){$environment.WGR_GEOMETRY_PAGE_RETAIL_WORLD_RESIDENCY='1'}
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $environment.Keys){Set-Item ('Env:'+$key) $environment[$key]}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
 $listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','2000',
  '--harness',[string]$port,'--test-world-freefly','1184','1200','18','90','-8',
  '--test-world',('"'+$world+'"'),'--test-mission',('"'+$mission+'"'),'--log-file',('"'+$log+'"'))
 @{head=(& git -C $root rev-parse HEAD);scriptSha256=(Get-FileHash $PSCommandPath).Hash;
   generatorSha256=(Get-FileHash $generator).Hash;worldHelperSha256=(Get-FileHash $helper).Hash;
   installed=$installed;hashes=$initialHashes;environment=$environment;argv=$argv;fixture=$proof;
   asset=$Asset;expectedFineTriangles=$expectedFineTriangles;
   scope=$(if($Residency){'Opt-in one-placement certified Root-only retirement, returned conventional original fallback, bounded one-page refill and fresh Fine takeover; no pixel fidelity, all-pass or GPU-completion proof'}elseif($Auto){'Opt-in one-placement original/page transaction with certified selected object-space unions and actual main-view projected demand; no pixel fidelity, other-view coverage or GPU-completion proof'}elseif($Invalidate -ne 'None'){'Opt-in one-placement world/page transaction followed by ordinary moved/removed actor invalidation and returned page absence; no visual parity or GPU-completion proof'}elseif($Visible){'Opt-in one-placement main-view world/page transaction with exact CPU rows and returned main camera; screenshots are comparison evidence, not GPU completion or all-pass proof'}else{'Actual installed-archive cold-world Shape/primary source and ordinary queued placement only; no renderer-return, takeover, complete model, or visual parity proof'})}|
  ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'provenance.json')
 $overall=[DateTime]::UtcNow.AddSeconds($(if($Residency){300}elseif($Auto){240}elseif($Visible){220}else{115}))
 $p=Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru -ArgumentList $argv
 $null=$p.Handle
 $client=[Net.Sockets.TcpClient]::new()
 $connectDeadline=[DateTime]::UtcNow.AddSeconds(85)
 while(!$client.Connected){
  try{$client.Connect('127.0.0.1',$port)}
  catch{if($p.HasExited -or [DateTime]::UtcNow -gt $connectDeadline -or [DateTime]::UtcNow -gt $overall){throw 'Retail cold world harness startup failed'};Start-Sleep -Milliseconds 250}
 }
 $stream=$client.GetStream();$stream.ReadTimeout=10000
 $reader=[IO.StreamReader]::new($stream)
 $writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command){
  $writer.WriteLine(($command|ConvertTo-Json -Depth 6 -Compress))
  do{$line=$reader.ReadLine();if($null -eq $line){throw 'Harness closed'};$reply=$line|ConvertFrom-Json}while($null -eq $reply.ok)
  ($command|ConvertTo-Json -Depth 6 -Compress)|Add-Content (Join-Path $out 'harness.jsonl')
  $line|Add-Content (Join-Path $out 'harness.jsonl')
  if(!$reply.ok){throw $line};return $reply
 }
 $null=Send @{cmd='eval';code='triFreeFlyPose "1184 1200 18 90 -8"'}
 $image=Join-Path $out 'retail-cold-world.png'
 $null=Send @{cmd='screenshot';path=$image}
 $imageDeadline=[DateTime]::UtcNow.AddSeconds(10)
 while([DateTime]::UtcNow -lt $imageDeadline){
  if((Test-Path -LiteralPath $image) -and (Get-Item -LiteralPath $image).Length -gt 0){break}
  Start-Sleep -Milliseconds 100
 }
 if(!(Test-Path -LiteralPath $image) -or (Get-Item -LiteralPath $image).Length -le 0){throw 'Missing actual-world screenshot'}
 $sourceMatch=$null;$placementMatch=$null
 $expectedVisualLevels=if($Asset -eq 'Skala2'){3}else{4}
 $sourcePattern='Retail actual-world source: shapeBirth=(?<birth>[1-9][0-9]*) sourceSha256=(?<source>[0-9a-f]{64}) decodedBytes=(?<decoded>[1-9][0-9]*) visualLevels='+$expectedVisualLevels+' primaryHandle=(?<handle>[1-9][0-9]*) primarySlot=(?<slot>[1-9][0-9]*) rawSha256=(?<raw>[0-9a-f]{64}) uploadedSha256=(?<upload>[0-9a-f]{64})'
 do{
  if($p.HasExited){throw 'Game exited before actual-world source/placement witness'}
  $text=if(Test-Path -LiteralPath $log){Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue}else{''}
  if($text){
   $sourceRefusal=[regex]::Match($text,
    'Retail actual-world source refused: reason=(?<reason>[A-Za-z0-9_]+) shapeBirth=[1-9][0-9]* scope=ordinary-world-kept')
   if($sourceRefusal.Success){throw ('Actual selected retail source refused: '+$sourceRefusal.Groups['reason'].Value)}
   $sourceMatch=[regex]::Match($text,$sourcePattern)
   if($sourceMatch.Success){
    $placementPattern='Retail actual-world placement queued: shapeBirth='+$sourceMatch.Groups['birth'].Value+
     ' placementObservation=[1-9][0-9]* producerInstance=[0-9]+ producerModel=[0-9]+ sourceCurrent=(true|1) static=(true|1) intact=(true|1) flags=[0-9]+'
    $placementMatch=[regex]::Match($text,$placementPattern)
   }
  }
  if($sourceMatch -and $sourceMatch.Success -and $placementMatch -and $placementMatch.Success){break}
  Start-Sleep -Milliseconds 500
 }while([DateTime]::UtcNow -lt $overall.AddSeconds(-15))
 if(!$sourceMatch -or !$sourceMatch.Success -or !$placementMatch -or !$placementMatch.Success){
  throw 'No exact cold-world Shape/PAC source plus current queued placement witness'
 }
 if($sourceMatch.Groups['source'].Value -cne $bankProof.model.decodedSha256 -or
    $sourceMatch.Groups['raw'].Value -cne $bankProof.texture.rawSha256 -or
    [uint64]$sourceMatch.Groups['decoded'].Value -ne [uint64]$bankProof.model.decodedBytes){
  throw 'Live source/PAC witness differs from independent installed bank member SHA'
 }
 if($Records){
  function RecordAction($action){return Send @{cmd='geometry_page_fixture';action=$action}}
  function WaitRecord($first,$poll){
   $recordDeadline=[DateTime]::UtcNow.AddSeconds(25);$record=$first
   do{
    if($record.status -eq 'Ready'){return $record}
    if($record.status -ne 'Pending'){throw ('Record refused: '+($record|ConvertTo-Json -Depth 3 -Compress))}
    Start-Sleep -Milliseconds 50;$record=RecordAction $poll
   }while([DateTime]::UtcNow -lt $recordDeadline -and [DateTime]::UtcNow -lt $overall)
   throw 'Bounded world record observation timed out'
  }
  $ready=WaitRecord (RecordAction 'probeRetailColdWorld') 'pollRetailRecords'
  if($null -eq $ready.PSObject.Properties['retailDiagnosticReferenceAllocated']){throw 'Missing private-reference ownership report'}
  $referenceCount=if($ready.retailDiagnosticReferenceAllocated){1}else{0}
  if(($Visible -and $referenceCount -ne 0) -or (!$Visible -and $referenceCount -ne 1)){throw 'Unexpected diagnostic reference ownership for selected mode'}
  $initialModelCount=2+$referenceCount
  if(!$ready.retailRecordOnly -or !$ready.retailFineTriangleSetExact -or
     [uint32]$ready.fineTriangles -ne $expectedFineTriangles -or
     [uint32]$ready.coarseTriangles -eq 0 -or
     [uint32]$ready.coarseTriangles -ge $expectedFineTriangles -or
     [uint32]$ready.retailRecordMeshesPresent -lt 3 -or
     [uint32]$ready.retailRecordMeshesPresent -ne [uint32]$ready.meshCount -or
     [uint32]$ready.retailRecordMeshesAbsent -ne 0 -or
     $ready.retailRecordModelsPresent -ne $initialModelCount -or $ready.retailRecordModelsAbsent -ne 0 -or
     $ready.sourceSha256 -cne $bankProof.model.decodedSha256 -or
     $ready.retailPixelSourceSha256 -cne $bankProof.texture.rawSha256 -or
     $ready.mainFrameReturned -or $ready.producerInstance -ne 4294967295){throw 'Initial actual-world records mismatch'}
  if($Visible){
   # Screenshots are comparison artifacts only. The CPU row/returned-camera
   # receipt supplies transition evidence; image hashes are not pixel parity.
   function SaveVisible {$visibleTrace|ConvertTo-Json -Depth 18|Set-Content (Join-Path $out 'actual-world-visible.json')}
   function CaptureVisible($name){
    $path=Join-Path $out $name
    if(Test-Path -LiteralPath $path){throw "Visible capture already exists: $name"}
    $null=Send @{cmd='screenshot';path=$path}
    $captureDeadline=[DateTime]::UtcNow.AddSeconds(10)
    $lastLength=-1;$stable=0
    while([DateTime]::UtcNow -lt $captureDeadline -and [DateTime]::UtcNow -lt $overall){
     if(Test-Path -LiteralPath $path){
      $length=(Get-Item -LiteralPath $path).Length
      if($length -gt 0 -and $length -eq $lastLength){$stable++}else{$stable=0}
      if($stable -ge 2){break};$lastLength=$length
     }
     Start-Sleep -Milliseconds 100
    }
    if(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0 -or $stable -lt 2){
     throw "Missing or incomplete actual-world visible capture: $name"
    }
    return @{path=$path;bytes=(Get-Item -LiteralPath $path).Length;
      sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();capturedAtUtc=[DateTime]::UtcNow.ToString('o')}
   }
   function WorldAction($action){return Send @{cmd='geometry_page_fixture';action=$action}}
   function WaitWorld($first,$phase){
    $until=[DateTime]::UtcNow.AddSeconds(30);$report=$first
    do{
     if($report.status -eq 'Ready'){
      if($report.retailWorldPhase -cne $phase -or !$report.retailWorldReturned){
       throw ('World transition returned wrong phase/facts: '+($report|ConvertTo-Json -Depth 4 -Compress))
      }
      return $report
     }
     if($report.status -ne 'Pending'){throw ('World transition refused: '+($report|ConvertTo-Json -Depth 4 -Compress))}
     Start-Sleep -Milliseconds 50;$report=WorldAction 'pollRetailWorldVisible'
    }while([DateTime]::UtcNow -lt $until -and [DateTime]::UtcNow -lt $overall)
    throw "Bounded world visible transition timed out: $phase"
   }
   function WaitInvalidatedWorld($first){
    $until=[DateTime]::UtcNow.AddSeconds(30);$report=$first
    do{
     if($report.retailWorldPhase -ceq 'InvalidatedPageAbsent'){return $report}
     if($report.status -ne 'Pending' -and $report.status -ne 'Failed'){
      throw ('World invalidation refused: '+($report|ConvertTo-Json -Depth 4 -Compress))
     }
     Start-Sleep -Milliseconds 50;$report=WorldAction 'pollRetailWorldVisible'
    }while([DateTime]::UtcNow -lt $until -and [DateTime]::UtcNow -lt $overall)
    throw 'Bounded actual-world invalidation cleanup timed out'
   }
   function CheckWorld($report,$phase,$other,$page,$restored,$previousCamera,$previousRequest){
    foreach($field in @('retailWorldVisiblePilot','retailWorldOriginalOtherViews','retailWorldPagePresent',
      'retailWorldRestored','retailWorldReturned','retailWorldPhase','retailWorldRequest',
      'retailWorldPageBirth','retailWorldCameraGeneration','retailWorldOriginalInstance',
      'retailWorldPageInstance','retailWorldPageModel','retailWorldPageRenderer')){
     if($null -eq $report.PSObject.Properties[$field]){throw "Missing actual-world visible field: $field"}
    }
    if($report.status -ne 'Ready' -or !$report.retailWorldVisiblePilot -or !$report.retailWorldReturned -or
       $report.retailWorldPhase -cne $phase -or [bool]$report.retailWorldOriginalOtherViews -ne $other -or
       [bool]$report.retailWorldPagePresent -ne $page -or [bool]$report.retailWorldRestored -ne $restored -or
       [uint64]$report.retailWorldCameraGeneration -le $previousCamera -or
       [uint64]$report.retailWorldRequest -le $previousRequest -or
       [uint64]$report.retailWorldPageBirth -eq 0 -or
       [uint64]$report.retailWorldOriginalInstance -ge 4294967295 -or
       [uint64]$report.retailWorldPageInstance -ge 4294967295){
     throw ('Actual-world visible returned-row mismatch: '+($report|ConvertTo-Json -Depth 4 -Compress))
    }
    if($page -and ([uint64]$report.retailWorldPageRenderer -eq 0 -or
                   [uint64]$report.retailWorldPageModel -ge 4294967295)){
     throw 'Active actual-world page lacks renderer/model identity'
    }
   }
   function CheckStableWorld($report,$identity){
    if([uint64]$report.retailWorldOriginalInstance -ne $identity.original -or
       [uint64]$report.retailWorldPageInstance -ne $identity.page -or
       [uint64]$report.retailWorldPageBirth -ne $identity.birth -or
       [uint64]$report.retailWorldPageRenderer -ne $identity.renderer){
     throw 'Actual-world original/page identity changed across the transaction'
    }
   }
   $visibleTrace=@{ready=$ready;invalidation=$Invalidate;asset=$Asset;residency=[bool]$Residency;
     sourceLine=$sourceMatch.Value;placementLine=$placementMatch.Value;
     installed=$installed;environment=$environment;images=@{};steps=@{};
     scope='One actual installed-world placement with original other-view row and private main-only page; exact CPU rows and returned camera, not GPU completion, all-view coverage, pixel parity or performance acceptance'}
   $visibleTrace.images.sourceA1=CaptureVisible 'world-source-a1.png'
   $visibleTrace.images.sourceA2=CaptureVisible 'world-source-a2.png'
   $fine=WaitWorld (WorldAction 'beginRetailWorldVisible') 'Fine'
   CheckWorld $fine 'Fine' $true $true $false 0 0
   $identity=@{original=[uint64]$fine.retailWorldOriginalInstance;page=[uint64]$fine.retailWorldPageInstance;
     birth=[uint64]$fine.retailWorldPageBirth;renderer=[uint64]$fine.retailWorldPageRenderer}
   $visibleTrace.identity=$identity;$visibleTrace.steps.beginFine=$fine;SaveVisible
   $visibleTrace.images.pageFine1=CaptureVisible 'world-page-fine-1.png'
   $visibleTrace.images.pageFine2=CaptureVisible 'world-page-fine-2.png'
   if($Auto){
    foreach($field in @('retailWorldAutoEnabled','retailWorldSurfaceCertified','retailWorldSurfaceStatus',
      'retailWorldSurfaceUpper','retailWorldSelectedTriangles','retailWorldSafeRootObservations',
      'retailWorldProjectionStatus','retailWorldProjectedBounded','retailWorldProjectedUpper',
      'retailWorldForcedFine','retailWorldStableFrameGeneration','retailWorldAutoObservations',
      'retailWorldAutoRefines','retailWorldAutoCoarsens')){
     if($null -eq $fine.PSObject.Properties[$field]){throw "Missing actual-world auto field: $field"}
    }
    if(!$fine.retailWorldSurfaceCertified -or [uint32]$fine.retailWorldSurfaceStatus -ne 0 -or
       [double]$fine.retailWorldSurfaceUpper -lt 0 -or
       [uint32]$fine.retailWorldSelectedTriangles -ne $expectedFineTriangles){
     throw ('Actual selected-surface certificate not verified at Fine: '+($fine|ConvertTo-Json -Depth 4 -Compress))
    }
    $autoTrace=@{scope='One original/page main-view follow using certified selected object-space surface bounds and returned stable frames; heuristic pixels only, no all-view or GPU-completion proof';
      certificate=@{status=$fine.retailWorldSurfaceStatus;upper=$fine.retailWorldSurfaceUpper;
        fineTriangles=$fine.retailWorldSelectedTriangles};poses=@();steps=@{initialFine=$fine};
      boundRootThreshold=3;boundFineThreshold=4;passed=$false}
    function SaveAuto {$autoTrace|ConvertTo-Json -Depth 18|Set-Content (Join-Path $out 'actual-world-auto.json')}
    function SetAutoPose($pose){
     $reply=Send @{cmd='eval';code=('triFreeFlyPose "'+$pose+'"')}
     if($null -eq $reply.PSObject.Properties['result'] -or $reply.result.Trim('"') -cne 'OK'){
      throw ('Actual-world auto pose refused: '+$pose)
     }
    }
    $follow=WorldAction 'followRetailWorldVisible'
    if($follow.status -notin @('Ready','Pending') -or !$follow.retailWorldAutoEnabled){
     throw ('Actual-world follow refused: '+($follow|ConvertTo-Json -Depth 4 -Compress))
    }
    $autoTrace.steps.follow=$follow;SaveAuto
    $autoRoot=$null
    # The fixture is a 64 x 50 m WRP. Zeus free-fly clamps its simulated
    # position to [0, 3200] on both horizontal axes after triFreeFlyPose
    # returns OK; negative coordinates would repeat the same border view.
    # The southwest corner is the farthest in-world view of the (1200,1200)
    # placement, with the heading aimed at the selected rock. The elevated
    # opposite edge adds valid horizontal distance; returned projection/ACK proves safety.
    foreach($pose in @('200 1200 30 90 -1','5 1200 30 90 -1',
                      '5 5 30 45 -0.6','3195 3195 30 225 -0.4')){
     SetAutoPose $pose
     $entry=@{pose=$pose;reports=@();startedAtUtc=[DateTime]::UtcNow.ToString('o')}
     $poseDeadline=[DateTime]::UtcNow.AddSeconds(3.5)
     do{
      $observed=WorldAction 'pollRetailWorldVisible'
      $entry.reports+=@{status=$observed.status;phase=$observed.retailWorldPhase;
       cameraGeneration=$observed.retailWorldCameraGeneration;
       stableGeneration=$observed.retailWorldStableFrameGeneration;
       selectedTriangles=$observed.retailWorldSelectedTriangles;
       bounded=$observed.retailWorldProjectedBounded;upper=$observed.retailWorldProjectedUpper;
       projectionStatus=$observed.retailWorldProjectionStatus;forcedFine=$observed.retailWorldForcedFine;
       safeRoot=$observed.retailWorldSafeRootObservations;
       observations=$observed.retailWorldAutoObservations;
       refines=$observed.retailWorldAutoRefines;coarsens=$observed.retailWorldAutoCoarsens}
      if($observed.status -notin @('Pending','Ready')){
       throw ('Actual-world auto follow refused: '+($observed|ConvertTo-Json -Depth 4 -Compress))
      }
      if($observed.status -eq 'Ready' -and $observed.retailWorldPhase -ceq 'Root' -and
         (!$observed.retailWorldProjectedBounded -or $observed.retailWorldForcedFine -or
          [double]$observed.retailWorldProjectedUpper -ge 3)){
       throw ('Actual-world auto selected unsafe Root: '+($observed|ConvertTo-Json -Depth 4 -Compress))
      }
      if($observed.status -eq 'Ready' -and $observed.retailWorldPhase -ceq 'Root' -and
         $observed.retailWorldReturned -and $observed.retailWorldAutoEnabled -and
         $observed.retailWorldSurfaceCertified -and $observed.retailWorldProjectedBounded -and
         !$observed.retailWorldForcedFine -and
         [double]$observed.retailWorldProjectedUpper -lt 3 -and
         [uint32]$observed.retailWorldSelectedTriangles -lt $expectedFineTriangles -and
         [uint64]$observed.retailWorldAutoCoarsens -ge 1 -and
         [uint32]$observed.retailWorldSafeRootObservations -ge 3 -and
         [uint64]$observed.retailWorldStableFrameGeneration -gt [uint64]$follow.retailWorldStableFrameGeneration -and
         [uint64]$observed.retailWorldCameraGeneration -gt [uint64]$fine.retailWorldCameraGeneration){
       $autoRoot=$observed;break
      }
      Start-Sleep -Milliseconds 100
     }while([DateTime]::UtcNow -lt $poseDeadline -and [DateTime]::UtcNow -lt $overall)
     $entry.endedAtUtc=[DateTime]::UtcNow.ToString('o');$autoTrace.poses+=,$entry;SaveAuto
     if($autoRoot){break}
    }
    if(!$autoRoot){throw 'Actual-world auto never returned a certified bounded Root across four in-world far poses'}
    CheckStableWorld $autoRoot $identity
    $autoTrace.steps.farRoot=$autoRoot;SaveAuto
    if($Residency){
     $autoTrace.cycles=@();$cycleStartIds=[uint32]$ready.retailRecordFreshIds
     for($cycle=1;$cycle -le 3;$cycle++){
     if($cycle -gt 1){SetAutoPose '3195 3195 30 225 -0.4'}
     $rootOnly=$null;$retireDeadline=[DateTime]::UtcNow.AddSeconds(20)
     do{
      $observed=WorldAction 'pollRetailWorldVisible'
      if($observed.status -notin @('Pending','Ready')){
       throw ('Actual-world Fine retirement refused: '+($observed|ConvertTo-Json -Depth 4 -Compress))
      }
      if($observed.retailWorldResidencyPhase -ceq 'RootOnly' -and
         [uint32]$observed.retailWorldResidencyCycles -eq ($cycle-1)){
       if($observed.status -ne 'Ready' -or $observed.retailWorldPhase -cne 'Root' -or
          !$observed.retailWorldReturned -or !$observed.retailWorldPagePresent -or
          !$observed.retailWorldOriginalOtherViews -or !$observed.retailWorldResidencyEnabled -or
          [uint64]$observed.retailWorldRetirementRequest -eq 0 -or
          [uint64]$observed.retailWorldResidencyTriggerCamera -eq 0 -or
          [uint32]$observed.retailWorldResidencyCycleLimit -ne 3 -or
          [uint64]$observed.retailWorldResidencyReservedBytes -eq 0 -or
          [uint64]$observed.retailWorldRetirementAckRequest -ne
           [uint64]$observed.retailWorldRetirementRequest -or
          [uint32]$observed.retailWorldLastRetiredMeshes -eq 0 -or
          [uint32]$observed.retailWorldRetiredFineModels -ne $cycle -or
          [uint32]$observed.retailWorldCompactedMeshes -ne
           ($cycle*[uint32]$observed.retailWorldLastRetiredMeshes) -or
          [uint32]$observed.retailRecordMeshesAbsent -ne 0 -or
          [uint32]$observed.retailRecordMeshesPresent + [uint32]$observed.retailWorldLastRetiredMeshes -ne
           [uint32]$ready.retailRecordMeshesPresent -or
          [uint32]$observed.retailRecordModelsPresent -ne 1 -or
          [uint32]$observed.retailRecordModelsAbsent -ne 0 -or
          $observed.pageLiveJobs -or $observed.pageReservedBytes){
        throw ('RootOnly lacks exact retired Fine ACK: '+($observed|ConvertTo-Json -Depth 4 -Compress))
       }
       $rootOnly=$observed;break
      }
      Start-Sleep -Milliseconds 100
     }while([DateTime]::UtcNow -lt $retireDeadline -and [DateTime]::UtcNow -lt $overall)
     if(!$rootOnly){throw 'Actual-world residency did not return RootOnly with retired Fine records'}
     CheckStableWorld $rootOnly $identity
     $autoTrace.steps.rootOnly=$rootOnly;SaveAuto
     SetAutoPose '1184 1200 18 90 -8'
     $fallback=$null;$completed=$null;$refillDeadline=[DateTime]::UtcNow.AddSeconds(35)
     do{
      $observed=WorldAction 'pollRetailWorldVisible'
      if($observed.status -notin @('Pending','Ready')){
       throw ('Actual-world residency refill refused: '+($observed|ConvertTo-Json -Depth 4 -Compress))
      }
      if(!$fallback -and $observed.retailWorldConventionalFallback -and
         $observed.retailWorldResidencyPhase -ceq 'RestoringForRefill' -and
         $observed.retailWorldPhase -ceq 'Restored' -and
         $observed.retailWorldReturned -and !$observed.retailWorldPagePresent -and
         !$observed.retailWorldOriginalOtherViews -and $observed.retailWorldRestored -and
         [uint64]$observed.retailWorldFallbackCameraGeneration -gt
          [uint64]$rootOnly.retailWorldCameraGeneration -and
         [uint64]$observed.retailWorldRefillRequest -eq 0){
       $fallback=$observed;$autoTrace.steps.conventionalFallback=$fallback;SaveAuto
      }
      if($observed.retailWorldResidencyPhase -in @('Both','Complete') -and
         [uint32]$observed.retailWorldResidencyCycles -eq $cycle){
       if($observed.status -ne 'Ready' -or
          $observed.retailWorldPhase -cne 'Fine' -or !$observed.retailWorldReturned -or
          !$observed.retailWorldPagePresent -or !$observed.retailWorldOriginalOtherViews -or
          !$observed.retailWorldResidencyEnabled -or
          [uint32]$observed.retailWorldResidencyCycles -ne $cycle -or
          [uint32]$observed.retailWorldSelectedTriangles -ne $expectedFineTriangles -or
          [uint64]$observed.retailWorldFallbackCameraGeneration -le
           [uint64]$rootOnly.retailWorldCameraGeneration -or
          [uint64]$observed.retailWorldFallbackRequest -le
           [uint64]$rootOnly.retailWorldRequest -or
          [uint64]$observed.retailWorldFallbackRequest -ge
           [uint64]$observed.retailWorldRequest -or
          [uint64]$observed.retailWorldFallbackPageBirth -ne [uint64]$identity.birth -or
          [uint64]$observed.retailWorldFallbackInstanceEpoch -eq 0 -or
          [uint32]$observed.retailWorldFallbackOriginalFlags -ne 0 -or
          !$observed.retailWorldFallbackPageAbsent -or
          [uint64]$observed.retailWorldRefillRequestAtFallback -ne 0 -or
          [uint64]$observed.retailWorldCameraGeneration -le
           [uint64]$observed.retailWorldFallbackCameraGeneration -or
          [uint64]$observed.retailWorldRefillRequest -eq 0 -or
          [uint64]$observed.retailWorldPageBirth -le [uint64]$identity.birth -or
          [uint64]$observed.retailWorldPageInstance -eq [uint64]$identity.page -or
          [uint64]$observed.retailWorldPageRenderer -eq [uint64]$identity.renderer -or
          [uint64]$observed.retailWorldPageModel -eq [uint64]$fine.retailWorldPageModel -or
          [uint64]$observed.retailWorldPageModel -eq [uint64]$rootOnly.retailWorldPageModel -or
          [uint64]$observed.retailWorldOriginalInstance -ne [uint64]$identity.original -or
          [uint32]$observed.retailRecordMeshesPresent -ne [uint32]$ready.retailRecordMeshesPresent -or
          [uint32]$observed.retailRecordMeshesAbsent -ne 0 -or
          [uint32]$observed.retailRecordModelsPresent -ne $initialModelCount -or
          [uint32]$observed.retailRecordModelsAbsent -ne 0 -or
          [uint32]$observed.retailRecordFreshIds -lt
           ($cycleStartIds + [uint32]$rootOnly.retailWorldLastRetiredMeshes + 1) -or
          $observed.pageLiveJobs -or $observed.pageReservedBytes){
        throw ('Completed residency lacks fresh Fine takeover/record ACK: '+
          ($observed|ConvertTo-Json -Depth 4 -Compress))
       }
       $completed=$observed;break
      }
      Start-Sleep -Milliseconds 20
     }while([DateTime]::UtcNow -lt $refillDeadline -and [DateTime]::UtcNow -lt $overall)
     if(!$completed){throw 'Actual-world residency never returned fresh Fine takeover'}
     CheckWorld $completed 'Fine' $true $true $false `
      ([uint64]$completed.retailWorldFallbackCameraGeneration) `
      ([uint64]$completed.retailWorldFallbackRequest)
     if(!$fallback){
      $autoTrace.steps.conventionalFallback=@{
       sampled=$false;returnedRestoreCameraGeneration=$completed.retailWorldFallbackCameraGeneration;
       restoreRequest=$completed.retailWorldFallbackRequest;
       retiredPageBirth=$completed.retailWorldFallbackPageBirth;
       instanceEpoch=$completed.retailWorldFallbackInstanceEpoch;
       originalFlags=$completed.retailWorldFallbackOriginalFlags;
       pageAbsent=$completed.retailWorldFallbackPageAbsent;
       refillRequestAtRestore=$completed.retailWorldRefillRequestAtFallback;
       note='Durable exact returned Restore ACK retained by producer; transient Restored state was not sampled by harness'}
     }elseif([uint64]$fallback.retailWorldFallbackRequest -ne
              [uint64]$completed.retailWorldFallbackRequest -or
            [uint64]$fallback.retailWorldFallbackInstanceEpoch -ne
              [uint64]$completed.retailWorldFallbackInstanceEpoch){
      throw 'Sampled conventional fallback differs from retained Restore ACK'
     }
     $autoTrace.steps.freshFine=$completed
     $autoTrace.cycles+=@{cycle=$cycle;rootOnly=$rootOnly;freshFine=$completed;
       conventionalFallback=$autoTrace.steps.conventionalFallback};SaveAuto
     $cycleStartIds=[uint32]$completed.retailRecordFreshIds
     $fine=$completed
     $identity=@{original=[uint64]$completed.retailWorldOriginalInstance;page=[uint64]$completed.retailWorldPageInstance;
       birth=[uint64]$completed.retailWorldPageBirth;renderer=[uint64]$completed.retailWorldPageRenderer}
     }
     $visibleTrace.images.pageFreshFine=CaptureVisible 'world-page-fresh-fine.png';SaveVisible
     $autoTrace.passed=$true;SaveAuto
    }else{
    SetAutoPose '1184 1200 18 90 -8'
    $autoFine=$null;$nearDeadline=[DateTime]::UtcNow.AddSeconds(8)
    do{
     $observed=WorldAction 'pollRetailWorldVisible'
     if($observed.status -notin @('Pending','Ready')){
      throw ('Actual-world auto near follow refused: '+($observed|ConvertTo-Json -Depth 4 -Compress))
     }
     if($observed.status -eq 'Ready' -and $observed.retailWorldPhase -ceq 'Root' -and
        (!$observed.retailWorldProjectedBounded -or $observed.retailWorldForcedFine)){
      throw ('Actual-world auto retained Root under unknown near projection: '+
        ($observed|ConvertTo-Json -Depth 4 -Compress))
     }
     if($observed.status -eq 'Ready' -and $observed.retailWorldPhase -ceq 'Fine' -and
        $observed.retailWorldReturned -and $observed.retailWorldAutoEnabled -and
        [uint32]$observed.retailWorldSelectedTriangles -eq $expectedFineTriangles -and
        [uint64]$observed.retailWorldAutoRefines -ge 1 -and
        [uint64]$observed.retailWorldStableFrameGeneration -gt [uint64]$autoRoot.retailWorldStableFrameGeneration -and
        [uint64]$observed.retailWorldCameraGeneration -gt [uint64]$autoRoot.retailWorldCameraGeneration -and
        ($observed.retailWorldForcedFine -or
         ($observed.retailWorldProjectedBounded -and [double]$observed.retailWorldProjectedUpper -gt 4))){
      $autoFine=$observed;break
     }
     Start-Sleep -Milliseconds 100
    }while([DateTime]::UtcNow -lt $nearDeadline -and [DateTime]::UtcNow -lt $overall)
    if(!$autoFine){throw 'Actual-world auto failed to return fresh near Fine after bounded far Root'}
    CheckStableWorld $autoFine $identity
    $autoTrace.steps.nearFine=$autoFine;SaveAuto
    $stopped=WaitWorld (WorldAction 'stopRetailWorldVisible') 'Fine'
    if($stopped.retailWorldAutoEnabled -or
       [uint32]$stopped.retailWorldSelectedTriangles -ne $expectedFineTriangles){
     throw ('Actual-world auto did not stop on returned Fine: '+($stopped|ConvertTo-Json -Depth 4 -Compress))
    }
    CheckStableWorld $stopped $identity
    $autoTrace.steps.stopped=$stopped;$autoTrace.passed=$true;SaveAuto
    }
    $visibleTrace.autoEvidence=Join-Path $out 'actual-world-auto.json';SaveVisible
   }
   if($Residency){
    $stopped=WaitWorld (WorldAction 'stopRetailWorldVisible') 'Restored'
    if($stopped.retailWorldAutoEnabled -or $stopped.retailWorldPagePresent -or
       $stopped.retailWorldOriginalOtherViews -or !$stopped.retailWorldRestored -or
       [uint64]$stopped.retailWorldCameraGeneration -le
        [uint64]$completed.retailWorldCameraGeneration){
     throw ('Residency Stop did not restore ordinary original: '+($stopped|ConvertTo-Json -Depth 4 -Compress))
    }
    $visibleTrace.steps.stoppedRestored=$stopped;SaveVisible
    $visibleTrace.images.sourceA3=CaptureVisible 'world-source-a3.png'
    $visibleTrace.images.sourceA4=CaptureVisible 'world-source-a4.png'
    $aborted=WaitRecord (RecordAction 'abortRetailRecords') 'pollRetailRecords'
    if($aborted.retailRecordMeshesPresent -ne 0 -or $aborted.retailRecordModelsPresent -ne 0 -or
       $aborted.pageLiveJobs -or $aborted.pageReservedBytes){throw 'World residency abort debt'}
    $visibleTrace.steps.aborted=$aborted;$visibleTrace.passed=$true;SaveVisible
    $released=$rootOnly;$refilled=$completed
   }else{
   $manualBeforeRoot=if($Auto){$stopped}else{$fine}
   $rootCut=WaitWorld (WorldAction 'rootRetailWorldVisible') 'Root'
   CheckWorld $rootCut 'Root' $true $true $false ([uint64]$manualBeforeRoot.retailWorldCameraGeneration) ([uint64]$manualBeforeRoot.retailWorldRequest)
   CheckStableWorld $rootCut $identity
   if($rootCut.retailWorldPageModel -eq $fine.retailWorldPageModel){throw 'Root did not select a distinct retained page model'}
   $visibleTrace.steps.root=$rootCut;SaveVisible
   $visibleTrace.images.pageRoot=CaptureVisible 'world-page-root.png'
   $released=WaitRecord (RecordAction 'releaseRetailRecords') 'pollRetailRecords'
   if([uint32]$released.retailRecordMeshesPresent -eq 0 -or
      [uint32]$released.retailRecordMeshesAbsent -eq 0 -or
      [uint32]$released.retailRecordMeshesPresent + [uint32]$released.retailRecordMeshesAbsent -ne
       [uint32]$ready.retailRecordMeshesPresent -or
      [uint32]$released.retailRecordModelsPresent -ne (1+$referenceCount) -or
      [uint32]$released.retailRecordModelsAbsent -ne 1){throw 'World record release mismatch'}
   $stillRoot=WorldAction 'pollRetailWorldVisible'
   if($stillRoot.status -ne 'Ready' -or $stillRoot.retailWorldPhase -cne 'Root' -or
      !$stillRoot.retailWorldReturned -or !$stillRoot.retailWorldOriginalOtherViews -or !$stillRoot.retailWorldPagePresent){
    throw 'World page ceased to be returned Root during record release'
   }
   CheckStableWorld $stillRoot $identity
   $visibleTrace.steps.released=$released;$visibleTrace.steps.rootAfterRelease=$stillRoot;SaveVisible
   $refilled=WaitRecord (RecordAction 'refillRetailRecords') 'pollRetailRecords'
   if([uint32]$refilled.retailRecordMeshesPresent -ne [uint32]$ready.retailRecordMeshesPresent -or
      [uint32]$refilled.retailRecordMeshesAbsent -ne [uint32]$released.retailRecordMeshesAbsent -or
      [uint32]$refilled.retailRecordModelsPresent -ne $initialModelCount -or
      [uint32]$refilled.retailRecordModelsAbsent -ne 1 -or
      [uint32]$refilled.retailRecordFreshIds -lt
       ([uint32]$ready.retailRecordFreshIds + [uint32]$released.retailRecordMeshesAbsent + 1)){
    throw 'World record fresh refill mismatch'
   }
   $visibleTrace.steps.refilled=$refilled;SaveVisible
   $freshFine=WaitWorld (WorldAction 'fineRetailWorldVisible') 'Fine'
   CheckWorld $freshFine 'Fine' $true $true $false ([uint64]$rootCut.retailWorldCameraGeneration) ([uint64]$rootCut.retailWorldRequest)
   CheckStableWorld $freshFine $identity
   if($freshFine.retailWorldPageModel -eq $fine.retailWorldPageModel -or
      $freshFine.retailWorldPageModel -eq $rootCut.retailWorldPageModel){throw 'Refill did not select a fresh Fine model'}
   $visibleTrace.steps.freshFine=$freshFine;SaveVisible
   $visibleTrace.images.pageFreshFine=CaptureVisible 'world-page-fresh-fine.png'
   if($Invalidate -eq 'None'){
    $restored=WaitWorld (WorldAction 'restoreRetailWorldVisible') 'Restored'
    CheckWorld $restored 'Restored' $false $false $true ([uint64]$freshFine.retailWorldCameraGeneration) ([uint64]$freshFine.retailWorldRequest)
    CheckStableWorld $restored $identity
    $visibleTrace.steps.restored=$restored;SaveVisible
    $visibleTrace.images.sourceA3=CaptureVisible 'world-source-a3.png'
    $visibleTrace.images.sourceA4=CaptureVisible 'world-source-a4.png'
   }else{
    $lifecycleAction=$(if($Invalidate -eq 'Move'){'moveRetailWorldVisible'}else{'removeRetailWorldVisible'})
    $invalidated=WaitInvalidatedWorld (WorldAction $lifecycleAction)
    foreach($field in @('retailWorldVisiblePilot','retailWorldReturned','retailWorldPagePresent',
      'retailWorldOriginalOtherViews','retailWorldRestored','retailWorldRequest',
      'retailWorldCameraGeneration','retailWorldPageBirth','retailWorldOriginalInstance',
      'retailWorldPageInstance','retailWorldPageRenderer')){
     if($null -eq $invalidated.PSObject.Properties[$field]){throw "Missing invalidation field: $field"}
    }
    if($invalidated.status -ne 'Failed' -or !$invalidated.retailWorldVisiblePilot -or
       !$invalidated.retailWorldReturned -or $invalidated.retailWorldPagePresent -or
       $invalidated.retailWorldOriginalOtherViews -or $invalidated.retailWorldRestored -or
       $invalidated.retailWorldPhase -cne 'InvalidatedPageAbsent' -or
       [uint64]$invalidated.retailWorldRequest -le [uint64]$freshFine.retailWorldRequest){
     throw ('Expected exact returned invalidation cleanup, not an active/ready page: '+
       ($invalidated|ConvertTo-Json -Depth 4 -Compress))
    }
    CheckStableWorld $invalidated $identity
    $visibleTrace.steps.invalidated=$invalidated
    $visibleTrace.invalidationCameraGenerationAdvanced=
      [uint64]$invalidated.retailWorldCameraGeneration -gt [uint64]$freshFine.retailWorldCameraGeneration
    SaveVisible
   }
   $aborted=WaitRecord (RecordAction 'abortRetailRecords') 'pollRetailRecords'
   if($aborted.retailRecordMeshesPresent -ne 0 -or $aborted.retailRecordModelsPresent -ne 0 -or
      $aborted.pageLiveJobs -or $aborted.pageReservedBytes){throw 'World record abort debt'}
   $visibleTrace.steps.aborted=$aborted;$visibleTrace.passed=$true;SaveVisible
   }
  }else{
   $released=WaitRecord (RecordAction 'releaseRetailRecords') 'pollRetailRecords'
   if([uint32]$released.retailRecordMeshesPresent -eq 0 -or
      [uint32]$released.retailRecordMeshesAbsent -eq 0 -or
      [uint32]$released.retailRecordMeshesPresent + [uint32]$released.retailRecordMeshesAbsent -ne
       [uint32]$ready.retailRecordMeshesPresent -or
      [uint32]$released.retailRecordModelsPresent -ne (1+$referenceCount) -or
      [uint32]$released.retailRecordModelsAbsent -ne 1){throw 'World record release mismatch'}
   $refilled=WaitRecord (RecordAction 'refillRetailRecords') 'pollRetailRecords'
   if([uint32]$refilled.retailRecordMeshesPresent -ne [uint32]$ready.retailRecordMeshesPresent -or
      [uint32]$refilled.retailRecordMeshesAbsent -ne [uint32]$released.retailRecordMeshesAbsent -or
      [uint32]$refilled.retailRecordModelsPresent -ne $initialModelCount -or
      [uint32]$refilled.retailRecordModelsAbsent -ne 1 -or
      [uint32]$refilled.retailRecordFreshIds -lt
       ([uint32]$ready.retailRecordFreshIds + [uint32]$released.retailRecordMeshesAbsent + 1)){
    throw 'World record fresh refill mismatch'
   }
   $aborted=WaitRecord (RecordAction 'abortRetailRecords') 'pollRetailRecords'
   if($aborted.retailRecordMeshesPresent -ne 0 -or $aborted.retailRecordModelsPresent -ne 0 -or
      $aborted.pageLiveJobs -or $aborted.pageReservedBytes){throw 'World record abort debt'}
  }
  @{ready=$ready;released=$released;refilled=$refilled;aborted=$aborted;
    asset=$Asset;residency=[bool]$Residency;independentBank=$bankProof;
    autoEvidence=$(if($Auto){Join-Path $out 'actual-world-auto.json'}else{$null});
    scope=$(if($Residency){'Actual one-placement source records, certified bounded Root, returned Root-only Fine retirement, conventional original fallback before one-page refill, fresh Fine takeover and final Restore/Abort; no pixel fidelity, GPU completion or general all-pass proof'}elseif($Auto){'Actual cold-world source records, certified Root/Fine object-space surface witness, returned main-view auto Root/Fine followed by manual lifecycle; no pixel fidelity or all-view proof'}elseif($Invalidate -ne 'None'){'Actual one-placement world/page transaction followed by ordinary actor invalidation and returned historical page absence; terminal Failed is expected cleanup, not an active page or visual proof'}elseif($Visible){'Actual cold-world Shape export, source-bound page records, and one returned main-view world/page transaction; no pixel parity, GPU completion, or general all-pass proof'}else{'Actual cold-world Shape export and private record lifetime; ordinary world model retained, no page instance takeover'})}|
   ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'actual-world-records.json')
 }
 $null=Send @{cmd='exit'}
 if(!$p.WaitForExit(10000) -or $p.ExitCode -ne 0){throw 'Game did not exit normally'}
 $normal=$true
 $text=Get-Content -LiteralPath $log -Raw
 if($text -match 'Out of memory|Validation Error|panicked at|DeviceLost|UNHANDLED' -or
    $text -notmatch 'Shutdown complete'){throw 'Runtime error or unclean shutdown'}
 $finalHashes=@(Get-FileHash -LiteralPath $exe,$dll)
 for($i=0;$i -lt 2;$i++){if($finalHashes[$i].Hash -ne $initialHashes[$i].Hash){throw 'Installed binary changed during run'}}
 if((Get-Content -LiteralPath $stampPath -Raw) -cne $installed){throw 'Installed deployment stamp changed during run'}
 $result=@{passed=$true;normalShutdown=$true;forcedTermination=$false;exitCode=$p.ExitCode;
  visible=[bool]$Visible;auto=[bool]$Auto;residency=[bool]$Residency;asset=$Asset;invalidation=$Invalidate;
  visibleEvidence=$(if($Visible){Join-Path $out 'actual-world-visible.json'}else{$null});
  autoEvidence=$(if($Auto){Join-Path $out 'actual-world-auto.json'}else{$null});
  sourceLine=$sourceMatch.Value;placementLine=$placementMatch.Value;shapeBirth=[uint64]$sourceMatch.Groups['birth'].Value;
  sourceSha256=$sourceMatch.Groups['source'].Value;pacRawSha256=$sourceMatch.Groups['raw'].Value;
  independentModelSha256=$bankProof.model.decodedSha256;
  independentPacRawSha256=$bankProof.texture.rawSha256;
  pacUploadedSha256=$sourceMatch.Groups['upload'].Value;primaryHandle=[uint64]$sourceMatch.Groups['handle'].Value;
  scope=$(if($Residency){'Actual selected installed-bank Shape/PAC SHA, certified bounded Root-only retirement, returned ordinary-original fallback before bounded refill, fresh Fine takeover and final Restore/Abort; CPU receipts, not pixel fidelity, GPU completion or all-view coverage'}elseif($Auto){'Actual installed-world Shape/PAC source, object-space selected-surface certificate, returned main-view auto Root/Fine and manual record lifecycle; heuristic projected demand is not pixel fidelity, GPU completion or all-view coverage'}elseif($Invalidate -ne 'None'){'Actual installed-world Shape/PAC source and returned page cleanup after ordinary actor move/removal; Failed is the expected fail-closed invalidated state, not GPU-completion or visual-parity proof'}elseif($Visible){'Actual installed-world Shape/PAC source and one returned original-other-views/private-main-view transaction with record refill and restore; screenshots captured for human comparison, no GPU-completion, pixel-parity, general all-pass or performance proof'}else{'Actual installed-archive cold-world Shape and same normal-bank PAC strict upload, plus source-current ordinary placement queued; no renderer return, private page takeover, complete model, or visual parity proof'})}
 $result|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'result.json')
}catch{$failure=$_}
finally{
 if($p -and !$p.HasExited){
  try{if($client -and $client.Connected){$null=Send @{cmd='exit'};$normal=$p.WaitForExit(10000) -and $p.ExitCode -eq 0}}catch{}
  if(!$p.HasExited){Stop-Process -Id $p.Id -Force;$forced=$true;$p.WaitForExit(5000)|Out-Null}
 }
 if($client){$client.Dispose()}
 if($Visible -and $visibleTrace){
  if($failure){$visibleTrace.passed=$false;$visibleTrace.failure=$failure.Exception.Message}
  $visibleTrace|ConvertTo-Json -Depth 18|Set-Content (Join-Path $out 'actual-world-visible.json')
 }
 if($Auto -and $autoTrace){
  if($failure){$autoTrace.passed=$false;$autoTrace.failure=$failure.Exception.Message}
  $autoTrace|ConvertTo-Json -Depth 18|Set-Content (Join-Path $out 'actual-world-auto.json')
 }
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $old.Keys){Set-Item ('Env:'+$key) $old[$key]}
 if($failure){@{passed=$false;reason=$failure.Exception.Message;normalShutdown=$normal;
   forcedTermination=$forced;exitCode=$(if($p -and $p.HasExited){$p.ExitCode}else{$null});
   pid=$(if($p){$p.Id}else{$null})}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'failure.json')}
}
if($failure){throw $failure}
Write-Host $out
