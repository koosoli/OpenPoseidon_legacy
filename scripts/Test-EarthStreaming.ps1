<# Installed-game acceptance: actual freefly crosses rolling-window boundaries,
   returns to the start, validates the upload receipts, captures rendered frames.
   Wrap in scripts/with-game-lock.sh. Stops only the process this script starts.
   -Coast adds Guernsey at 1000m, looking southeast from local [8000,4800],
   then 9000m to check near-water retirement. Sea appearance needs image inspection. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$ExpectedCommit,[switch]$Offline,[switch]$Horizon,[switch]$Globe,[switch]$Controls,[switch]$Coast)
$ErrorActionPreference='Stop'
if(!$env:LOCK_OWNER){throw 'Game lock required'}
$repo=Split-Path -Parent $PSScriptRoot
$game='D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
if(Get-Process OpenPoseidon,ColdWarAssault -ErrorAction SilentlyContinue){throw 'Existing game preserved'}
$stamp=(Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()
if(!$stamp.Contains($ExpectedCommit)){throw 'Installed source differs'}
$out=Join-Path $repo ('build/earth-streaming/'+(Get-Date -Format yyyyMMdd-HHmmss))
$null=New-Item -ItemType Directory -Force -Path $out
$cache=Join-Path $env:LOCALAPPDATA 'OpenPoseidon/world-stream'
$world=& "$PSScriptRoot/Launch-WholeWorld.ps1" -PrepareOnly -CacheDirectory $cache
$mission=Join-Path $cache 'fixture/earth-preview.abel'
$settings=@{POSEIDON_WHOLE_WORLD='1';POSEIDON_EARTH_LATITUDE='46.6';POSEIDON_EARTH_LONGITUDE='8.1';POSEIDON_EARTH_CACHE=$cache;WGR_GRASS='0';WGR_TERRAIN_RESOURCE_REUSE='1';WGR_WET_SOIL_DIAGNOSTIC='1';POSEIDON_USER_DIR=(Join-Path $out 'user')}
$settings.POSEIDON_EARTH_IMAGERY=Join-Path $cache 'blue-marble-june-5400.jpg'
if($Coast){$settings.WGR_WATER_BACKEND='1';$settings.WGR_TW_WAKE_STATS='1'}
if($Offline){$settings.HTTPS_PROXY='http://127.0.0.1:9';$settings.HTTP_PROXY='http://127.0.0.1:9';$settings.ALL_PROXY='http://127.0.0.1:9';$settings.NO_PROXY=''}
$saved=@{};foreach($key in $settings.Keys){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
$null=New-Item -ItemType Directory -Force -Path $env:POSEIDON_USER_DIR
[IO.File]::WriteAllText((Join-Path $env:POSEIDON_USER_DIR 'graphics.cfg'),"qualityPreset=3;`nmsaaSamples=4;`ndlssMode=0;`nvsync=0;`nfpsCap=60;`n")
$receipt=@{installed=$stamp;offline=[bool]$Offline;views=@();passed=$false};$p=$null;$client=$null;$writer=$null
function Require($ok,$message){if(!$ok){throw $message}}
function Send($cmd){$writer.WriteLine(($cmd|ConvertTo-Json -Depth 10 -Compress));do{$line=$reader.ReadLine();Require $line 'Harness disconnected';$reply=$line|ConvertFrom-Json}while($null-eq$reply.ok);Require $reply.ok $line;return $reply}
function CoastEval([string]$code){
 $value=([string](Send @{cmd='eval';code=$code}).result).Trim()
 if($value.StartsWith('"')){return $value.Substring(1,$value.Length-2).Replace('""','"')}
 return $value
}
function CoastGround([double]$x,[double]$z){
 $culture=[Globalization.CultureInfo]::InvariantCulture
 return (CoastEval ('triEarthGround ['+$x.ToString('R',$culture)+','+$z.ToString('R',$culture)+']'))|ConvertFrom-Json
}
function CoastPatch([uint64]$revision){
 $deadline=[DateTime]::UtcNow.AddSeconds(150)
 do{
  Require (!$p.HasExited) 'Owned game exited during coast publication'
  $g=CoastGround 8000 4800
  Require ($g.revision-eq$revision) 'Coast ground belongs to a superseded geographic origin'
  if($g.covered -and [Math]::Abs(($g.bounds[0]+$g.bounds[2])/2-8000)-lt.1 -and [Math]::Abs(($g.bounds[1]+$g.bounds[3])/2-4800)-lt.1){
   $patch=@(Select-String -LiteralPath $log -Pattern ('Earth terrain patch '+$g.generation+': .*revision '+$revision+'$'))
   if($patch.Count){
    $horizon=@(Select-String -LiteralPath $log -Pattern 'Earth horizon: ' | Where-Object{$_.LineNumber-gt$patch[-1].LineNumber -and $_.LineNumber-le$patch[-1].LineNumber+5})
    if($horizon.Count){return @{ground=$g;upload=$patch[-1].Line;horizon=$horizon[0].Line}}
   }
  }
  Require ([DateTime]::UtcNow-lt$deadline) 'Coast current-revision ground/horizon publication deadline'
  Start-Sleep -Milliseconds 250
 }while($true)
}
function CoastWaterActivity([bool]$active){
 # Published timings can lag. Require completed frames and three consecutive
 # agreeing samples; negative means no bracket, never zero GPU cost.
 $start=[double](CoastEval 'triFrameCount');$deadline=[DateTime]::UtcNow.AddSeconds(20);$stable=0
 do{
  Require (!$p.HasExited) 'Owned game exited during coast water check'
  Start-Sleep -Milliseconds 300;$perf=Send @{cmd='frame_performance'}
  $fft=@($perf.regions|Where-Object{$_.name-ceq'Tidewater: FFT + mips'})
  if(!$perf.gpuTimestampsAvailable -or !$fft.Count){return @{verified=$false;reason='GPU timestamps/Tidewater FFT row unavailable';performance=$perf}}
  if([double](CoastEval 'triFrameCount')-ge$start+12){
   if(([double]$fft[0].gpuMs-ge0)-eq$active){$stable++}else{$stable=0}
   if($stable-ge3){return @{verified=$true;expectedActive=$active;fft=$fft[0];performance=$perf}}
  }
  Require ([DateTime]::UtcNow-lt$deadline) ('Tidewater FFT activity did not match near-water band: active='+$active)
 }while($true)
}
function CoastCapture([string]$label,[double]$height){
 $camera=(Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera
 Require ($camera.position.Count-eq3 -and [Math]::Abs($camera.position[1]-$height)-lt.2) 'Actual coast render-camera height differs'
 Require ([Math]::Abs($camera.position[0]-8000)-lt.2 -and [Math]::Abs($camera.position[2]-4800)-lt.2) 'Actual coast render-camera X/Z differs'
 Require ($camera.direction.Count-eq3 -and [Math]::Abs($camera.direction[0]-.664463)-lt.01 -and [Math]::Abs($camera.direction[1]+.342020)-lt.01 -and [Math]::Abs($camera.direction[2]+.664463)-lt.01) 'Actual coast camera is not looking southeast/down20 degrees'
 $path=Join-Path $out ($label+'.png');$null=Send @{cmd='screenshot';path=$path}
 $deadline=[DateTime]::UtcNow.AddSeconds(15)
 while(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length-eq0){Require (!$p.HasExited -and [DateTime]::UtcNow-lt$deadline) 'Coast screenshot deadline';Start-Sleep -Milliseconds 100}
 return @{camera=$camera;screenshot=$path}
}
try{
 $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start();$port=$listener.LocalEndpoint.Port;$listener.Stop()
 $log=Join-Path $out 'engine.log'
 $argv=@('--render=wgpu','--window','--dev','--width','1280','--height','720','--vd','20000','--harness',"$port",'--test-mission',('"'+$mission+'"'),'--test-world',('"'+$world+'"'),'--test-world-freefly','6400','6400','4500','30','-20','--test-world-hour','12','--log-file',('"'+$log+'"'))
 $p=Start-Process (Join-Path $game 'OpenPoseidon.exe') -WorkingDirectory $game -WindowStyle Hidden -ArgumentList $argv -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError (Join-Path $out 'stderr.log') -PassThru
 $deadline=[DateTime]::UtcNow.AddSeconds(150)
 do{Require (!$p.HasExited) 'Startup exited';$client=[Net.Sockets.TcpClient]::new();try{$client.Connect('127.0.0.1',$port)}catch{$client.Dispose();$client=$null};if(!$client){Require ([DateTime]::UtcNow-lt$deadline) 'Startup deadline';Start-Sleep -Milliseconds 250}}while(!$client)
 $stream=$client.GetStream();$stream.ReadTimeout=30000;$reader=[IO.StreamReader]::new($stream);$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
 while(([string](Send @{cmd='eval';code='triSceneReady'}).result).Trim('"')-cne'OK'){Require ([DateTime]::UtcNow-lt$deadline) 'Scene-ready deadline';Start-Sleep -Milliseconds 250}
 Require (!(Select-String -Path $log -Pattern 'Cannot load --test-world|Critical error' -Quiet)) 'Test world failed to load'
 $null=Send @{cmd='exec';code='player allowDamage false;0 setRain 0;0 setOvercast .15;0 setFog 0;setDate [1985,6,21,12,0]'}
 foreach($x in @(6400,9600,16000,32000,6400)){
  $before=@(Select-String -Path $log -Pattern 'Earth terrain patch ').Count
  $null=Send @{cmd='exec';code=('triFreeFlyPose "'+$x+' 6400 4500 30 -20"')}
  $origin=$x-3200 # 4500m selects the 25m/z12 detail band.
  $pattern='origin \['+$origin+',3200\]'
  $deadline=[DateTime]::UtcNow.AddSeconds(90)
  do{
   Require (!$p.HasExited) 'Game exited during streaming'
   $rows=@(Select-String -Path $log -Pattern 'Earth terrain patch ')
   $last=if($rows.Count){$rows[-1].Line}else{''}
   if($last -match $pattern -and ($x-eq6400 -and $receipt.views.Count-eq0 -or $rows.Count-gt$before)){break}
   Require ([DateTime]::UtcNow-lt$deadline) ('Patch deadline at x='+$x+'; last='+$last)
   Start-Sleep -Milliseconds 300
  }while($true)
  Start-Sleep -Milliseconds 500
  $camera=Send @{cmd='dev_wet_soil_diagnostic';action='state'}
  Require ([Math]::Abs($camera.camera.position[0]-$x)-lt.1) 'Actual freefly position differs'
  $path=Join-Path $out ('earth-'+$receipt.views.Count+'.png');$null=Send @{cmd='screenshot';path=$path}
  $receipt.views+=@{x=$x;upload=$last;screenshot=$path;performance=(Send @{cmd='frame_performance'})}
 }
 if($Horizon){
  foreach($altitude in @(30000,120000,4500)){
   $before=@(Select-String -Path $log -Pattern 'Earth horizon: ').Count
   $null=Send @{cmd='exec';code=('triFreeFlyPose "6400 6400 '+$altitude+' 30 -20"')}
   $deadline=[DateTime]::UtcNow.AddSeconds(150)
   do{
    Require (!$p.HasExited) 'Game exited during altitude change'
    $rows=@(Select-String -Path $log -Pattern 'Earth horizon: ')
    if($rows.Count-gt$before){break}
    Require ([DateTime]::UtcNow-lt$deadline) ('Horizon deadline at altitude='+$altitude)
    Start-Sleep -Milliseconds 300
   }while($true)
   Start-Sleep -Milliseconds 700
   $path=Join-Path $out ('horizon-'+$altitude+'.png');$null=Send @{cmd='screenshot';path=$path}
   $receipt.views+=@{altitude=$altitude;horizon=$rows[-1].Line;screenshot=$path;performance=(Send @{cmd='frame_performance'})}
  }
 }
 if($Globe){
  Require (Test-Path -LiteralPath $settings.POSEIDON_EARTH_IMAGERY) 'Globe imagery cache required; launch preview once online'
  $null=Send @{cmd='exec';code='0 setOvercast 0'}
  $go=Send @{cmd='eval';code='triEarthLocation [46.6,8.1,18000000]'}
  Require ($go.result-match'OK:latitude=') 'Orbital geographic Go was rejected'
  Start-Sleep -Seconds 2
  $camera=Send @{cmd='dev_wet_soil_diagnostic';action='state'}
  Require ([Math]::Abs($camera.camera.position[1]-18000000)-lt10) 'Actual orbital camera height differs'
  Require (Select-String -Path $log -Pattern 'Earth globe: .*imagery local NASA Blue Marble' -Quiet) 'Globe texture not uploaded'
  $path=Join-Path $out 'globe.png';$null=Send @{cmd='screenshot';path=$path}
  $receipt.views+=@{altitude=18000000;screenshot=$path;performance=(Send @{cmd='frame_performance'})}
  $null=Send @{cmd='exec';code='triFreeFlyPose "6400 6400 4500 30 -20"'}
  Start-Sleep -Seconds 2
  $path=Join-Path $out 'return-from-orbit.png';$null=Send @{cmd='screenshot';path=$path}
  $receipt.views+=@{altitude=4500;screenshot=$path;performance=(Send @{cmd='frame_performance'})}
 }
 if($Controls){
  # Drive the actual Freefly input path, not another scripted teleport. Commands
  # enter this owned game's SDL queue; no desktop focus or human keys are used.
  $null=Send @{cmd='exec';code='triFreeFlyPose "6400 6400 12000000 0 -89.9"'}
  Start-Sleep -Milliseconds 500
  $before=(Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera.position
  try{$null=Send @{cmd='key';sc=26;hold=$true};Start-Sleep -Seconds 2}
  finally{$null=Send @{cmd='key_up';sc=26}}
  Start-Sleep -Milliseconds 300
  $approach=(Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera.position
  Require ($approach[1]-lt$before[1]-10000) 'Looking down and holding W did not approach Earth'
  Require ([Math]::Abs($approach[0]-$before[0])-lt1000) 'Approach unexpectedly moved sideways'
  try{$null=Send @{cmd='key';sc=22;hold=$true};Start-Sleep -Seconds 2}
  finally{$null=Send @{cmd='key_up';sc=22}}
  Start-Sleep -Milliseconds 300
  $retreat=(Send @{cmd='dev_wet_soil_diagnostic';action='state'}).camera.position
  $receipt.controls=@{before=$before;approach=$approach;retreat=$retreat;input='SDL W/S held for 2 seconds each'}
  Require ($retreat[1]-gt$approach[1]+10000) 'Looking down and holding S did not retreat'
 }
 if($Coast){
  $receipt.coast=@{setup='Origin49.45,-2.55; camera engineXYZ[8000,1000,4800], azimuth135, elevation-20; retirement view at9000m, same X/Z and orientation.';scope='Guernsey resident near-water presentation only. Inspect coastline blend, waves and near/far sea join in captures; this does not prove shoreline physics, bathymetry or water pixels.'}
  Require ((CoastEval 'triEarthLocation [49.45,-2.55,1000]').StartsWith('OK:')) 'Coast geographic Go refused'
  $location=CoastEval 'triEarthLocation []';Require ($location-match'revision=([0-9]+)$') 'Coast origin query malformed';$revision=[uint64]$Matches[1]
  Require ((CoastEval 'triFreeFlyPose "8000 4800 1000 135 -20"')-ceq'OK') 'Coast camera pose refused'
  $receipt.coast.location=$location;$receipt.coast.patch=CoastPatch $revision
  $receipt.coast.waterSettings=Send @{cmd='water_probe'}
  $receipt.coast.nearActivity=CoastWaterActivity $true
  Start-Sleep -Milliseconds 500;$receipt.coast.overview=CoastCapture 'coast-1000m' 1000
  # A bounded 9x9 resident sample grid seeks plausible sea and nearby land.
  # Terrarium's zero coast elevation is not bathymetry or a coastline oracle.
  $samples=@();foreach($dz in -4..4){foreach($dx in -4..4){
   $x=8000+$dx*400;$z=4800+$dz*400;$g=CoastGround $x $z
   Require ($g.revision-eq$revision) 'Coast sample retained stale ground'
   if($g.covered){$samples+=@{x=$x;z=$z;height=$g.height;generation=$g.generation}}
  }}
  $receipt.coast.groundSamples=$samples
  $sea=@($samples|Where-Object{$_.height-le2 -and $_.height-ge-5}|Sort-Object height)
  $land=@($samples|Where-Object{$_.height-gt5})
  $receipt.coast.coverage=@{nearZeroCandidates=$sea.Count;elevatedLandSamples=$land.Count;classification='Height candidates only; rendered sea and coastline require inspection.'}
  if(!$sea.Count -or !$land.Count){$receipt.coast.coverage.note='Bounded grid did not resolve both plausible sea and elevated land; sea blending is unvalidated. No extra requests or camera overrides were attempted.'}
  Require ((CoastEval 'triFreeFlyPose "8000 4800 9000 135 -20"')-ceq'OK') 'Above-water-band camera refused'
  $receipt.coast.retiredActivity=CoastWaterActivity $false
  $receipt.coast.above=CoastCapture 'coast-above-water-band-9000m' 9000
  $receipt.coast.waterLog=@(Select-String -LiteralPath $log -Pattern 'TW-WAKE GPU ms'|ForEach-Object{$_.Line})
  $receipt.coast.diagnosticsVerified=$receipt.coast.nearActivity.verified -and $receipt.coast.retiredActivity.verified
  $null=CoastEval 'triFreeFlyPose "6400 6400 4500 30 -20"'
 }
 $receipt.tiles=@(Get-ChildItem $cache -Directory -Filter 'terrarium-z*' | Get-ChildItem -Filter '*.png').Count
 Require ($receipt.tiles-le256) 'Disk cache exceeds its bound'
 if($Offline){
  Require (!(Select-String -Path $log -Pattern 'downloads [1-9]' -Quiet)) 'Offline traversal unexpectedly downloaded data'
  $rows=@(Select-String -Path $log -Pattern 'Earth terrain patch ').Count
  $null=Send @{cmd='exec';code='triFreeFlyPose "100000 6400 4500 30 -20"'}
  $deadline=[DateTime]::UtcNow.AddSeconds(20)
  do{
   $failed=Select-String -Path (Join-Path $out 'stderr.log') -Pattern 'retaining last completed patch' -Quiet
   if($failed){break}
   Require ([DateTime]::UtcNow-lt$deadline) 'Offline missing-tile failure not observed'
   Start-Sleep -Milliseconds 250
  }while($true)
  Require (@(Select-String -Path $log -Pattern 'Earth terrain patch ').Count-eq$rows) 'Incomplete/offline window was published'
  $receipt.offlineFailureObserved=$true
  $receipt.afterFailure=Send @{cmd='frame_performance'}
 }
 Require (!(Select-String -Path @($log,(Join-Path $out 'stderr.log')) -Pattern 'UNHANDLED EXCEPTION|Validation Error|DeviceLost|panicked at|wgr_create failed|Critical error' -Quiet)) 'Runtime error'
 Require ((Get-Content (Join-Path $game 'DEPLOYED-FROM.txt') -Raw).Trim()-ceq$stamp) 'Installed build changed'
 $receipt.passed=$true
 Write-Host ('Earth streaming acceptance PASS: '+$out)
}catch{$receipt.error=$_.Exception.Message;throw}finally{
 if($p-and!$p.HasExited){if($writer){try{$writer.WriteLine('{"cmd":"exit"}');$null=$p.WaitForExit(10000)}catch{}};if(!$p.HasExited){$p.Kill();$null=$p.WaitForExit(10000)}}
 if($client){$client.Dispose()}
 foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}
 $receipt|ConvertTo-Json -Depth 15|Set-Content (Join-Path $out 'result.json')
}
