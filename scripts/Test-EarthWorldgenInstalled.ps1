<# Actual generated RVW4 asset presentation fixture. Operator/parent owns the
   installed build and runs through with-game-lock.sh. No downloads or deployment.
   A passing receipt is not visual, collision, navigation or upstream validation. #>
[CmdletBinding()]
param(
 [ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit,
 [string]$Manifest='D:/OpenPoseidon-Worldgen-Lab-20261008/lux-region-03/generated/manifest.json',
 [string]$AddonRoot='D:/OpenPoseidon-Worldgen-Lab-20261008/lux-region-03/generated/@CWR-Milestone4',
 [string]$CacheDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'),
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [ValidateRange(30,180)][int]$PoseTimeoutSeconds=120,
 [switch]$SelfTest
)
$ErrorActionPreference='Stop'
function Require([bool]$ok,[string]$message){if(!$ok){throw $message}}
function Fmt([double]$number){Require ([double]::IsFinite($number)) 'Nonfinite coordinate';return $number.ToString('R',[Globalization.CultureInfo]::InvariantCulture)}
function Decode([string]$value){$value=$value.Trim();if($value.StartsWith('"')){Require ($value.EndsWith('"')) 'Malformed SQF string';return $value.Substring(1,$value.Length-2).Replace('""','"')};return $value}
function ParsePublication([string]$line){
 if($line-match'Earth Worldgen publication: revision (\d+) generation (\d+) source (\d+) covered (\d+) selected (\d+) modelLimitSkipped (\d+) error(.*)$'){
  return @{revision=[uint64]$Matches[1];generation=[uint64]$Matches[2];source=[int]$Matches[3];covered=[int]$Matches[4];selected=[int]$Matches[5];modelLimitSkipped=[int]$Matches[6];error=$Matches[7].Trim();raw=$line}
 }
}
function ParseInstalled([string]$line){
 if($line-match'Earth Worldgen installed assets: revision (\d+) generation (\d+) created (\d+) attempted (\d+) models (\d+); presentation only'){
  return @{revision=[uint64]$Matches[1];generation=[uint64]$Matches[2];created=[int]$Matches[3];attempted=[int]$Matches[4];models=[int]$Matches[5];raw=$line}
 }
}
function RegisteredFamilies($lines){
 $models=@(foreach($line in $lines){if($line-match'Earth Worldgen installed model: (.+?\.p3d) retained (\d+) levels ([1-9]\d*) positions ([1-9]\d*) mapType \d+ rigidAuthored 1'){ $Matches[1].ToLowerInvariant() }})
 return @{models=$models;tree=[bool]@($models|Where-Object {$_-match'^data3d[\\/]str .+\.p3d$'}).Count;road=[bool]@($models|Where-Object {$_-match'^o[\\/]road[\\/].+\.p3d$'}).Count;house=[bool]@($models|Where-Object {$_-match'^o[\\/]hous[\\/].+\.p3d$'}).Count}
}
if($SelfTest){
 $pub=ParsePublication 'Earth Worldgen publication: revision 3 generation 7 source 7369 covered 7000 selected 2048 modelLimitSkipped 0 error '
 Require ($pub.selected-eq2048-and$pub.revision-eq3-and$pub.error-eq'') 'Publication parser'
 $settled=ParseInstalled 'Earth Worldgen installed assets: revision 3 generation 7 created 2000 attempted 2048 models 16; presentation only'
 Require ($settled.attempted-eq2048-and$settled.created-eq2000) 'Created/attempted distinction lost'
 $families=RegisteredFamilies @('Earth Worldgen installed model: data3d\str smrk.p3d retained 0 levels 3 positions 100 mapType 1 rigidAuthored 1','Earth Worldgen installed model: o\road\sil25.p3d retained 2 levels 2 positions 100 mapType 1 rigidAuthored 1','Earth Worldgen installed model: O\Hous\domek_sedy.p3d retained 3 levels 4 positions 200 mapType 1 rigidAuthored 1')
 Require ($families.tree-and$families.road-and$families.house) 'Registered model-family parser'
 $bad=RegisteredFamilies @('Earth Worldgen model unavailable/bounded: data3d\str smrk.p3d','Earth Worldgen model not retained: o\road\sil25.p3d','source O\Hous\domek_sedy.p3d','Earth Worldgen installed model: O\Hous\domek_sedy.p3d retained 3 levels 0 positions 0 mapType 1 rigidAuthored 1')
 Require (!$bad.tree-and!$bad.road-and!$bad.house) 'Attempted/unavailable model falsely accepted'
 Require ((Decode '"{""covered"":true}"')-ceq'{"covered":true}') 'Ground SQF decode'
 Write-Host 'Worldgen installed harness parser and registration negative controls PASS';return
}
Require ($ExpectedCommit-and$env:LOCK_OWNER) 'ExpectedCommit and shared scripts/with-game-lock.sh required'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game preserved'
$stampPath=Join-Path $GameDir 'DEPLOYED-FROM.txt';$stamp=(Get-Content -LiteralPath $stampPath -Raw).Trim()
Require ($stamp.Contains($ExpectedCommit)) 'Installed source differs from ExpectedCommit'
$Manifest=(Resolve-Path -LiteralPath $Manifest).Path;$AddonRoot=(Resolve-Path -LiteralPath $AddonRoot).Path;$CacheDirectory=(Resolve-Path -LiteralPath $CacheDirectory).Path
$addonArchiveRoot=$AddonRoot
if(!@(Get-ChildItem -LiteralPath $addonArchiveRoot -File -Filter '*.pbo').Count){$addonArchiveRoot=Join-Path $AddonRoot 'AddOns'}
Require ((Test-Path -LiteralPath $addonArchiveRoot -PathType Container)-and@(Get-ChildItem -LiteralPath $addonArchiveRoot -File -Filter '*.pbo' -ErrorAction SilentlyContinue).Count-gt0) 'Generated addon archive root must contain PBO files directly, or provide a mod root with an AddOns child containing PBO files'
$source=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
Require ($source.world.bbox.Count-eq4-and[double]$source.world.bbox[0]-lt49.61-and[double]$source.world.bbox[2]-gt49.61-and[double]$source.world.bbox[1]-lt6.13-and[double]$source.world.bbox[3]-gt6.13) 'Fixture source must contain Luxembourg origin 49.61/6.13'
$imagery=Join-Path $CacheDirectory 'blue-marble-june-5400.jpg';Require (Test-Path -LiteralPath $imagery) 'Cached NASA overview required; no test downloads'
$repo=Split-Path -Parent $PSScriptRoot;$out=Join-Path $repo ('build/earth-worldgen-installed/'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[Guid]::NewGuid().ToString('N').Substring(0,6))
$null=New-Item -ItemType Directory -Force -Path $out
# Reuse only the supported empty fixture preparation, never its launcher/download.
$world=& "$PSScriptRoot/Launch-WholeWorld.ps1" -PrepareOnly -CacheDirectory $CacheDirectory
$mission=Join-Path $CacheDirectory 'fixture/earth-preview.abel'
$user=Join-Path ([IO.Path]::GetTempPath()) ('op-wg-'+[Guid]::NewGuid().ToString('N').Substring(0,8));$null=New-Item -ItemType Directory -Path $user
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -MsaaSamples 4 -DlssMode 0
Add-Content -LiteralPath (Join-Path $user 'graphics.cfg') -Value 'optionalReShade=0;'
$settings=@{POSEIDON_WHOLE_WORLD='1';POSEIDON_EARTH_LATITUDE='49.61';POSEIDON_EARTH_LONGITUDE='6.13';POSEIDON_EARTH_CACHE=$CacheDirectory;POSEIDON_EARTH_IMAGERY=$imagery;
 POSEIDON_EARTH_FEATURES='0';POSEIDON_EARTH_BUILDINGS='0';POSEIDON_EARTH_SURFACE='1';POSEIDON_EARTH_CONTENT_URL='';POSEIDON_EARTH_WORLDGEN_MANIFEST=$Manifest;
 POSEIDON_USER_DIR=$user;WGPU_BACKEND='vulkan';WGR_DLSS='0';WGR_RENDER_SCALE='1';WGR_GRASS='0';WGR_TERRAIN_RESOURCE_REUSE='1';WGR_WET_SOIL_DIAGNOSTIC='1';
 WGR_RESHADE_SCENE_DEPTH='0';VK_INSTANCE_LAYERS=$null;VK_ADD_LAYER_PATH=$null;VK_LOADER_LAYERS_ENABLE=$null;RESHADE_BASE_PATH_OVERRIDE=$null;
 HTTPS_PROXY='http://127.0.0.1:9';HTTP_PROXY='http://127.0.0.1:9';ALL_PROXY='http://127.0.0.1:9';NO_PROXY=''}
$saved=@{};$exists=@{};$p=$null;$client=$null;$reader=$null;$writer=$null
$log=Join-Path $out 'engine.log';$stderr=Join-Path $out 'stderr.log';$overall=[DateTime]::UtcNow.AddSeconds(180+3*$PoseTimeoutSeconds)
$result=@{installed=$stamp;expectedCommit=$ExpectedCommit;manifest=$Manifest;manifestSha256=(Get-FileHash -LiteralPath $Manifest).Hash;addonRoot=$AddonRoot;addonArchiveRoot=$addonArchiveRoot;cacheDirectory=$CacheDirectory;userDirectory=$user;
 origin=@(49.61,6.13);width=1600;height=900;msaaSamples=4;surface=1;proxyFeatures=0;proxyBuildings=0;offline=$true;upstreamValidation=$source.final_validation;
 passed=$false;visualAccepted=$false;playabilityAccepted=$false;poses=@();scope='Actual local Worldgen asset presentation only. Upstream validation may be failed. Counts prove retained registration, not pixels, collision, navigation, complete proxies or grounding quality.'}
function Health{Require ($p-and!$p.HasExited) 'Owned game exited';Require ([DateTime]::UtcNow-lt$overall) 'Overall Worldgen presentation deadline'}
function Send($command){Health;$writer.WriteLine(($command|ConvertTo-Json -Depth 8 -Compress));do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require $reply.ok $line;return $reply}
function Eval([string]$code){return Decode ([string](Send @{cmd='eval';code=$code}).result)}
function Location{ $loc=Eval 'triEarthLocation []';Require ($loc-match'^OK:latitude=([-+0-9.eE]+) longitude=([-+0-9.eE]+) altitude=([-+0-9.eE]+) revision=(\d+)$') 'Malformed Earth location';return @{revision=[uint64]$Matches[4];raw=$loc} }
function Ground($x,$z){return (Eval ('triEarthGround ['+(Fmt $x)+','+(Fmt $z)+']')|ConvertFrom-Json)}
function WaitFrames([int]$count){$start=[double](Eval 'triFrameCount');$until=[DateTime]::UtcNow.AddSeconds(20);do{Require ([DateTime]::UtcNow-lt$until) 'Rendered frame deadline';Start-Sleep -Milliseconds 50}while([double](Eval 'triFrameCount')-lt$start+$count)}
try{
 foreach($key in $settings.Keys){$exists[$key]=Test-Path -LiteralPath ('Env:'+$key);$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');if($null-eq$settings[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}else{[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}}
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $argv=@('--render=wgpu','--window','--dev','--width','1600','--height','900','--vd','3000','--harness',"$port",'--addon-root',$addonArchiveRoot,'--test-mission',$mission,'--test-world',$world,'--test-world-freefly','6400','6400','1000','0','-20','--test-world-hour','12','--log-file',$log)
 $result.arguments=$argv;$result.environment=$settings
 $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -WorkingDirectory $GameDir -WindowStyle Hidden -ArgumentList (($argv|ForEach-Object {'"'+$_+'"'})-join' ') -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError $stderr -PassThru
 $result.ownedPID=$p.Id;$until=[DateTime]::UtcNow.AddSeconds(150)
 do{Health;$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$until) 'Harness startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=15000;$stream.WriteTimeout=15000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 while((Eval 'triSceneReady')-cne'OK'){Require ([DateTime]::UtcNow-lt$until) 'Scene-ready deadline';Start-Sleep -Milliseconds 250}
 Require (!(Select-String -LiteralPath $log -Pattern 'Cannot load --test-world|Critical error' -Quiet)) 'Empty Earth fixture failed to load'
 $null=Send @{cmd='exec';code='player allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0;setDate [1985,6,21,12,0];setAccTime 0;triDevPanelShow 0;triCursorMove [0,0]'}
 $poses=@(@{name='north-road-house-tree';x=6625.248;z=6857.043;heading=5.56},@{name='east-road-house-tree';x=6961.358;z=6634.919;heading=220.875},@{name='south-road-house-tree';x=7058.299;z=5935.521;heading=237.315})
 foreach($pose in $poses){
  $arm=@{name=$pose.name;x=$pose.x;z=$pose.z;heading=$pose.heading;pitch=-35;clearance=15};$result.poses+=,$arm
  $location=Location;$until=[DateTime]::UtcNow.AddSeconds($PoseTimeoutSeconds)
  do{$ground=Ground $pose.x $pose.z;Require ($ground.revision-eq$location.revision) 'Ground origin changed before placement';if(!$ground.covered){Require ([DateTime]::UtcNow-lt$until) 'Resident ground missing at record-derived pose';Start-Sleep -Milliseconds 250}}while(!$ground.covered)
  $arm.groundBefore=$ground;$arm.revision=$location.revision;$arm.y=[double]$ground.height+15
  $beforeLines=@(Get-Content -LiteralPath $log).Count
  $placed=Eval ('triFreeFlyPose "'+(Fmt $pose.x)+' '+(Fmt $pose.z)+' '+(Fmt $arm.y)+' '+(Fmt $pose.heading)+' -35"');Require ($placed-ceq'OK') ('Ground-relative camera refused: '+$placed)
  $publication=$null;$installed=$null;$families=$null
  do{
   $lines=@(Get-Content -LiteralPath $log);$ground=Ground $pose.x $pose.z
   Require ($ground.covered-and$ground.revision-eq$location.revision) 'Current ground disappeared during asset admission'
   $publication=$null;$installed=$null;$start=-1;$end=-1
   for($i=$beforeLines;$i-lt$lines.Count;$i++){
    $candidate=ParsePublication $lines[$i]
    if($candidate-and$candidate.revision-eq$ground.revision-and$candidate.generation-eq$ground.generation){$publication=$candidate;$start=$i;$installed=$null;$end=-1}
    $candidate=ParseInstalled $lines[$i]
    if($publication-and$candidate-and$candidate.revision-eq$publication.revision-and$candidate.generation-eq$publication.generation){$installed=$candidate;$end=$i}
   }
   if($publication){$arm.publication=$publication;Require (!$publication.error) ('Worldgen publication error: '+$publication.error)}
   Require ([DateTime]::UtcNow-lt$until) 'Current Worldgen selection did not finish bounded admission; inspect publication/created/attempted log evidence'
   if(!$installed){Start-Sleep -Milliseconds 250}
  }while(!$installed)
  Require ($publication.selected-gt0-and$publication.selected-le2048-and$installed.attempted-eq$publication.selected-and$installed.created-gt0-and$installed.created-le$installed.attempted) 'Invalid settled created/attempted selection counts'
  $familyLines=@($lines[($start+1)..$end]);$families=RegisteredFamilies $familyLines
  $arm.installed=$installed;$arm.registered=$families;$arm.failedModels=@($familyLines|Where-Object {$_-match'Earth Worldgen model unavailable/bounded:|Earth Worldgen model not retained:'})
  $arm.allSelectedRegistered=($installed.created-eq$installed.attempted)
  Require ($families.tree-and$families.road-and$families.house) 'Successful retained stock tree, road and house registration required in this pose publication'
  WaitFrames 30;$arm.groundAfter=Ground $pose.x $pose.z;$arm.camera=(Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera
  Require ($arm.groundAfter.covered-and$arm.groundAfter.generation-eq$publication.generation-and(Location).revision-eq$location.revision) 'Resident publication changed before capture'
  Require ([Math]::Abs([double]$arm.camera.position[0]-$pose.x)-lt.1-and[Math]::Abs([double]$arm.camera.position[2]-$pose.z)-lt.1-and[Math]::Abs([double]$arm.camera.position[1]-$arm.y)-lt.1) 'Actual camera differs from fixed ground-relative pose'
  $arm.performance=Send @{cmd='frame_performance'};$arm.weather=Send @{cmd='weather_visibility'}
  Require ($arm.performance.renderWidth-eq1600-and$arm.performance.renderHeight-eq900-and$arm.performance.outputWidth-eq1600-and$arm.performance.outputHeight-eq900-and$arm.performance.msaaSamples-eq4-and$arm.performance.activeUpscaler-eq0) 'Actual renderer must be native 1600x900 with 4x MSAA and no upscaler'
  Require ([Math]::Abs([double]$arm.weather.fog)-lt.0001) 'Locked fixture native fog differs from zero'
  $path=Join-Path $out ($pose.name+'.png');$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(15)
  while(!(Test-Path -LiteralPath $path)-or(Get-Item -LiteralPath $path).Length-eq0){Health;Require ([DateTime]::UtcNow-lt$until) 'Screenshot deadline';Start-Sleep -Milliseconds 100}
  $arm.screenshot=$path;$arm.visualAccepted=$false
 }
 $null=Send @{cmd='exit'};$null=$p.WaitForExit(10000);Require $p.HasExited 'Clean exit deadline';$result.exitCode=$p.ExitCode;Require ($p.ExitCode-eq0) 'Owned process failed exit';$result.cleanTeardown=$true
 Require (!(Select-String -LiteralPath @($log,$stderr) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error' -Quiet)) 'GPU/engine runtime error'
 Require (!(Select-String -LiteralPath $log -Pattern 'downloads [1-9]' -Quiet)) 'Offline fixture unexpectedly downloaded elevation'
 Require ((Get-Content -LiteralPath $stampPath -Raw).Trim()-ceq$stamp) 'Installed build changed during presentation test'
 Require ((Get-FileHash -LiteralPath $Manifest).Hash-ceq$result.manifestSha256) 'Generated source manifest changed during test'
 $result.assetDiagnostics=@(Select-String -LiteralPath $log -Pattern 'Worldgen|Cannot load texture|Cannot open object|Cannot load model'|ForEach-Object {$_.Line})
 $result.performanceScope='Published engine timings only; no injected GPU cost or global performance acceptance.'
 $result.passed=$true;Write-Host ('Worldgen installed presentation evidence ready; images and grounding need inspection: '+$out)
}catch{
 $failure=$_;$result.error=$failure.Exception.Message
 # Keep the strict failed registration verdict, but preserve the actual scene
 # for diagnosis when the owned renderer is still responsive.
 if($p-and!$p.HasExited-and$writer){
  try{
   $path=Join-Path $out 'failure-native.png';$null=Send @{cmd='screenshot';path=$path};$until=[DateTime]::UtcNow.AddSeconds(5)
   while(!(Test-Path -LiteralPath $path)-or(Get-Item -LiteralPath $path).Length-eq0){Require ([DateTime]::UtcNow-lt$until) 'Failure screenshot deadline';Start-Sleep -Milliseconds 100}
   $result.failureScreenshot=$path
  }catch{$result.failureScreenshotError=$_.Exception.Message}
 }
 throw $failure
}finally{
 if($p-and!$p.HasExited){if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000);$result.forcedCleanup=$true}}
 if($writer){$writer.Dispose()};if($reader){$reader.Dispose()};if($client){$client.Dispose()}
 foreach($key in $saved.Keys){if($exists[$key]){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}else{Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}}
 $result|ConvertTo-Json -Depth 25|Set-Content -LiteralPath (Join-Path $out 'result.json')
}
