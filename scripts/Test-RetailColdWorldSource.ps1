param(
 [switch]$Records,
 [string]$Label='retail-cold-world-source',
 [string]$GameDir='D:\SteamLibrary\steamapps\common\ARMA Cold War Assault',
 [string]$Python='C:\Program Files\Python311\python.exe'
)
$ErrorActionPreference='Stop'
if($Label -notmatch '^[a-zA-Z0-9_-]{1,64}$'){throw 'Invalid bounded label'}
if(!$env:LOCK_OWNER){
 $shell=Join-Path $PSHOME $(if($PSVersionTable.PSEdition -eq 'Core'){'pwsh.exe'}else{'powershell.exe'})
 $argv=@((Join-Path $PSScriptRoot 'with-game-lock.sh'),$shell.Replace('\','/'),'-NoProfile','-File',$PSCommandPath,
  '-Label',$Label,'-GameDir',$GameDir,'-Python',$Python)
 if($Records){$argv+='-Records'}
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
New-Item -ItemType Directory $out,$profile,$mission | Out-Null
$old=@{};Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {$old[$_.Name]=$_.Value}
$p=$null;$client=$null;$normal=$false;$forced=$false;$failure=$null;$result=$null
try{
 if(!(Test-Path -LiteralPath $exe) -or !(Test-Path -LiteralPath $dll) -or !(Test-Path -LiteralPath $stampPath)){
  throw 'Installed game or deployment stamp missing'
 }
 $initialHashes=@(Get-FileHash -LiteralPath $exe,$dll)
 $installed=Get-Content -LiteralPath $stampPath -Raw
 $generated=@(& $Python $generator $fixture)
 if($LASTEXITCODE -ne 0 -or $generated.Count -ne 1 -or $generated[0].Length -gt 8192){throw 'Bounded retail WRP generation failed'}
 $proof=$generated[0]|ConvertFrom-Json
 $world=Join-Path $fixture 'retail-cold.wrp'
 if($proof.schema -ne 1 -or $proof.modelPath -cne 'data3d\skala_new.p3d' -or
    $proof.modelCount -ne 1 -or @($proof.placements).Count -ne 2 -or
    $proof.worldBytes -le 0 -or $proof.worldBytes -gt 131072 -or
    $proof.worldSha256 -cne (Get-FileHash -LiteralPath $world).Hash.ToLowerInvariant()){
  throw 'Retail WRP self-check/provenance mismatch'
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
 }
 if($Records){$environment.WGR_GEOMETRY_PAGE_FIXTURE='1';$environment.WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY='1'}
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
   scope='Actual installed-archive cold-world Shape/primary source and ordinary queued placement only; no renderer-return, takeover, complete model, or visual parity proof'}|
  ConvertTo-Json -Depth 8|Set-Content (Join-Path $out 'provenance.json')
 $overall=[DateTime]::UtcNow.AddSeconds(115)
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
 $sourcePattern='Retail actual-world source: shapeBirth=(?<birth>[1-9][0-9]*) sourceSha256=(?<source>[0-9a-f]{64}) decodedBytes=(?<decoded>[1-9][0-9]*) visualLevels=4 primaryHandle=(?<handle>[1-9][0-9]*) primarySlot=(?<slot>[1-9][0-9]*) rawSha256=(?<raw>[0-9a-f]{64}) uploadedSha256=(?<upload>[0-9a-f]{64})'
 do{
  if($p.HasExited){throw 'Game exited before actual-world source/placement witness'}
  $text=if(Test-Path -LiteralPath $log){Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue}else{''}
  if($text){
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
 if($Records){
  function RecordAction($action){return Send @{cmd='geometry_page_fixture';action=$action}}
  function WaitRecord($first,$poll){
   $recordDeadline=[DateTime]::UtcNow.AddSeconds(25);$record=$first
   do{
    if($record.status -eq 'Ready'){return $record}
    if($record.status -ne 'Pending'){throw ('Record refused: '+($record|ConvertTo-Json -Depth 3 -Compress))}
    Start-Sleep -Milliseconds 50;$record=RecordAction $poll
   }while([DateTime]::UtcNow -lt $recordDeadline)
   throw 'Bounded world record observation timed out'
  }
  $ready=WaitRecord (RecordAction 'probeRetailColdWorld') 'pollRetailRecords'
  if(!$ready.retailRecordOnly -or !$ready.retailFineTriangleSetExact -or
     $ready.retailRecordMeshesPresent -ne 8 -or $ready.retailRecordModelsPresent -ne 3 -or
     $ready.mainFrameReturned -or $ready.producerInstance -ne 4294967295){throw 'Initial actual-world records mismatch'}
  $released=WaitRecord (RecordAction 'releaseRetailRecords') 'pollRetailRecords'
  if($released.retailRecordMeshesPresent -ne 4 -or $released.retailRecordMeshesAbsent -ne 4){throw 'World record release mismatch'}
  $refilled=WaitRecord (RecordAction 'refillRetailRecords') 'pollRetailRecords'
  if($refilled.retailRecordMeshesPresent -ne 8 -or $refilled.retailRecordMeshesAbsent -ne 4 -or
     $refilled.retailRecordFreshIds -lt 8){throw 'World record fresh refill mismatch'}
  $aborted=WaitRecord (RecordAction 'abortRetailRecords') 'pollRetailRecords'
  if($aborted.retailRecordMeshesPresent -ne 0 -or $aborted.retailRecordModelsPresent -ne 0 -or
     $aborted.pageLiveJobs -or $aborted.pageReservedBytes){throw 'World record abort debt'}
  @{ready=$ready;released=$released;refilled=$refilled;aborted=$aborted;
    scope='Actual cold-world Shape export and private record lifetime; ordinary world model retained, no page instance takeover'}|
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
  sourceLine=$sourceMatch.Value;placementLine=$placementMatch.Value;shapeBirth=[uint64]$sourceMatch.Groups['birth'].Value;
  sourceSha256=$sourceMatch.Groups['source'].Value;pacRawSha256=$sourceMatch.Groups['raw'].Value;
  pacUploadedSha256=$sourceMatch.Groups['upload'].Value;primaryHandle=[uint64]$sourceMatch.Groups['handle'].Value;
  scope='Actual installed-archive cold-world Shape and same normal-bank PAC strict upload, plus source-current ordinary placement queued; no renderer return, private page takeover, complete model, or visual parity proof'}
 $result|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'result.json')
}catch{$failure=$_}
finally{
 if($p -and !$p.HasExited){
  try{if($client -and $client.Connected){$null=Send @{cmd='exit'};$normal=$p.WaitForExit(10000) -and $p.ExitCode -eq 0}}catch{}
  if(!$p.HasExited){Stop-Process -Id $p.Id -Force;$forced=$true;$p.WaitForExit(5000)|Out-Null}
 }
 if($client){$client.Dispose()}
 Get-ChildItem Env:|Where-Object {$_.Name -match '^(WGR_|POSEIDON_)'}|ForEach-Object {Remove-Item ('Env:'+$_.Name)}
 foreach($key in $old.Keys){Set-Item ('Env:'+$key) $old[$key]}
 if($failure){@{passed=$false;reason=$failure.Exception.Message;normalShutdown=$normal;
   forcedTermination=$forced;exitCode=$(if($p -and $p.HasExited){$p.ExitCode}else{$null});
   pid=$(if($p){$p.Id}else{$null})}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'failure.json')}
}
if($failure){throw $failure}
Write-Host $out
