<# Bounded installed depth diagnostic. Parent/operator executes under the shared
   game lock. No downloads, no existing runtime/profile writes, one owned process.
   A successful capture is NOT a depth correctness or visual acceptance result. #>
[CmdletBinding()]
param(
 [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{8,40}$')][string]$ExpectedCommit,
 [Parameter(Mandatory)][string]$RuntimeDirectory,
 [ValidateSet('Probe','DHDebug','DH')][string]$Mode='Probe',
 [ValidateSet(1,2,4,8)][int]$MsaaSamples=1,
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault')
$ErrorActionPreference='Stop'
function Require($ok,[string]$message){if(!$ok){throw $message}}
Require ([bool]$env:LOCK_OWNER) 'Shared game lock required'
Require (!(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue)) 'Existing game preserved'
$repo=Split-Path -Parent $PSScriptRoot
$stamp=(Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw).Trim()
Require ($stamp.Contains($ExpectedCommit)) 'Unexpected installed source'
$source=(Resolve-Path -LiteralPath $RuntimeDirectory).Path
$presetName=switch($Mode){'Probe'{'OpenPoseidon-Depth-Probe.ini'};'DHDebug'{'OpenPoseidon-DH-Depth-Debug.ini'};'DH'{'OpenPoseidon-DH-RTGI-Preview.ini'}}
$effectName=if($Mode-eq'Probe'){'OpenPoseidonDepthProbe.fx'}else{'dh_uber_rt.fx'}
$technique=if($Mode-eq'Probe'){'OpenPoseidonDepthProbe@OpenPoseidonDepthProbe.fx'}else{'DH_UBER_RT@dh_uber_rt.fx'}
$required=@('ReShade64.dll','ReShade64.json','OpenPoseidonReShade.addon64','OpenPoseidon.ini',
 'Shaders/OpenPoseidonLook.fx','DEPTH-PROVENANCE.json',('Presets/'+$presetName),('Shaders/'+$effectName))
if($Mode-ne'Probe'){$required+=@('Shaders/ReShade.fxh','Licenses/DH-UBER-RT-GPL-2.0.txt')}
foreach($name in $required){Require (Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf) ('Stage the licensed runtime first; missing '+$name)}
$digests=@{
 'ReShade64.dll'='0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7';
 'ReShade64.json'='aa21713718843e531da396e2bfc80772c9cb3c369d6c30b836be6b0ae812d503'
}
if($Mode-ne'Probe'){
 $digests['Licenses/DH-UBER-RT-GPL-2.0.txt']='8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643'
 $digests['Shaders/dh_uber_rt.fx']='c14f19eb7b0359cb4042749e52c28c6a18c2df89b9fd40f5ab9bd209e36e5320'
 $digests['Shaders/ReShade.fxh']='6dabfbbaf968c3871905d2ea17f96572ff7b1cec01310b5d0e5252b66b30174f'
}
foreach($name in $digests.Keys){Require ((Get-FileHash -LiteralPath (Join-Path $source $name)).Hash.ToLowerInvariant()-ceq$digests[$name]) ('Staged file digest mismatch: '+$name)}
$provenance=Get-Content -LiteralPath (Join-Path $source 'DEPTH-PROVENANCE.json') -Raw|ConvertFrom-Json
Require ($provenance.probe_license-ceq'GPL-3.0-or-later') 'Probe licence receipt missing'
if($Mode-ne'Probe'){Require ($provenance.dh_license-ceq'GPL-2.0 (upstream LICENSE)') 'DH licence receipt missing'}
$preset=Get-Content -LiteralPath (Join-Path $source ('Presets/'+$presetName)) -Raw
Require ([regex]::IsMatch($preset,'(?m)^Techniques='+[regex]::Escape($technique)+'\r?$')) 'Preset must select exactly the requested technique'
# Short scratch paths avoid the legacy profile path limit. Only newly created
# files below this directory are writable; the caller's staged files stay intact.
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('op-rsd-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$out=Join-Path $repo ('build/reshade-depth/'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+$Mode)
$null=New-Item -ItemType Directory -Path $scratch
$null=New-Item -ItemType Directory -Force -Path $out
$runtime=Join-Path $scratch 'runtime';$user=Join-Path $scratch 'user';$mission=Join-Path $scratch 'comparison.eden'
foreach($dir in @($runtime,$user,$mission)){ $null=New-Item -ItemType Directory -Path $dir }
foreach($name in $required){
 $target=Join-Path $runtime $name
 $null=New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)
 Copy-Item -LiteralPath (Join-Path $source $name) -Destination $target
}
# A fresh config deliberately selects one preset and has no global add-on paths.
@"
[GENERAL]
EffectSearchPaths=.\Shaders
TextureSearchPaths=.\Textures
PresetPath=.\Presets\$presetName
PerformanceMode=0
[ADDON]
AddonPath=.
[OVERLAY]
TutorialProgress=4
ShowScreenshotMessage=0
[SCREENSHOT]
SavePath=.\Screenshots
"@ | Set-Content -LiteralPath (Join-Path $runtime 'ReShade.ini') -Encoding UTF8
@'
version=11;
class Mission {
 randomSeed=42;
 class Intel {year=1985;month=6;day=21;hour=16;minute=0;startWeather=0.25;forecastWeather=0.25;startFog=0.25;};
 class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=1;class Item0 {
  position[]={8131.43,0,4086.37};azimut=229.1;id=0;side="WEST";vehicle="SoldierWB";
  player="PLAYER COMMANDER";leader=1;skill=1.0;
 };};};};
};
class Intro {randomSeed=1;class Intel {};};
class OutroWin {randomSeed=2;class Intel {};};
class OutroLoose {randomSeed=3;class Intel {};};
'@ | Set-Content -LiteralPath (Join-Path $mission 'mission.sqm')
& "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0 -MsaaSamples $MsaaSamples
# Exercise the engine's validated startup route as well as explicit layer env.
# This file is newly created above; ordinary graphics profiles are never changed.
Add-Content -LiteralPath (Join-Path $user 'graphics.cfg') -Value 'optionalReShade=1;'
$keys=@('POSEIDON_USER_DIR','POSEIDON_WHOLE_WORLD','VK_ADD_LAYER_PATH','VK_LAYER_PATH',
 'VK_INSTANCE_LAYERS','VK_LOADER_LAYERS_ENABLE','VK_LOADER_LAYERS_DISABLE','DISABLE_VK_LAYER_reshade_1','VK_LOADER_DEBUG',
 'RESHADE_BASE_PATH_OVERRIDE','WGPU_BACKEND','WGR_DLSS','WGR_AUTO_EXPOSURE','WGR_EXPOSURE','WGR_GTAO','WGR_GI')
