<# Installed single-player Vulkan/ReShade acceptance. Run under with-game-lock.sh.
   Uses final ReShade captures, a clean profile, and only the process it starts. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [string]$RuntimeDirectory=(Join-Path $env:LOCALAPPDATA 'OpenPoseidon/reshade/6.8.0'))
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Shared game lock required'}
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Existing game preserved'}
$repo=Split-Path -Parent $PSScriptRoot
$stamp=(Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw).Trim()
if(!$stamp.Contains($ExpectedCommit)){throw 'Unexpected installed source'}
$out=Join-Path $repo ('build/reshade-acceptance/'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Force -Path $out
$mission=Join-Path $out 'comparison.eden'
$null=New-Item -ItemType Directory -Force -Path $mission
# Stock OPRW2 worlds use the normal mission loader; --test-world is an importer
# and can silently leave an unrelated empty fallback landscape on rejection.
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
'@ | Set-Content (Join-Path $mission 'mission.sqm')
$keys=@('POSEIDON_USER_DIR','POSEIDON_WHOLE_WORLD','VK_ADD_LAYER_PATH','VK_INSTANCE_LAYERS',
 'RESHADE_BASE_PATH_OVERRIDE','WGPU_BACKEND','WGR_DLSS','WGR_AUTO_EXPOSURE','WGR_EXPOSURE')
$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
$receipt=@{installed=$stamp;passed=$false;arms=@();scope='Optional official ReShade color-only preset; no depth/RT parity claim'}
function Require($ok,$message){if(!$ok){throw $message}}
try{
 foreach($injected in @($false,$true)){
  $arm=Join-Path $out $(if($injected){'injected'}else{'normal'})
  $user=Join-Path $arm 'user';$null=New-Item -ItemType Directory -Force -Path $user
  & "$PSScriptRoot/Write-BenchmarkGraphics.ps1" -UserDir $user -DlssMode 0
  $env:POSEIDON_USER_DIR=$user;$env:POSEIDON_WHOLE_WORLD='0';$env:WGPU_BACKEND='vulkan'
  $env:WGR_DLSS='0';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='0.5'
  if($injected){
   Require (Test-Path (Join-Path $RuntimeDirectory 'OpenPoseidonReShade.addon64')) 'Prepare optional runtime first'
   $env:VK_ADD_LAYER_PATH=$RuntimeDirectory
   $env:VK_INSTANCE_LAYERS='VK_LAYER_reshade'
   $env:RESHADE_BASE_PATH_OVERRIDE=$RuntimeDirectory
  }else{
   Remove-Item Env:VK_ADD_LAYER_PATH,Env:VK_INSTANCE_LAYERS,Env:RESHADE_BASE_PATH_OVERRIDE -ErrorAction SilentlyContinue
  }
  $reserve=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$reserve.Start()
  $port=$reserve.LocalEndpoint.Port;$reserve.Stop()
  $log=Join-Path $arm 'game.log';$p=$null;$client=$null;$writer=$null
  try{
   $argsGame=@('--render=wgpu','--window','--dev','--width','1600','--height','900','--vd','3000',
    '--test-mission',$mission,
    '--test-world-freefly','8131.43','4086.37','191.04','229.1','-11.3',
    '--test-world-hour','16','--harness',"$port",'--log-file',$log)
   $quoted=($argsGame|ForEach-Object {'"'+$_+'"'})-join' '
   $p=Start-Process (Join-Path $GameDir 'OpenPoseidon.exe') -ArgumentList $quoted -WorkingDirectory $GameDir -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $arm 'stdout.log') -RedirectStandardError (Join-Path $arm 'stderr.log')
   $deadline=[DateTime]::UtcNow.AddSeconds(150)
   do{
    Require (!$p.HasExited) 'Game exited before harness'
    try{$client=[Net.Sockets.TcpClient]::new('127.0.0.1',$port)}catch{$client=$null;Start-Sleep -Milliseconds 250}
    Require ([DateTime]::UtcNow-lt$deadline) 'Harness deadline'
   }while(!$client)
   $stream=$client.GetStream();$stream.ReadTimeout=30000
   $reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
   function Send($command){
    $writer.WriteLine(($command|ConvertTo-Json -Compress -Depth 8))
    do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok)
    Require $reply.ok $line;return $reply
   }
   while(([string](Send @{cmd='eval';code='triSceneReady'}).result).Trim('"')-cne'OK'){
    Require ([DateTime]::UtcNow-lt$deadline) 'Scene deadline';Start-Sleep -Milliseconds 250
   }
   $null=Send @{cmd='exec';code='player allowDamage false;0 setRain 0;0 setOvercast .25;0 setFog .25;setDate [1985,6,21,16,0]'}
   $pose=([string](Send @{cmd='eval';code='triFreeFlyPose "8131.43 4086.37 191.04 229.1 -11.3"'}).result).Trim('"')
   Require ($pose.StartsWith('OK')) ('Freefly comparison pose refused: '+$pose)
   Start-Sleep -Seconds 5
   Require (!(Select-String -Path $log -Pattern 'Critical error:|Cannot load --test-world' -Quiet)) 'Comparison world failed to load'
   Require (Select-String -Path $log -Pattern 'worlds[\\/]eden.wrp' -Quiet) 'Everon landscape load missing'
   $state=Send @{cmd='optional_reshade';action='state'}
   $record=@{injected=$injected;initial=$state;log=$log}
   if(!$injected){
    Require (!$state.ready) 'Normal launch unexpectedly loaded ReShade'
    $path=Join-Path $arm 'normal.png';$null=Send @{cmd='screenshot';path=$path};$record.image=$path
   }else{
    $deadline=[DateTime]::UtcNow.AddSeconds(90)
    while(!$state.ready-or$state.techniques-lt1){
     Require ([DateTime]::UtcNow-lt$deadline) 'ReShade effects never became ready'
     Start-Sleep -Milliseconds 500;$state=Send @{cmd='optional_reshade';action='state'}
    }
    $null=Send @{cmd='optional_reshade';action='off'};Start-Sleep -Seconds 3
    $record.off=Send @{cmd='optional_reshade';action='state'}
    Require (!$record.off.enabled) 'Live off toggle failed'
    $record.offPerformance=Send @{cmd='frame_performance'}
    $null=Send @{cmd='optional_reshade';action='on'};Start-Sleep -Seconds 3
    $record.on=Send @{cmd='optional_reshade';action='state'}
    Require ($record.on.enabled-and$record.on.activeTechniques-gt0) 'Live on toggle has no active effect'
    $record.onPerformance=Send @{cmd='frame_performance'}
    $startCapture=Get-Date;$count=$record.on.captureCount
    $null=Send @{cmd='optional_reshade';action='capture'}
    $deadline=[DateTime]::UtcNow.AddSeconds(30)
    do{
     Start-Sleep -Milliseconds 250;$state=Send @{cmd='optional_reshade';action='state'}
     Require ([DateTime]::UtcNow-lt$deadline) 'Capture deadline'
    }while($state.captureCount-le$count)
    $files=@(Get-ChildItem (Join-Path $RuntimeDirectory 'Screenshots') -File |
     Where-Object {$_.LastWriteTime-ge$startCapture.AddSeconds(-1)})
    Require (@($files|Where-Object Name -Match 'Native').Count-gt0) 'Native final-frame capture missing'
    Require (@($files|Where-Object Name -Match 'ReShade').Count-gt0) 'ReShade final-frame capture missing'
    foreach($file in $files){Copy-Item -LiteralPath $file.FullName -Destination $arm}
    $record.captures=@($files|ForEach-Object {Join-Path $arm $_.Name});$record.final=$state
    Copy-Item -LiteralPath (Join-Path $RuntimeDirectory 'ReShade.log') -Destination $arm
    Require (!(Select-String -Path (Join-Path $arm 'ReShade.log') -Pattern 'Failed to compile|error [XC][0-9]|Failed to create effect' -Quiet)) 'ReShade effect error'
   }
   $record.performance=Send @{cmd='frame_performance'};$receipt.arms+=$record
   $null=Send @{cmd='exit'};$null=$p.WaitForExit(10000)
   Require $p.HasExited 'Clean exit deadline'
   Require (!(Select-String -Path @($log,(Join-Path $arm 'stderr.log')) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error:' -Quiet)) 'Renderer runtime error'
  }finally{
   if($p-and!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}
   if($client){$client.Dispose()}
  }
 }
 Require ((Get-Content (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Build changed during comparison'
 $receipt.passed=$true;Write-Host ('Optional ReShade acceptance PASS: '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
 $receipt|ConvertTo-Json -Depth 15|Set-Content (Join-Path $out 'result.json')
}
