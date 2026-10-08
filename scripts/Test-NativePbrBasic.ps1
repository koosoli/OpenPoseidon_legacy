<#
.SYNOPSIS
  Fixed-camera A/B for the optional native opaque PBR preview, using owned data.
.DESCRIPTION
  Run under scripts/with-game-lock.sh. Captures four identical synthetic spheres
  whose only difference is authored RoughnessScale/MetalnessScale. Preview=0 is
  the legacy control; Preview=1 must produce an eligible-material log. This tests
  our channel contract, not pixel parity with Reforger's renderer.
#>
param(
 [string]$GameDir='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault',
 [string]$Python='python',
 [string]$Output='',
 [switch]$PrepareOnly
)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
if(!$Output){$Output=Join-Path $repo ('build/native-pbr-basic/'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$Output=[IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$fixture=& $Python (Join-Path $PSScriptRoot 'generate-native-pbr-fixture.py') (Join-Path $Output 'fixture') | ConvertFrom-Json
if($LASTEXITCODE-ne0){throw 'Synthetic fixture generation failed'}
if($PrepareOnly){$fixture;return}
if(!$env:LOCK_OWNER){throw 'Run through scripts/with-game-lock.sh'}
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Preserving existing game; close it before this test'}
$stamp=(Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw).Trim()
$receipt=@{passed=$false;installed=$stamp;fixture=$fixture;arms=@();scope='Owned native opaque Basic preview contract; not native-reference parity'}
$keys=@('WGR_REFORGER_PBR_PREVIEW','WGR_REFORGER_PBR_PROVENANCE','POSEIDON_USER_DIR','POSEIDON_WHOLE_WORLD','WGR_GRASS','POSEIDON_VSYNC','WGR_GPU_DRIVEN','WGR_AUTO_EXPOSURE','WGR_EXPOSURE')
$saved=@{};foreach($key in $keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
try{
 foreach($preview in 0,1){
  $arm=Join-Path $Output ('preview-'+$preview);New-Item -ItemType Directory -Path $arm -Force | Out-Null
  # Recreate the same clean graphics profile for each arm; don't write owner's settings.
  $user=Join-Path $arm 'user';New-Item -ItemType Directory -Path $user -Force | Out-Null
  & (Join-Path $PSScriptRoot 'Write-BenchmarkGraphics.ps1') -UserDir $user -DlssMode 0
  $env:POSEIDON_USER_DIR=$user;$env:WGR_REFORGER_PBR_PREVIEW=[string]$preview
  Remove-Item Env:WGR_REFORGER_PBR_PROVENANCE -ErrorAction SilentlyContinue
  $env:POSEIDON_WHOLE_WORLD='0';$env:WGR_GRASS='0';$env:POSEIDON_VSYNC='0'
  $env:WGR_GPU_DRIVEN='1';$env:WGR_AUTO_EXPOSURE='0';$env:WGR_EXPOSURE='0.5'
  $reserve=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$reserve.Start()
  $port=$reserve.LocalEndpoint.Port;$reserve.Stop()
  $log=Join-Path $arm 'game.log';$client=$null;$p=$null;$writer=$null
  try{
   $argv=@('--render=wgpu','--dev','--window','--width','1600','--height','900','--vd','1000',
    '--test-mission',$fixture.mission,'--test-world',$fixture.world,
    '--test-world-pos','6400','6400','--test-world-yaw','0','--test-world-hour','16',
    '--test-model',$fixture.model,'--test-model-distance','15',
    '--harness',[string]$port,'--log-file',$log,'--log-level','info')
   $quoted=($argv|ForEach-Object {'"'+$_+'"'}) -join ' '
   $p=Start-Process -FilePath (Join-Path $GameDir 'OpenPoseidon.exe') -ArgumentList $quoted -WorkingDirectory $GameDir -PassThru `
    -RedirectStandardOutput (Join-Path $arm 'stdout.log') -RedirectStandardError (Join-Path $arm 'stderr.log')
   $deadline=[DateTime]::UtcNow.AddSeconds(120)
   do{
    if($p.HasExited){throw 'Game exited before harness connected'}
    try{$client=[Net.Sockets.TcpClient]::new('127.0.0.1',$port)}catch{$client=$null;Start-Sleep -Milliseconds 250}
    if([DateTime]::UtcNow-gt$deadline){throw 'Harness connection deadline'}
   }while(!$client)
   $client.ReceiveTimeout=30000;$stream=$client.GetStream()
   $writer=[IO.StreamWriter]::new($stream);$writer.AutoFlush=$true;$reader=[IO.StreamReader]::new($stream)
   function Send($command){
    $writer.WriteLine(($command|ConvertTo-Json -Compress -Depth 8))
    do{$line=$reader.ReadLine();if($null-eq$line){throw 'Harness disconnected'};$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok)
    if(!$reply.ok){throw ('Harness command failed: '+$line)};return $reply
   }
   do{
    $ready=Send @{cmd='eval';code='triSceneReady'}
    if(([string]$ready.result).Trim('"')-eq'OK'){break}
    if([DateTime]::UtcNow-gt$deadline){throw 'Scene readiness deadline'}
    Start-Sleep -Milliseconds 250
   }while($true)
   $null=Send @{cmd='exec';code='player allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0;setDate [1985,6,21,16,0]'}
   Start-Sleep -Seconds 4
   if(!(Select-String -LiteralPath $log -Pattern '\-\-test-model: placed ' -Quiet)){throw 'Synthetic model did not load'}
   # --test-model places relative to the player, not a freefly CameraEffect.
   # Read its actual pose, then center both captures on that exact fixed object.
   $position=Select-String -LiteralPath $log -Pattern '--test-model: object pos ([\d.+eE-]+) ([\d.+eE-]+) ([\d.+eE-]+)' | Select-Object -Last 1
   if(!$position){throw 'Synthetic model position was not logged'}
   $number=[Globalization.CultureInfo]::InvariantCulture
   $x=[double]::Parse($position.Matches[0].Groups[1].Value,$number)
   $y=[double]::Parse($position.Matches[0].Groups[2].Value,$number)
   $z=[double]::Parse($position.Matches[0].Groups[3].Value,$number)-15
   $pose=($x.ToString($number),$z.ToString($number),$y.ToString($number),'0','0') -join ' '
   $camera=Send @{cmd='eval';code=('triFreeFlyPose "'+$pose+'"')}
   if(([string]$camera.result).Trim('"')-cne'OK'){throw 'Fixed camera was refused'}
   Start-Sleep -Seconds 2
   if($preview-eq1-and!(Select-String -LiteralPath $log -Pattern 'Wgpu Reforger PBR preview: first eligible opaque section' -Quiet)){
    throw 'Native Basic preview never became eligible; capture is not a valid scalar test'
   }
   if($preview-eq0-and(Select-String -LiteralPath $log -Pattern 'Wgpu Reforger PBR preview: first eligible opaque section' -Quiet)){
    throw 'Legacy control unexpectedly enabled native preview'
   }
   $image=Join-Path $arm 'fixed-camera.png';$null=Send @{cmd='screenshot';path=$image}
   $receipt.arms+=@{preview=$preview;camera=$pose;screenshot=$image;performance=(Send @{cmd='frame_performance'});log=$log}
   $null=Send @{cmd='exit'};$null=$p.WaitForExit(10000)
   if(!$p.HasExited){throw 'Game did not exit cleanly'}
   if(Select-String -LiteralPath @($log,(Join-Path $arm 'stderr.log')) -Pattern 'Validation Error|DeviceLost|panicked at|wgr_create failed|UNHANDLED EXCEPTION' -Quiet){throw 'Renderer error'}
  }finally{
   if($p-and!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)} # Exact process launched by this test only.
   if($client){$client.Dispose()}
  }
 }
 if((Get-Content -LiteralPath (Join-Path $GameDir 'DEPLOYED-FROM.txt') -Raw).Trim()-cne$stamp){throw 'Installed build changed during A/B'}
 $receipt.passed=$true;Write-Host ('Native PBR Basic A/B captured: '+$Output)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 foreach($key in $keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
 $receipt|ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $Output 'result.json')
}