$saved=@{};$savedExists=@{}
foreach($key in $keys){
 $savedExists[$key]=Test-Path -LiteralPath ('Env:'+ $key)
 $saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
}
$receipt=@{installed=$stamp;mode=$Mode;selectedTechnique=$technique;selectedPreset=$presetName;
 sourceRuntime=$source;scratch=$scratch;runtime=$runtime;msaaSamples=$MsaaSamples;optionalReShade=1;
 captureSucceeded=$false;visualAccepted=$false;depthAvailability='Pending operator inspection of probe/debug image';
 performanceScope='Engine frame_performance timers exclude injected ReShade GPU work; no total GPU cost claim';
 depthHint='Probe top strip: red=no selected DEPTH resource; green=selected resource only. Check scene geometry alignment.';
 fileHashes=@{};provenance=$provenance}
foreach($name in $required){$receipt.fileHashes[$name]=(Get-FileHash -LiteralPath (Join-Path $runtime $name)).Hash}
$p=$null;$client=$null;$reader=$null;$writer=$null
try{
 # Clear inherited explicit layer selection, then opt in solely for this process.
 # Recent .NET preserves an empty environment entry for SetEnvironmentVariable
 # with null. Vulkan enable/disable filters require absence, not an empty value.
 foreach($key in $keys){Remove-Item -LiteralPath ('Env:'+ $key) -ErrorAction SilentlyContinue}
 $env:POSEIDON_USER_DIR=$user;$env:POSEIDON_WHOLE_WORLD='0';$env:WGPU_BACKEND='vulkan'
 $env:WGR_DLSS='0';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='0.5';$env:WGR_GTAO='0';$env:WGR_GI='0'
 $env:VK_ADD_LAYER_PATH=$runtime;$env:VK_INSTANCE_LAYERS='VK_LAYER_reshade';$env:RESHADE_BASE_PATH_OVERRIDE=$runtime
 $env:VK_LOADER_DEBUG='error,warn,layer'
 $receipt.launchEnvironment=@{}
 foreach($key in $keys){
  $receipt.launchEnvironment[$key]=@{present=(Test-Path -LiteralPath ('Env:'+ $key));value=[Environment]::GetEnvironmentVariable($key,'Process')}
 }
 $reserve=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$reserve.Start();$port=$reserve.LocalEndpoint.Port;$reserve.Stop()
 $log=Join-Path $out 'game.log'
 $argsGame=@('--render=wgpu','--window','--dev','--width','1600','--height','900','--vd','3000',
  '--test-mission',$mission,'--test-world-freefly','8131.43','4086.37','191.04','229.1','-11.3',
  '--test-world-hour','16','--harness',"$port",'--log-file',$log)
 $quoted=($argsGame|ForEach-Object {'"'+$_+'"'})-join' '
 $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -ArgumentList $quoted -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError (Join-Path $out 'stderr.log')
 $receipt.pid=$p.Id;$deadline=[DateTime]::UtcNow.AddSeconds(90)
 do{
  Require (!$p.HasExited) 'Game exited before harness'
  try{$client=[Net.Sockets.TcpClient]::new('127.0.0.1',$port)}catch{$client=$null;Start-Sleep -Milliseconds 250}
  Require ([DateTime]::UtcNow-lt$deadline) 'Harness startup deadline'
 }while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=10000
 $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 function Send($command){
  $writer.WriteLine(($command|ConvertTo-Json -Compress -Depth 8))
  do{$line=$reader.ReadLine();Require ([bool]$line) 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok)
  Require $reply.ok $line;return $reply
 }
 # The harness is registered after renderer initialization. No runtime log here
 # means injection/discovery failed, rather than an expensive shader compilation.
 $reshadeLog=Join-Path $runtime 'ReShade.log'
 $loadDeadline=[DateTime]::UtcNow.AddSeconds(10)
 while(!(Test-Path -LiteralPath $reshadeLog)){
  Require ([DateTime]::UtcNow-lt$loadDeadline) 'ReShade did not load: no isolated ReShade.log within 10s after renderer/harness startup; inspect Vulkan loader diagnostics in stderr.log and launchEnvironment in result.json'
  Start-Sleep -Milliseconds 250
 }
 $receipt.runtimeLogCreated=$true
 Require (Select-String -Path $log -Pattern 'Optional ReShade requested for this launch' -Quiet) 'Engine validated ReShade startup opt-in was not evidenced in game.log'
 while(([string](Send @{cmd='eval';code='triSceneReady'}).result).Trim('"')-cne'OK'){
  Require ([DateTime]::UtcNow-lt$deadline) 'Scene startup deadline';Start-Sleep -Milliseconds 250
 }
 $null=Send @{cmd='exec';code='player allowDamage false;0 setRain 0;0 setOvercast .25;0 setFog .25;setDate [1985,6,21,16,0];setAccTime 0'}
 $pose=([string](Send @{cmd='eval';code='triFreeFlyPose "8131.43 4086.37 191.04 229.1 -11.3"'}).result).Trim('"')
 Require ($pose.StartsWith('OK')) ('Freefly comparison pose refused: '+$pose)
 $receipt.pose=$pose
 Require (!(Select-String -Path $log -Pattern 'Critical error:|Cannot load --test-world' -Quiet)) 'Comparison world failed to load'
 Require (Select-String -Path $log -Pattern 'worlds[\\/]eden.wrp' -Quiet) 'Everon landscape load missing'
 $state=Send @{cmd='optional_reshade';action='state'}
 $compilePattern=[regex]::Escape($effectName)+'.*successfully|successfully.*'+[regex]::Escape($effectName)
 # At most 90 seconds for compilation/readiness, separate from startup.
 $deadline=[DateTime]::UtcNow.AddSeconds(90)
 while(!$state.ready-or$state.techniques-lt1-or!(Test-Path -LiteralPath $reshadeLog)-or
       !(Select-String -Path $reshadeLog -Pattern $compilePattern -Quiet)){
  Require ([DateTime]::UtcNow-lt$deadline) 'ReShade compilation/readiness deadline'
  Start-Sleep -Milliseconds 500;$state=Send @{cmd='optional_reshade';action='state'}
 }
 $null=Send @{cmd='optional_reshade';action='off'};Start-Sleep -Seconds 3
 $receipt.off=Send @{cmd='optional_reshade';action='state'}
 Require (!$receipt.off.enabled) 'Live off toggle failed'
 $receipt.offEnginePerformance=Send @{cmd='frame_performance'}
 $null=Send @{cmd='optional_reshade';action='on'};Start-Sleep -Seconds 3
 $receipt.on=Send @{cmd='optional_reshade';action='state'}
 Require ($receipt.on.enabled-and$receipt.on.activeTechniques-eq1) 'Expected exactly one active selected effect'
 $receipt.onEnginePerformance=Send @{cmd='frame_performance'}
 $startCapture=Get-Date;$count=$receipt.on.captureCount
 $null=Send @{cmd='optional_reshade';action='capture'}
 $deadline=[DateTime]::UtcNow.AddSeconds(30)
 do{
  Start-Sleep -Milliseconds 250;$state=Send @{cmd='optional_reshade';action='state'}
  Require ([DateTime]::UtcNow-lt$deadline) 'Capture deadline'
 }while($state.captureCount-le$count-or$state.captureStage-ne0)
 $files=@(Get-ChildItem -LiteralPath (Join-Path $runtime 'Screenshots') -File|Where-Object {$_.LastWriteTime-ge$startCapture.AddSeconds(-1)})
 Require (@($files|Where-Object Name -Match 'Native').Count-gt0) 'Native final-frame capture missing'
 Require (@($files|Where-Object Name -Match 'ReShade').Count-gt0) 'Processed final-frame capture missing'
 foreach($file in $files){Copy-Item -LiteralPath $file.FullName -Destination $out}
 $receipt.captures=@($files|ForEach-Object {Join-Path $out $_.Name});$receipt.final=$state
 $null=Send @{cmd='exit'};$null=$p.WaitForExit(10000)
 Require $p.HasExited 'Clean exit deadline';$receipt.exitCode=$p.ExitCode
 Require ($p.ExitCode-eq0) 'Game exited unsuccessfully'
 Require (Test-Path -LiteralPath $reshadeLog) 'ReShade compile log missing'
 Copy-Item -LiteralPath $reshadeLog -Destination $out
 Require (!(Select-String -Path $reshadeLog -Pattern 'Failed to compile|error [XC][0-9]|Failed to create effect' -Quiet)) 'ReShade shader compile/create error'
 Require (Select-String -Path $reshadeLog -Pattern $compilePattern -Quiet) 'Selected shader successful compilation not evidenced in log'
 Require (!(Select-String -Path @($log,(Join-Path $out 'stderr.log')) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error:' -Quiet)) 'Renderer runtime error'
 Require ((Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Build changed during capture'
 $receipt.captureSucceeded=$true
 Write-Host ('Depth capture ready for operator inspection ('+$technique+'): '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 if($p-and!$p.HasExited){
  try{$receipt.loadedReShadeModules=@($p.Modules|Where-Object {$_.ModuleName-match'ReShade'}|ForEach-Object {$_.FileName})}
  catch{$receipt.moduleInspectionError=$_.Exception.Message}
 }
 if($p-and!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000);$receipt.forcedCleanup=$true}
 if($writer){$writer.Dispose()};if($reader){$reader.Dispose()};if($client){$client.Dispose()}
 foreach($key in $saved.Keys){
  if($savedExists[$key]){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
  else{Remove-Item -LiteralPath ('Env:'+ $key) -ErrorAction SilentlyContinue}
 }
 $stderr=Join-Path $out 'stderr.log'
 if(Test-Path -LiteralPath $stderr){
  $receipt.loaderDiagnostics=@(Select-String -Path $stderr -Pattern 'LOADER|VK_LAYER|ReShade'|Select-Object -First 200|ForEach-Object {$_.Line})
 }
 # Retain scratch logs/configs for diagnosis; never delete caller-owned files.
 foreach($name in @('ReShade.log','ReShade.ini',('Presets/'+$presetName))){
  $path=Join-Path $runtime $name
  if(Test-Path -LiteralPath $path){Copy-Item -LiteralPath $path -Destination (Join-Path $out (Split-Path -Leaf $name)) -Force}
 }
 $receipt|ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $out 'result.json')
}
